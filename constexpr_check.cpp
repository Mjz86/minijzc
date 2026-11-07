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
#include "src/00_common.hpp"
#include "src/10_lexer.hpp"
#include "src/20_ast.hpp"
#include "src/21_parser.hpp"
#include "src/30_ir.hpp"
#include "src/31_builder.hpp"
#include "src/40_opt_core.hpp"
#include "src/41_opt_analysis.hpp"
#include "src/42_linform.hpp"
#include "src/50_callgraph.hpp"
#include "src/50b_scctco.hpp"
#include "src/50c_tco.hpp"
#include "src/50d_cdfold.hpp"
#include "src/50e_ifconv.hpp"
#include "src/51_optimize.hpp"
#include "src/52_destr.hpp"
#include "src/60_vm.hpp"
#include "src/61_astrun.hpp"
#include "src/70_program.hpp"
#include "src/71_emit_c.hpp"
#include "src/72_emit_x64.hpp"
#include "src/80_demo.hpp"
#include "src/81_fuzz.hpp"
#include "src/90_cli.hpp"

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
