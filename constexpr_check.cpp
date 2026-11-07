// ============================================================================
// MiniJZ — the compile-time proof, as its own translation unit.
// ----------------------------------------------------------------------------
// The whole pipeline (parse -> SSA -> optimize -> destroy -> bytecode -> VM)
// is constexpr; these programs are evaluated and checked by the compiler
// itself. g++ retains each constant expression's evaluation state until
// the end of the TU, so several proofs in ONE file can exceed a 4 GiB
// process budget — build each check as its own g++ invocation instead:
//
//   for k in 1 2 3 4; do
//     g++ -std=c++20 -O0 -I. -I<dir-of-mjz_lib> \
//         -DMJZ_CX_CHECK=$k -fconstexpr-depth=8192 \
//         -fconstexpr-ops-limit=4000000000 -c constexpr_check.cpp -o /dev/null
//   done
//
// MJZ_CX_CHECK=0 keeps all four (fits when memory allows). The tool
// itself never needs this file — it is a proof/CI artifact.
// ============================================================================
#include "src/minijz.hpp"

#ifndef MJZ_CX_CHECK
#define MJZ_CX_CHECK 0
#endif

#if MJZ_CX_CHECK == 0 || MJZ_CX_CHECK == 1
static_assert(cx_arrays(),
              "MiniJZ pipeline (arena arrays + lib loop analyses) at compile time");
#endif
#if MJZ_CX_CHECK == 0 || MJZ_CX_CHECK == 2
static_assert(cx_fns(),
              "MiniJZ pipeline (recursion/inline/const-call/tail-call) at compile time");
#endif
#if MJZ_CX_CHECK == 0 || MJZ_CX_CHECK == 3
static_assert(cx_series(),
              "MiniJZ pipeline (unroll/closed forms/divmagic/VM) at compile time");
#endif
#if MJZ_CX_CHECK == 0 || MJZ_CX_CHECK == 4
static_assert(cx_tour(),
              "MiniJZ pipeline (branchy unrolling + short-circuit) at compile time");
#endif
