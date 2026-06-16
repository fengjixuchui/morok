// SPDX-License-Identifier: MIT
//
// Morok — modular LLVM IR obfuscator.
//
// morok/ir/SymbolCloak.hpp — inline, per-site decryption of C symbol names.
//
// Several passes resolve imports dynamically (`dlsym`/anti-hook).  Emitting the
// symbol as a readable string is the single worst leak: a decompiler annotates
// the call with the import name.  `emitCloakedSymbol` instead recovers the name
// at the call site from per-site ciphertext, keyed on a runtime-opaque module
// seed, using one of several keystream generators chosen per site — so the name
// never appears in the binary and cracking one site does not crack the rest.

#ifndef MOROK_IR_SYMBOL_CLOAK_HPP
#define MOROK_IR_SYMBOL_CLOAK_HPP

#include "llvm/ADT/StringRef.h"

namespace llvm {
class IRBuilderBase;
class Module;
class Value;
} // namespace llvm

namespace morok::ir {

class IRRandom;

/// Emit, at `B`'s current insertion point, an inline per-site decryption of the
/// C symbol name `symbol` into a fresh stack buffer, and return an `i8*` to the
/// recovered NUL-terminated string.  No readable copy of `symbol` exists in the
/// artifact: each call site carries its own ciphertext (a private `morok.cloak.c`
/// byte global) and its own unrolled keystream — one of several generators,
/// XOR- or ADD-combined, chosen per site — keyed on `k0 = (volatile load of the
/// mutable module seed `morok.cloak.seed`) ^ siteKey`.  The volatile load is
/// opaque to the optimizer, so the cipher never folds back to text.
llvm::Value *emitCloakedSymbol(llvm::IRBuilderBase &B, llvm::Module &M,
                               llvm::StringRef symbol, IRRandom &rng);

} // namespace morok::ir

#endif // MOROK_IR_SYMBOL_CLOAK_HPP
