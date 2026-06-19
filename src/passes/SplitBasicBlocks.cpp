// SPDX-License-Identifier: MIT
//
// Morok — modular LLVM IR obfuscator.
//
// morok/passes/SplitBasicBlocks.cpp
//
// Walks down each original block, repeatedly cleaving its tail into a fresh
// block at a random instruction boundary.  Only unconditional fall-through
// edges are introduced, so behaviour is unchanged by construction; blocks
// containing PHIs are split only after the PHIs, and EH pads are skipped.

#include "morok/passes/SplitBasicBlocks.hpp"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <iterator>
#include <vector>

using namespace llvm;

namespace morok::passes {

namespace {

// Number of decoy stack slots used by stack-confusion.  A small fixed pool kept
// in the entry block: enough to look like real spill slots without inflating the
// frame or risking unbounded growth (the allocas are never inside a loop).
constexpr unsigned kDecoySlotCount = 3;

} // namespace

bool splitBlocksFunction(Function &F, const SplitParams &params,
                         ir::IRRandom &rng) {
    if (params.splits == 0)
        return false;

    // Snapshot the original blocks; we only split each once into `splits`
    // pieces.
    std::vector<BasicBlock *> originals;
    for (BasicBlock &bb : F)
        originals.push_back(&bb);

    auto *I64 = Type::getInt64Ty(F.getContext());
    // Decoy stack slots are created lazily on the first split so functions with
    // no eligible blocks gain no dead allocas.
    SmallVector<AllocaInst *, kDecoySlotCount> decoySlots;
    auto ensureDecoySlots = [&]() {
        if (!params.stack_confusion || !decoySlots.empty())
            return;
        IRBuilder<NoFolder> B(&*F.getEntryBlock().getFirstInsertionPt());
        for (unsigned s = 0; s < kDecoySlotCount; ++s) {
            auto *slot = B.CreateAlloca(I64, nullptr, "morok.split.decoy");
            slot->setAlignment(Align(8));
            decoySlots.push_back(slot);
        }
    };
    // At a split boundary, push spurious volatile traffic through a decoy slot:
    // the volatile store/load cannot be optimized away (keeping the slot a live
    // analysis surface), but the slot is never meaningfully read, so program
    // behaviour is unchanged.
    auto confuseAt = [&](BasicBlock *tail) {
        if (!params.stack_confusion || decoySlots.empty())
            return;
        IRBuilder<NoFolder> B(&*tail->getFirstInsertionPt());
        AllocaInst *slot = decoySlots[rng.range(kDecoySlotCount)];
        auto *st = B.CreateStore(ConstantInt::get(I64, rng.next()), slot);
        st->setVolatile(true);
        st->setAlignment(Align(8));
        auto *ld = B.CreateLoad(I64, slot, "morok.split.decoy.load");
        ld->setVolatile(true);
        ld->setAlignment(Align(8));
    };

    bool changed = false;
    for (BasicBlock *bb : originals) {
        if (bb->isEHPad() || bb->isLandingPad())
            continue;

        // `cur` is the block whose tail we keep cleaving off.
        BasicBlock *cur = bb;
        for (std::uint32_t i = 0; i < params.splits; ++i) {
            // Candidate cut points: non-PHI instructions strictly between the
            // first non-PHI instruction and the terminator (so both halves are
            // non-empty and the head keeps at least one real instruction).
            BasicBlock::iterator firstNonPhi = cur->getFirstNonPHIIt();
            if (firstNonPhi == cur->end())
                break;
            // Count candidate cut points without materializing them, then walk
            // back to the selected one.  The rng draw and the chosen iterator
            // are identical to collecting into a vector first.
            std::uint32_t count = 0;
            for (BasicBlock::iterator it = std::next(firstNonPhi);
                 it != cur->end() && !it->isTerminator(); ++it)
                ++count;
            if (count == 0)
                break;

            std::uint32_t pick = rng.range(count);
            BasicBlock::iterator cut = std::next(firstNonPhi);
            for (std::uint32_t k = 0; k < pick; ++k)
                ++cut;
            BasicBlock *tail = SplitBlock(cur, cut);
            changed = true;
            ensureDecoySlots();
            confuseAt(tail);
            cur = tail; // keep splitting the remaining tail
        }
    }
    return changed;
}

PreservedAnalyses SplitBasicBlocksPass::run(Function &F,
                                            FunctionAnalysisManager &) {
    if (F.isDeclaration())
        return PreservedAnalyses::all();
    ir::IRRandom rng(engine_);
    return splitBlocksFunction(F, params_, rng) ? PreservedAnalyses::none()
                                                : PreservedAnalyses::all();
}

} // namespace morok::passes
