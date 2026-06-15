// SPDX-License-Identifier: MIT
//
// Morok — modular LLVM IR obfuscator.
//
// morok/ir/InstUtil.hpp — shared instruction-level resilience helpers.
//
// Small predicates that several passes need in order to stay well-formed on
// unusual-but-valid IR.  Keeping them in one place means the invariant is
// enforced identically everywhere instead of being re-derived per pass.

#ifndef MOROK_IR_INST_UTIL_HPP
#define MOROK_IR_INST_UTIL_HPP

namespace llvm {
class Function;
class ReturnInst;
} // namespace llvm

namespace morok::ir {

/// True if `RI` is the `ret` that closes a `musttail` call sequence.
///
/// The verifier requires a `musttail` call to be immediately followed by a
/// `ret` that returns the call's (optionally bitcast) result, or `ret void`.
/// Any transform that splits the block at this return, inserts instructions
/// between the call and the return, or rewrites the return operand produces
/// IR that fails verification.  Passes that touch returns must skip these.
bool isMustTailReturn(const llvm::ReturnInst &RI);

/// True if `F` uses a Windows-style funclet EH personality (MSVC C++, SEH,
/// CoreCLR).  Calls inserted into funclet-colored blocks of such functions
/// require a `["funclet"(token)]` operand bundle or the verifier rejects them;
/// passes that splice plain calls into arbitrary blocks should skip these
/// functions.  Always false on Itanium (macOS/Linux) and EH-free functions, so
/// it is a no-op on those targets.
bool usesFuncletEH(const llvm::Function &F);

} // namespace morok::ir

#endif // MOROK_IR_INST_UTIL_HPP
