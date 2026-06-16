// SPDX-License-Identifier: MIT
//
// Morok — modular LLVM IR obfuscator.
//
// morok/passes/StateOpaquePredicates.cpp
//
// Stateful MBA opaque predicates.  Selected post-flattening blocks are split
// before their first real instruction.  The guard compares an MBA expression
// over the current `fla.state` with its reconstructed value; the cancellation
// term is routed through volatile shadow memory, and the token is mixed with
// live scalar integer/FP values.  Runtime semantics are unchanged because the
// false edge is opaque-dead, but the predicate is not a pure algebraic identity.

#include "morok/passes/StateOpaquePredicates.hpp"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

using namespace llvm;

namespace morok::passes {

namespace {

constexpr char kShadowPrefix[] = "morok.stateop.shadow";
constexpr char kFalsePrefix[] = "morok.stateop.false";

bool isGeneratedBlock(const BasicBlock &BB) {
    return BB.getName().starts_with("morok.stateop") ||
           BB.getName().starts_with("morok.ifsm") ||
           BB.getName().starts_with("morok.path");
}

AllocaInst *findState(Function &F) {
    for (Instruction &I : F.getEntryBlock())
        if (auto *AI = dyn_cast<AllocaInst>(&I))
            if (AI->getName().starts_with("fla.state"))
                return AI;
    return nullptr;
}

AllocaInst *findShadow(Function &F) {
    for (Instruction &I : F.getEntryBlock())
        if (auto *AI = dyn_cast<AllocaInst>(&I))
            if (AI->getName().starts_with(kShadowPrefix))
                return AI;
    return nullptr;
}

AllocaInst *ensureShadow(Function &F) {
    if (AllocaInst *Existing = findShadow(F))
        return Existing;
    IRBuilder<> B(&*F.getEntryBlock().getFirstInsertionPt());
    auto *I32 = Type::getInt32Ty(F.getContext());
    auto *ShadowTy = ArrayType::get(I32, 2);
    auto *Shadow = B.CreateAlloca(ShadowTy, nullptr, kShadowPrefix);
    Shadow->setAlignment(Align(4));
    return Shadow;
}

Instruction *splitPoint(BasicBlock &BB) {
    for (Instruction &I : BB) {
        if (isa<PHINode>(&I) || isa<AllocaInst>(&I))
            continue;
        if (I.isTerminator())
            return nullptr;
        return &I;
    }
    return nullptr;
}

bool supportedFloatType(Type *Ty) {
    return Ty->isHalfTy() || Ty->isBFloatTy() || Ty->isFloatTy() ||
           Ty->isDoubleTy();
}

IntegerType *integerCarrierFor(Type *Ty) {
    if (Ty->isHalfTy() || Ty->isBFloatTy())
        return IntegerType::get(Ty->getContext(), 16);
    if (Ty->isFloatTy())
        return IntegerType::get(Ty->getContext(), 32);
    if (Ty->isDoubleTy())
        return IntegerType::get(Ty->getContext(), 64);
    return nullptr;
}

void addTerm(Value *V, SmallPtrSetImpl<Value *> &Seen,
             std::vector<Value *> &Terms) {
    if (!V ||
        (!V->getType()->isIntegerTy() && !supportedFloatType(V->getType())))
        return;
    if (Seen.insert(V).second)
        Terms.push_back(V);
}

std::vector<Value *> collectTerms(Function &F, BasicBlock &BB,
                                  Instruction *Stop) {
    std::vector<Value *> Terms;
    SmallPtrSet<Value *, 32> Seen;
    for (Argument &Arg : F.args())
        addTerm(&Arg, Seen, Terms);
    for (Instruction &I : BB) {
        if (&I == Stop)
            break;
        if (isa<AllocaInst>(&I))
            continue;
        addTerm(&I, Seen, Terms);
    }
    return Terms;
}

void shuffleTerms(std::vector<Value *> &Terms, ir::IRRandom &rng) {
    for (std::size_t I = Terms.size(); I > 1; --I) {
        const std::size_t J = rng.range(static_cast<std::uint32_t>(I));
        std::swap(Terms[I - 1], Terms[J]);
    }
}

Value *asI32(IRBuilder<NoFolder> &B, Value *V) {
    auto *I32 = B.getInt32Ty();
    if (V->getType() == I32)
        return V;
    if (auto *IT = dyn_cast<IntegerType>(V->getType())) {
        const unsigned Bits = IT->getBitWidth();
        if (Bits < 32)
            return B.CreateZExt(V, I32, "morok.stateop.term.zext");
        if (Bits > 32)
            return B.CreateTrunc(V, I32, "morok.stateop.term.trunc");
    }
    if (supportedFloatType(V->getType())) {
        Value *Bits = B.CreateBitCast(V, integerCarrierFor(V->getType()),
                                      "morok.stateop.term.fp");
        auto *CarrierTy = cast<IntegerType>(Bits->getType());
        if (CarrierTy->getBitWidth() < 32)
            return B.CreateZExt(Bits, I32, "morok.stateop.term.zext");
        if (CarrierTy->getBitWidth() > 32)
            return B.CreateTrunc(Bits, I32, "morok.stateop.term.trunc");
        return Bits;
    }
    return nullptr;
}

Value *slot(IRBuilder<NoFolder> &B, AllocaInst *Shadow, unsigned Index,
            const Twine &Name) {
    auto *I32 = B.getInt32Ty();
    return B.CreateInBoundsGEP(
        Shadow->getAllocatedType(), Shadow,
        {ConstantInt::get(I32, 0), ConstantInt::get(I32, Index)}, Name);
}

Value *volatileCancel(IRBuilder<NoFolder> &B, AllocaInst *Shadow,
                      Value *Token) {
    auto *I32 = B.getInt32Ty();
    Value *S0 = slot(B, Shadow, 0, "morok.stateop.shadow.slot0");
    Value *S1 = slot(B, Shadow, 1, "morok.stateop.shadow.slot1");
    auto *Store0 = B.CreateStore(Token, S0);
    Store0->setVolatile(true);
    Store0->setAlignment(Align(4));
    auto *Store1 = B.CreateStore(Token, S1);
    Store1->setVolatile(true);
    Store1->setAlignment(Align(4));
    auto *A = B.CreateLoad(I32, S0, "morok.stateop.shadow.a");
    A->setVolatile(true);
    A->setAlignment(Align(4));
    auto *Bv = B.CreateLoad(I32, S1, "morok.stateop.shadow.b");
    Bv->setVolatile(true);
    Bv->setAlignment(Align(4));
    return B.CreateXor(A, Bv, "morok.stateop.cancel");
}

Value *buildToken(IRBuilder<NoFolder> &B, Value *State,
                  std::vector<Value *> Terms, const StateOpParams &Params,
                  ir::IRRandom &rng) {
    auto *I32 = B.getInt32Ty();
    Value *Token = B.CreateXor(
        State, ConstantInt::get(I32, static_cast<std::uint32_t>(rng.next())),
        "morok.stateop.token.seed");
    shuffleTerms(Terms, rng);
    const std::uint32_t Limit = std::min<std::uint32_t>(
        Params.max_terms, static_cast<std::uint32_t>(Terms.size()));
    for (std::uint32_t I = 0; I < Limit; ++I) {
        Value *Term = asI32(B, Terms[I]);
        if (!Term)
            continue;
        Value *Salt =
            ConstantInt::get(I32, static_cast<std::uint32_t>(rng.next()));
        Value *Odd =
            ConstantInt::get(I32, static_cast<std::uint32_t>(rng.next()) | 1u);
        Token = B.CreateMul(
            B.CreateXor(Token, B.CreateXor(Term, Salt),
                        "morok.stateop.token.fold"),
            Odd, "morok.stateop.token.mix");
    }
    return Token;
}

Value *buildPredicate(IRBuilder<NoFolder> &B, AllocaInst *StateVar,
                      AllocaInst *Shadow, std::vector<Value *> Terms,
                      const StateOpParams &Params, ir::IRRandom &rng) {
    auto *I32 = B.getInt32Ty();
    Value *State = B.CreateLoad(I32, StateVar, "morok.stateop.state");
    Value *Token = buildToken(B, State, std::move(Terms), Params, rng);
    Value *Cancel = volatileCancel(B, Shadow, Token);
    Value *TokenShadow =
        B.CreateXor(Token, Cancel, "morok.stateop.mba.token.shadow");
    Value *Base = B.CreateXor(
        State, ConstantInt::get(I32, static_cast<std::uint32_t>(rng.next())),
        "morok.stateop.mba.base");
    Value *Key =
        ConstantInt::get(I32, static_cast<std::uint32_t>(rng.next()) | 1u);
    Value *Mixed =
        B.CreateXor(B.CreateAdd(Base, TokenShadow, "morok.stateop.mba.add"),
                    Key, "morok.stateop.mba.xor");
    Value *Restored =
        B.CreateSub(B.CreateXor(Mixed, Key, "morok.stateop.mba.unxor"),
                    TokenShadow, "morok.stateop.mba.restore");
    return B.CreateICmpEQ(Restored, Base, "morok.stateop.pred");
}

void buildFalseBlock(BasicBlock *FalseBB, BasicBlock *Body,
                     AllocaInst *Shadow, ir::IRRandom &rng) {
    IRBuilder<NoFolder> B(FalseBB);
    auto *I32 = B.getInt32Ty();
    Value *S0 = slot(B, Shadow, 0, "morok.stateop.false.slot0");
    Value *Noise =
        ConstantInt::get(I32, static_cast<std::uint32_t>(rng.next()));
    auto *Store = B.CreateStore(Noise, S0);
    Store->setVolatile(true);
    Store->setAlignment(Align(4));
    B.CreateBr(Body);
}

} // namespace

bool stateOpaquePredicatesFunction(Function &F, const StateOpParams &params,
                                   ir::IRRandom &rng) {
    if (F.isDeclaration() || F.getName().starts_with("morok.") ||
        params.probability == 0 || params.max_blocks == 0)
        return false;
    AllocaInst *State = findState(F);
    if (!State)
        return false;

    std::vector<std::pair<BasicBlock *, Instruction *>> Candidates;
    for (BasicBlock &BB : F) {
        if (&BB == &F.getEntryBlock() || BB.isEHPad() || BB.isLandingPad() ||
            isGeneratedBlock(BB))
            continue;
        if (Instruction *Pt = splitPoint(BB))
            Candidates.push_back({&BB, Pt});
    }
    for (std::size_t I = Candidates.size(); I > 1; --I) {
        const std::size_t J = rng.range(static_cast<std::uint32_t>(I));
        std::swap(Candidates[I - 1], Candidates[J]);
    }

    AllocaInst *Shadow = nullptr;
    bool Changed = false;
    std::uint32_t Count = 0;
    for (auto [Head, Pt] : Candidates) {
        if (Count >= params.max_blocks)
            break;
        if (!rng.chance(params.probability))
            continue;
        if (!Shadow)
            Shadow = ensureShadow(F);

        std::vector<Value *> Terms = collectTerms(F, *Head, Pt);
        BasicBlock *Body = SplitBlock(Head, Pt);
        Instruction *HeadTerm = Head->getTerminator();
        IRBuilder<NoFolder> B(HeadTerm);
        Value *Pred = buildPredicate(B, State, Shadow, std::move(Terms), params,
                                     rng);
        BasicBlock *FalseBB =
            BasicBlock::Create(F.getContext(), kFalsePrefix, &F, Body);
        buildFalseBlock(FalseBB, Body, Shadow, rng);
        B.CreateCondBr(Pred, Body, FalseBB);
        HeadTerm->eraseFromParent();
        Changed = true;
        ++Count;
    }

    return Changed;
}

PreservedAnalyses StateOpaquePredicatesPass::run(
    Function &F, FunctionAnalysisManager &) {
    if (F.isDeclaration())
        return PreservedAnalyses::all();
    ir::IRRandom rng(engine_);
    return stateOpaquePredicatesFunction(F, params_, rng)
               ? PreservedAnalyses::none()
               : PreservedAnalyses::all();
}

} // namespace morok::passes
