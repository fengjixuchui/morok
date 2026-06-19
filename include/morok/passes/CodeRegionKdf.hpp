// SPDX-License-Identifier: MIT
//
// Morok — modular LLVM IR obfuscator.
//
// Shared post-link code-region hash provider for dataflow-bound consumers.
//
#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/IRBuilder.h"

#include <cstdint>

namespace llvm {
class BasicBlock;
class Function;
class GlobalVariable;
class PHINode;
class Value;
} // namespace llvm

namespace morok::passes::code_region_kdf {

inline constexpr std::uint32_t kUnsealedCodeSize = 0xFFFFFFFFu;

struct SealedCodeHash {
    llvm::Value *has_code = nullptr;
    llvm::PHINode *final_hash = nullptr;
};

std::uint64_t hashStep(std::uint64_t H, std::uint8_t Byte);
std::uint64_t hashBytes(llvm::ArrayRef<std::uint8_t> Bytes,
                        std::uint64_t Seed);

llvm::Value *emitHashStep(llvm::IRBuilderBase &B, llvm::Value *H,
                          llvm::Value *Byte, const llvm::Twine &Name);

// When FailClosedOnUnsealed is true, the value the Exit PHI takes on the
// UNSEALED path is replaced by a nonzero corruption avalanched from the live
// code_size sentinel (instead of the caller-supplied UnsealedValue), so a
// never-sealed / downgrade-reset binary reconstructs garbage from this hash and
// fails closed (#106).  Sealed binaries take the real-hash path and are
// byte-for-byte unaffected — UnsealedValue / the corruption are never consumed
// when code_size holds the real window length.
SealedCodeHash emitSealedCodeHash(llvm::IRBuilderBase &CheckB,
                                  llvm::BasicBlock *CodeCheck,
                                  llvm::BasicBlock *CodeLoop,
                                  llvm::BasicBlock *Exit,
                                  llvm::Function *Target,
                                  llvm::GlobalVariable *CodeSize,
                                  llvm::Value *Seed,
                                  llvm::Value *UnsealedValue,
                                  llvm::StringRef Prefix,
                                  llvm::StringRef FinalName,
                                  bool FailClosedOnUnsealed = false);

} // namespace morok::passes::code_region_kdf
