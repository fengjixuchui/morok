// SPDX-License-Identifier: MIT
//
// Morok — test support.  The single translation unit that provides doctest's
// implementation and `main()`.  Every test executable compiles exactly this
// file for its entry point; the test translation units only include the header
// for registration, avoiding duplicate-symbol clashes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
// doctest's timeout path references DBL_EPSILON; some libc++ revisions (e.g. the
// LLVM 22 release toolchain) don't pull <cfloat> in transitively, so include it.
#include <cfloat>
#include "doctest.h"
