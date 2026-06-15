// SPDX-License-Identifier: MIT
//
// Morok — modular LLVM IR obfuscator.
//
// morok/passes/StackCoalescing.cpp
//
// Single-buffer stack coalescing: static local allocas become aligned offsets
// inside one [N x i8] frame object.  The replacement is intentionally scoped to
// allocas whose address does not escape; memory operations and GEPs are safe,
// but code that observes a stack address is left untouched.

#include "morok/passes/StackCoalescing.hpp"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

using namespace llvm;

namespace morok::passes {

namespace {

constexpr std::uint64_t kMaxCoalescedFrameBytes = 16ull * 1024ull * 1024ull;

struct Slot {
    AllocaInst *alloca = nullptr;
    std::uint64_t size = 0;
    Align align{1};
    std::uint64_t offset = 0;
    Value *replacement = nullptr;
};

bool addWouldOverflow(std::uint64_t a, std::uint64_t b) {
    return a > std::numeric_limits<std::uint64_t>::max() - b;
}

std::optional<std::uint64_t> alignUp(std::uint64_t value, Align align) {
    const std::uint64_t mask = align.value() - 1u;
    if (addWouldOverflow(value, mask))
        return std::nullopt;
    return (value + mask) & ~mask;
}

bool isAllowedIntrinsic(const IntrinsicInst &II) {
    switch (II.getIntrinsicID()) {
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::dbg_declare:
    case Intrinsic::dbg_value:
        return true;
    default:
        return false;
    }
}

bool isLifetimeIntrinsic(const IntrinsicInst &II) {
    switch (II.getIntrinsicID()) {
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
        return true;
    default:
        return false;
    }
}

bool hasOnlyLocalMemoryUses(Value *V, SmallPtrSetImpl<Value *> &seen) {
    if (!seen.insert(V).second)
        return true;

    for (User *U : V->users()) {
        if (auto *LI = dyn_cast<LoadInst>(U)) {
            if (LI->getPointerOperand() != V)
                return false;
            continue;
        }

        if (auto *SI = dyn_cast<StoreInst>(U)) {
            if (SI->getPointerOperand() != V)
                return false;
            continue;
        }

        if (auto *GEP = dyn_cast<GetElementPtrInst>(U)) {
            if (GEP->getPointerOperand() != V)
                return false;
            if (!hasOnlyLocalMemoryUses(GEP, seen))
                return false;
            continue;
        }

        if (auto *Cast = dyn_cast<CastInst>(U)) {
            if (!Cast->getType()->isPointerTy())
                return false;
            if (!hasOnlyLocalMemoryUses(Cast, seen))
                return false;
            continue;
        }

        if (isa<MemIntrinsic>(U))
            continue;

        if (auto *II = dyn_cast<IntrinsicInst>(U)) {
            if (isAllowedIntrinsic(*II))
                continue;
        }

        return false;
    }

    return true;
}

void collectLifetimeMarkers(Value *V, SmallPtrSetImpl<Value *> &seen,
                            SmallVectorImpl<Instruction *> &markers) {
    if (!seen.insert(V).second)
        return;

    for (User *U : V->users()) {
        if (auto *II = dyn_cast<IntrinsicInst>(U)) {
            if (isLifetimeIntrinsic(*II))
                markers.push_back(II);
            continue;
        }
        if (isa<GetElementPtrInst>(U) || isa<CastInst>(U))
            collectLifetimeMarkers(static_cast<Value *>(U), seen, markers);
    }
}

bool isEligibleAlloca(AllocaInst &AI, const DataLayout &DL) {
    if (AI.getAddressSpace() != 0)
        return false;
    if (!AI.isStaticAlloca() || AI.isUsedWithInAlloca() || AI.isSwiftError())
        return false;
    if (AI.getName().starts_with("morok.stack"))
        return false;

    const std::optional<TypeSize> size = AI.getAllocationSize(DL);
    if (!size.has_value() || size->isScalable() || size->getFixedValue() == 0)
        return false;

    SmallPtrSet<Value *, 8> seen;
    return hasOnlyLocalMemoryUses(&AI, seen);
}

Align effectiveAlign(const AllocaInst &AI, const DataLayout &DL) {
    const Align explicitAlign = AI.getAlign();
    const Align abiAlign = DL.getABITypeAlign(AI.getAllocatedType());
    return explicitAlign.value() >= abiAlign.value() ? explicitAlign : abiAlign;
}

void shuffleSlots(std::vector<Slot> &slots, ir::IRRandom &rng) {
    for (std::size_t i = slots.size(); i > 1; --i) {
        const std::size_t j = rng.range(static_cast<std::uint32_t>(i));
        std::swap(slots[i - 1], slots[j]);
    }
}

Value *makeOffsetValue(Module &M, IRBuilder<NoFolder> &B, std::uint64_t offset,
                       bool opaque, ir::IRRandom &rng) {
    Type *i64 = B.getInt64Ty();
    if (!opaque)
        return ConstantInt::get(i64, offset);

    std::uint64_t key = rng.next();
    if (key == 0)
        key = 0x9E3779B97F4A7C15ull;
    const std::uint64_t encoded = offset ^ key;

    auto *GV = new GlobalVariable(
        M, i64, /*isConstant=*/false, GlobalValue::PrivateLinkage,
        ConstantInt::get(i64, encoded), "morok.stack.off");
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    GV->setAlignment(Align(8));

    LoadInst *loaded =
        B.CreateLoad(i64, GV, /*isVolatile=*/true, "morok.stack.off.enc");
    return B.CreateXor(loaded, ConstantInt::get(i64, key), "morok.stack.off");
}

std::optional<std::uint64_t> layoutSlots(std::vector<Slot> &slots,
                                         Align &maxAlign) {
    std::uint64_t cursor = 0;
    maxAlign = Align(1);
    for (Slot &slot : slots) {
        const auto aligned = alignUp(cursor, slot.align);
        if (!aligned.has_value())
            return std::nullopt;
        slot.offset = *aligned;
        if (addWouldOverflow(slot.offset, slot.size))
            return std::nullopt;
        cursor = slot.offset + slot.size;
        if (slot.align.value() > maxAlign.value())
            maxAlign = slot.align;
        if (cursor > kMaxCoalescedFrameBytes)
            return std::nullopt;
    }
    return cursor;
}

} // namespace

bool stackCoalesceFunction(Function &F, const StackCoalesceParams &params,
                           ir::IRRandom &rng) {
    if (F.isDeclaration() || params.probability == 0)
        return false;

    Module *M = F.getParent();
    if (!M)
        return false;

    const DataLayout &DL = M->getDataLayout();
    std::vector<Slot> slots;
    BasicBlock &entry = F.getEntryBlock();
    for (Instruction &I : entry) {
        auto *AI = dyn_cast<AllocaInst>(&I);
        if (!AI)
            continue;
        if (!isEligibleAlloca(*AI, DL))
            continue;
        if (!rng.chance(params.probability))
            continue;

        const std::optional<TypeSize> size = AI->getAllocationSize(DL);
        if (!size.has_value() || size->isScalable())
            continue;
        slots.push_back(Slot{AI, size->getFixedValue(), effectiveAlign(*AI, DL),
                             0, nullptr});
    }

    if (slots.empty())
        return false;

    shuffleSlots(slots, rng);

    Align maxAlign{1};
    const auto totalBytes = layoutSlots(slots, maxAlign);
    if (!totalBytes.has_value() || *totalBytes == 0)
        return false;

    Instruction *insertBefore = &*entry.getFirstInsertionPt();
    IRBuilder<NoFolder> B(insertBefore);
    Type *i8 = B.getInt8Ty();
    auto *frameTy = ArrayType::get(i8, *totalBytes);
    AllocaInst *frame = B.CreateAlloca(frameTy, nullptr, "morok.stack");
    frame->setAlignment(maxAlign);

    for (Slot &slot : slots) {
        Value *offset =
            makeOffsetValue(*M, B, slot.offset, params.opaque_offsets, rng);
        Value *ptr = B.CreateInBoundsGEP(i8, frame, offset,
                                         slot.alloca->getName() + ".slot");
        slot.replacement = ptr;
    }

    SmallVector<Instruction *, 8> lifetimeMarkers;
    for (Slot &slot : slots) {
        SmallPtrSet<Value *, 8> seen;
        collectLifetimeMarkers(slot.alloca, seen, lifetimeMarkers);
    }
    SmallPtrSet<Instruction *, 8> erased;
    for (Instruction *marker : lifetimeMarkers)
        if (erased.insert(marker).second)
            marker->eraseFromParent();

    for (Slot &slot : slots)
        slot.alloca->replaceAllUsesWith(slot.replacement);
    for (Slot &slot : slots)
        slot.alloca->eraseFromParent();

    return true;
}

PreservedAnalyses StackCoalescingPass::run(Function &F,
                                           FunctionAnalysisManager &) {
    if (F.isDeclaration())
        return PreservedAnalyses::all();
    ir::IRRandom rng(engine_);
    return stackCoalesceFunction(F, params_, rng) ? PreservedAnalyses::none()
                                                  : PreservedAnalyses::all();
}

} // namespace morok::passes
