#pragma once
// === config.hpp ========================================
// fundamental types + checked arithmetic + the pipeline road map.
// No mjz_lib here (see graph_lib.hpp): val_t/NONE/Rv and the w_* ops
// are pure C++.

// ============================================================================
// minijz_compiler.cpp — a basic compiler for the MiniJZ imperative language,
// built on the graph-analysis algorithms of mjz_lib (mjz::graph_ns).
//
// EVERY piece of CFG mathematics is delegated to the library:
//   mjz::graph_ns::make_basic_forest / make_basic_inv_forest      (CSR graphs)
//   mjz::graph_ns::calculate_dominators_from_entry (Lengauer–Tarjan)
//   mjz::graph_ns::make_dominator_forest_given_dominators        (dominator
//   tree) mjz::graph_ns::calculate_dominance_intervals_given_...       (O(1)
//   dominates) mjz::graph_ns::calculate_natural_loops_with_header_... (natural
//   loops) mjz::graph_ns::calculate_nesting_of_natural_loop_headers     (loop
//   nesting) mjz::graph_ns::calculate_dominance_frontiers_given_dominators (DF)
//   mjz::graph_ns::calculate_iterated_dominance_frontiers_...    (IDF -> phi!)
//   mjz::graph_ns::calculate_strongly_connected_components        (call graph)
//
// Pipeline:
//   source -> lexer -> parser -> AST -> CFG (3-address code)
//          -> [multi-pred normalization: binary merge trees, so every phi
//             has at most 2 args — break/continue create 3+ pred blocks]
//          -> [lib analyses]
//          -> SSA construction (phi placement at IDF, dom-tree renaming)
//          -> PASS MANAGER (tiered fixpoint scheduling, -O0..-O3 presets,
//             per-pass --enable/--disable, --debug dumps, --fp-limit):
//             tier 0 “cheap”    : const fold/prop + algebraic identities +
//               branch folding, copy propagation, canonicalization (incl.
//               NOT-of-compare combination: !(a<b) -> a>=b), dominator-
//               scoped CSE, unreachable-block removal + phi degeneration,
//               block splicing, SSA DCE — always run to a fixpoint first
//             tier 1 “middle”   : mem2reg (array memory-to-register lifting
//               + dead-store elimination for never-read arrays + DEAD-ALLOC
//               DELETION: once nothing reads/writes an array, its allocation
//               itself is pure dead weight and dies, gated by the same
//               handle-observability analysis — no stack array, no memset,
//               no arena slot), trapelim (interval range analysis + handle
//               aliasing -> `nt` proofs: div/mod/load/store traps PROVEN
//               impossible), LICM, div/mod-by-constant -> mulhi+shift+bitops
//               (reciprocal from mjz_lib's
//               get_devide_inverse_and_shift), reassociation (+,-,*c chains
//               -> canonical linear forms), IF-CONVERSION (a cbr whose arms
//               merely materialize constants and whose condition the range
//               analysis proves in [0,1] becomes select arithmetic
//               dst = K0 + (K1-K0)*cond) — fixpoint rounds after each
//             tier 2 “expensive”: full loop unrolling (exact constant trip
//               counts), closed-form loop evaluation — affine recurrences ->
//               additive/geometric series exact mod 2^64, non-affine bodies
//               simulated at compile time, a GENERAL any-condition simulator
//               (no induction variable needed: Euclid's `while (h != 0)` GCD
//               folds to a constant), RUNTIME trip counts: when the body is
//               provably pure+trap-free and the induction provably cannot
//               wrap (constant i0 >= 0, unit step) the exact closed form is
//               emitted UNGUARDED and the loop deleted; otherwise a guarded
//               closed form keeps the original loop as the wrap fallback
//               (with the header phis correctly re-keyed to the new entry
//               edge). CONST-CALL EVALUATION: a call with constant arguments
//               to a function that is pure arithmetic, transitively, is
//               evaluated exactly at compile time — RECURSIVE functions
//               included (fact(4), mutually recursive is_even(10)); a trap,
//               the 512-frame cap, or the step budget aborts and keeps the
//               runtime call. CFG restructuring runs ONLY after tiers 0/1
//               reach their fixpoint.
//          -> stack-allocation analysis (non-escaping constant-length
//             arrays become C locals; the arena is emitted only when
//             something actually escapes) — gated by the program-wide
//             handle-observability analysis (handle PARITY is a semantic)
//          -> interprocedural: call-graph SCC (mjz_lib Tarjan) -> inlining
//             (never recursive fns; bottom-up by SCC; dead-fn elimination)
//          -> SSA destruction (critical-edge split, phis -> edge copies,
//             parallel-copy sequentialization with cycle breaking)
//          -> REGISTER ALLOCATION for C + bytecode: interference coloring
//             over the destroyed form (params pinned to slots 0..P-1 for the
//             VM call convention; unbounded colors, no spills; --no-regalloc
//             keeps one named local per vreg, the "SSA-like C")
//          -> backends: x86-64 asm (GAS, register-allocated via the lib's
//                chordal graph coloring; rbx/r12-r15 + spill slots),
//                C (locals r0..r{K-1} or v0..vN, stack arrays),
//                stack bytecode + VM (frames of K slots)
//          -> differential: AST oracle vs SSA interp vs destroyed interp vs
//             VM vs native C vs native asm  (results must match)
//   plus: the whole pipeline (incl. destruction + VM) is constexpr — one
//         program is compiled and executed inside a static_assert.
//
// Language:
//   let x = expr;   x = expr;   print expr;
//   let a = array(n);   a[i]   a[i] = expr;   len(a)      (arrays)
//   x = input();        read next integer from stdin (0 on EOF)
//   fn name(p0, .. p5) { .. return expr; }    (functions; max 6 params,
//                                             hoisted: mutual recursion ok)
//   name(e0, ..)        call (values are int64; recursion capped at 512
//                       frames -> TRAP, identically in every backend)
//   if (expr) { ... } else { ... }     (else optional)
//   while (expr) { ... }
//   break;   continue;    (loop body only; checked at parse time)
//   expr: int literals, vars, + - * / %, unary - !, comparisons
//         < <= > >= == !=, short-circuit && ||, calls
//   values: 64-bit wrapping signed ints; /0 and INT64_MIN/-1 trap.
//          storage is zero-initialized: reading a variable before its
//          first assignment (possible via let-shadowing) yields 0.
//
//   arrays: a handle is an index into a global bump arena shared by every
//   backend (interpreters, VM, C, x86-64); allocation is monotonic (no free).
//   len(h) of a non-handle yields 0; loads/stores bounds-check and TRAP on
//   out-of-range, exactly like /0. Handles are ordinary int64 values, so
//   arrays-of-arrays (m[i][j]) and aliasing (let b = a) work, and print a
//   prints the handle itself. input() reads a deterministic tape (see
//   --stdin) so all five runners stay byte-for-byte comparable.
//
// Build (normal):     g++ -std=c++20 -I<path-to-repo> -O2 \
//                       minijz_compiler.cpp -o minijz
// Build (compile-time): add  -DMJZ_CX_ASSERT -fconstexpr-depth=8192 \
//                       -fconstexpr-ops-limit=4000000000
//   -> three static_asserts compile and run programs through the ENTIRE
//      pipeline (lex, parse, CFG, mjz graph analyses, SSA, optimization
//      incl. call-graph SCC + inlining + const-call evaluation of
//      recursion + unrolling + closed forms + divmagic, SSA destruction,
//      bytecode emission, VM execution) at build time; g++ retains each
//      constant expression's state, so the assert count is memory-bounded
//      (--selftest runs the full battery of all four at runtime).
//
// Usage:  minijz [options] [file.mjz]
//   no file + tty  -> interactive REPL (blank line runs the buffer)
//   no file + pipe -> program read from stdin
//   -e "<source>"  -> compile inline source
//   --run          -> emit + build + run ALL backends (interp, bytecode VM,
//                     native C, native x86-64 asm), verify identical output,
//                     print a timing table
//   --emit=c,asm,bc       write backend files (c: .c, asm: .s, bc: .mbc+.lst)
//   --stdin=FILE          integers fed to input(): parsed for the in-process
//                          runners AND piped verbatim into the native builds
//   --native-O=0,2,3      -O levels handed to the native compiler (default 0:
//                          measure OUR codegen, not the backend compiler's)
//   --cc=clang++          native compiler driver (default: auto clang++->g++)
//   --repeats=N           timing repeats (default 3)
//   --steps=N             interpreter/VM step limit (default 200000000)
//   -O0 .. -O3            pass-manager preset (default -O3): 0 = SSA only,
//                         1 = cheap tier, 2 = + middle tier, 3 = + unroll/
//                         lcf/inline (see the architecture notes below)
//   --enable=P,P2 / --disable=P,P2   force passes on/off on top of the
//                         preset (fold copyprop canon cse unreach splice
//                         dce mem2reg trapelim licm divmagic reassoc unroll
//                         lcf inline)
//   --unroll=N            full-unroll trip-count cap (default 16, 0 = off)
//   --fp-limit=N          fixpoint round cap per tier (default 24)
//   --debug=N             1 = pass trace, 2 = + IR after each changed pass,
//                         3 = verbose
//   --no-regalloc         one named local per vreg (SSA-like C) instead of
//                         interference-colored shared slots (r0..r{K-1})
//   --no-opt              disable the optimizer (same as -O0)
//   --no-inline / --no-reassoc / --no-lcf / --no-divmagic   granular off
//   --print-ir            dump SSA (pre/post opt) and destroyed IR
//   --print-bc            print the bytecode listing
//   --opt-report / --quiet  control the per-pass report
//   --demo / --fuzz[=N] / --selftest   test suites
// ============================================================================
//
// ============================================================================
// DESIGN & ARCHITECTURE
// ============================================================================
// A guided tour of how the compiler is put together and WHY, in pipeline
// order. Section numbers below match the numbered sections in the code.
//
// ---------------------------------------------------------------- 1..4 front
//   lexer -> recursive-descent parser -> arena AST (indices, no pointers:
//   the whole thing must run constexpr). `fn` names are hoisted by a brace-
//   depth prescan, so calls may reference later definitions (mutual
//   recursion). `break`/`continue` are parsed with a loop-depth counter and
//   lowered to plain jumps (6) — they carry no values, so the AST oracle
//   (11) models them with two flags that the enclosing While consumes.
//
// ------------------------------------------------------- 5..6 IR + CFG build
//   OneInstr = one three-address operation; a Fn is blocks of instrs plus
//   a 2-wide successor array. Key invariant: a PHI HOLDS EXACTLY TWO ARGS,
//   keyed by the predecessor ids in t/f — merge points must have <= 2 preds.
//   `break`/`continue` break that invariant (a continue is a SECOND back
//   edge into the header), so the CFG builder runs normalize_multi_preds:
//   every >2-pred block gets its extra edges rerouted through a right-
//   leaning chain of fresh 2-pred `br` blocks (a binary merge tree). Phi
//   placement at the IDF then lowers every value merge into 2-arg phi
//   chains automatically, and the loop analyses see a single latch again.
//   The `nt` flag on Instr is a PROVEN-impossible-trap certificate (9d3c).
//
// ------------------------------------------------------- 7..8 analyses + SSA
//   All dominator/loop/frontier math comes from mjz::graph_ns (list at the
//   top of this file). SSA: phis at the iterated dominance frontier of each
//   def set, then a dominator-tree renaming walk with per-variable stacks.
//
// ------------------------------------------- 9d3a/9d3b alias + range analyses
//   alias_analyze: which vregs provably hold the handle of EXACTLY ONE
//   allocation site (Alloc dst, copies, same-alloc phis; everything else —
//   arithmetic, params, loads, input — is "obscure"). Two PROGRAM-WIDE
//   gates are derived from it: lift_ok (no Load/Store through an obscure
//   handle anywhere: array promotion is invisible) and stkarr_ok (no handle
//   VALUE observable outside handle position: moving an array out of the
//   arena shifts every later handle, which is OBSERVABLE via print).
//   Handle parity is a semantic: the arena order must match in all runners.
//
//   range_analyze: signed intervals per vreg per block entry, grown UP from
//   concrete seeds (bottom-up), with branch-condition refinement on each
//   out-edge (`x < y` taken caps x.hi; `x != 0` chops an endpoint), WIDENING
//   AT LOOP HEADERS ONLY (widening everywhere re-inflates refined body
//   states and an i+1 over the widened bound overflows to TOP, poisoning
//   the loop counter to [MIN+k, ...] forever), then narrowing rounds. No
//   fixpoint at all -> fall back to TOP (sound, proves nothing). Consumers:
//   trapelim (below) and nothing else — it is purely a proofs engine.
//
// ------------------------------------------------- 9d3c trap elimination (nt)
//   A Div/Mod whose divisor range excludes 0 (and excludes -1, or the
//   dividend excludes INT64_MIN), and a Load/Store whose handle is provably
//   one constant-length allocation with the index interval inside [0,len),
//   can never trap. Marked `nt`, the backends drop the guard, DCE/LICM
//   treat the op as pure, and the loop passes may delete the loop computing
//   it. TRAP-ELIMINATION NEVER CHANGES OBSERVABLE BEHAVIOR — a trap that is
//   merely POSSIBLE keeps its guard.
//
// ----------------------------------------------------- 9d3d mem2reg for arrays
//   Scalar mem2reg IS SSA construction (every source variable is one vreg).
//   This pass is the ARRAY version — memory-to-register lifting:
//     * liftable: one allocation, constant length <= 256, every access via
//       a provably-single handle with CONSTANT in-range index -> each array
//       element becomes a stack of SSA versions (phis at the element's IDF).
//     * dead: never read, and every store provably non-trapping (constant
//       index in range, or `nt` from trapelim — the tier-1 fixpoint re-runs
//       mem2reg, so round 2 sees round 1's proofs) -> the stores are simply
//       deleted. "If the array is not read we just delete writes."
//     * dead ALLOCATION: once lifting/DSE has removed every Load/Store/Len
//       of an array (or it never had any), the allocation itself is unused;
//       under the program-wide handle-observability gate (stkarr_ok) its
//       deletion is invisible, so the Alloc dies too — the emitted C gets
//       no `int64_t mjz_sN[len];` declaration and no memset at all.
//
// ------------------------------------------------------- 9e0 the pass manager
//   PassCfg is threaded CLI -> compile -> optimize. Three cost tiers (see
//   the pipeline list above). The scheduler's discipline: cheap passes to a
//   FIXPOINT first; middle passes one round each + a cheap fixpoint, to a
//   global fixpoint; expensive CFG restructuring (unroll, lcf) ONLY after
//   0/1 stabilized, each round followed by full 0(+1) fixpoints again —
//   expensive passes see maximally-simplified loops and re-enable cheap
//   ones only when they actually restructure something. -O presets gate
//   tiers; --enable/--disable apply on top; --fp-limit caps every fixpoint
//   loop; --debug dumps per-pass traces (1) and IR snapshots (2).
//
// --------------------------------------------- 9d5 loop closed forms + sim
//   Counted loops (induction phi, `<`/`<=` test vs constant): exact trip
//   count N; every loop-carried var is an affine recurrence x' = a*x +
//   b*i + g with closed form x_N = K1*x0 + K0 — K1 = a^N, K0 from geometric
//   series S(n) and T(n) = sum j*a^j, all EXACT mod 2^64 by bit-doubling
//   (x = x*3 + 1 folds in one mul-add chain even when it wraps; runtime x0
//   works too). Non-affine bodies: exact compile-time simulation of the N
//   iterations. Non-counted tests (e.g. `while (h != 0)`): the GENERAL
//   simulator — if all loop inputs are compile-time constants and the body
//   is straight-line pure math, iterate the header test + body chain with
//   the exact wrapping/trapping arithmetic (a trap aborts and keeps the
//   loop) and splice the final phi values in as constants. This is what
//   folds Euclid's GCD to 21 with no induction variable at all. Runtime
//   trip counts: if the body is pure and provably non-trapping AND the
//   induction provably cannot wrap (constant i0 >= 0 with unit step: D =
//   B - i0 lands in (0, MAX] whenever the condition holds, and iN = i0 +
//   (B - i0) reproduces B bit-exactly), the closed form is exact for every
//   runtime B — emit it UNGUARDED and delete the loop. Otherwise keep a
//   guard (closed form only when the wrap-safety proof holds at runtime)
//   with the original loop as the honest fallback, the header phis
//   re-keyed to the guard edge so destruction places the entry copies.
//   COUNTDOWN FOLD (cdfold): the loop shape tail-call elimination emits —
//   [sel-dispatch; nP/budget header phis; nP==0 base exits; budget==0 TRAP;
//   latch: nP-1, bH-1, sel toggle] — is replaced by the exact closed form:
//   N = (u64)n0 (unsigned countdown), guard `(u64)n0 > (u64)budget` -> the
//   SAME trap the recursion cap would have fired (negative n wraps to a
//   huge unsigned count and over-budget n both trap), sel_N = (1-2p)*sel0
//   + p with p = N&1 (the toggle's exact closed form; sel stays in {0,1}
//   when sel0 is — which is when the final cbr select is sound), additive
//   accumulators via x_N = x0 + gN + b(N*n0 - N(N-1)/2), and the n%2 ->
//   n&1 strength reduction happens by emitting the parity bit as
//   And(n0,1) directly. Binary-search-shaped bodies (args not affine in
//   the phis) keep their loop.
//
// ------------------------------------------------- 9e4 tail-call elimination
//   Inner-SCC TAIL calls (the result flows straight into the fn's Ret)
//   need no return address: a self-tail-recursive fn becomes a LOOP in
//   place (its entry block becomes the loop header with phis for the
//   parameters; each tail site becomes [budget check -> synthesized trap;
//   jump back]), and a pure-tail MUTUAL-recursion SCC (is_even/is_odd)
//   merges into ONE sel-dispatched leaf function Fm(sel, a0..): callers
//   pass the member index (an invariant tailrec itself creates and records
//   in Fn::tce_sel), and sibling tail calls become [sel'=sibling; args;
//   budget check; jump back]. A new K::Depth intrinsic (the frame count,
//   identical in every runner) materializes the EXACT remaining recursion
//   budget = 512+1-Depth(), so the transformed loop traps at precisely the
//   activation where the original call chain would have hit the 512-frame
//   cap — including the "negative n never reaches the base case" case. The
//   trap is the existing `t = 1/0` idiom (div-by-constant-zero), which
//   every backend already emits as TRAP+exit and every optimizer pass
//   already treats as an observable, unfoldable root. After tailrec the
//   merged leaf is inlined into its callers, and cdfold (above) sees the
//   recursion as the countdown loop it always was: print(is_odd(input()))
//   compiles to [if ((uint64_t)input() > 511) TRAP] + print(input() & 1) —
//   the guard being semantically REQUIRED (is_odd(600) TRAPs in the source
//   language, at exactly the point the guard fires). Inlining a
//   Depth-reading fn adjusts Depth to caller+1 (and in main it
//   constant-folds to 1), so folded budgets are exact at every call depth.
//
//   Call-graph SCC (lib Tarjan): multi-member SCC or self-edge = recursive =
// never inlined; otherwise inline bottom-up (callees first, constants flow
// through), re-optimizing each caller, then dead-function elimination.
//
// ------------------------------------------------- 9e3 const-call evaluation
//   Interprocedural, after inlining (so only recursion survivors remain as
//   calls): a Call whose arguments are all compile-time constants and whose
//   callee is pure arithmetic TRANSITIVELY (every fn it reaches uses only
//   Const/Copy/Bin/Br/Cbr/Phi/Call/Ret — no Print, no input(), no arena) is
//   executed exactly at compile time by a mini SSA interpreter that mirrors
//   run_prog (edge-resolved phis, wrapping arithmetic, Ret = val[a]), with
//   the SAME 512-frame recursion cap and a 2^20-step budget: a trap or a
//   cap aborts the fold and keeps the runtime call (a runtime TRAP is
//   observable). fact(4) and is_even(10) become constants; dead-function
//   elimination then removes the callless functions entirely.
//
// ------------------------------------------------ 9g SSA destruction + slots
//   Critical edges split; phis -> per-edge parallel copies; cycles broken
//   with a scratch temp; copy fusion merges chains. Then (11z) the C and
//   bytecode backends get their REGISTER ALLOCATION: block-granular
//   backward liveness to a fixpoint -> interference graph (def vs
//   live-after; params are entry defs, so they conflict with each other and
//   everything live-in) -> greedy coloring, params PINNED to slots 0..P-1
//   (the VM's Call writes args there; the C signature names them r0..rP-1),
//   everyone else smallest-free-color, highest degree first. Unbounded
//   colors: C has unlimited locals, the VM unlimited frame slots — no spill
//   code exists at all. vregs with disjoint live ranges share a slot, so
//   the emitted C declares a handful of r0..rK locals instead of one v per
//   SSA value; --no-regalloc keeps the identity mapping (SSA-like C).
//
// ------------------------------------------------------- 13.5 the backends
//   All three agree bit-for-bit with the two interpreters (differential
//   testing below): wrapping arithmetic (uint64 casts in C, explicit ops in
//   asm, VM ops), trap order and text ("TRAP\n" + exit 1), arena layout and
//   handle values, input() tape. The C backend inlines single-def consts,
//   fuses compares into branches (and folds the NOT of a compare into the
//   compare: `if (!(a<b)) goto L;` -> `if (a>=b) goto L;`), drops fallthrough
//   gotos, and emits zero-initialized stack arrays for provably-local
//   allocations. The x86-64 backend additionally does chordal coloring onto
//   rbx/r12-r15 with spill slots (that one HAS a fixed register set).
//
// ------------------------------------------------- 14..17 verification
//   Every compiled program is run by up to SIX runners that must agree
//   byte-for-byte: the AST oracle (a direct interpreter of the tree), the
//   unoptimized-SSA interpreter, the optimized-SSA interpreter, the
//   destroyed-code interpreter, the bytecode VM, and (in --run/--fuzz-
//   native) natively-compiled C and asm at -O0 and -O2. --fuzz generates
//   random programs (scalars, arrays incl. deliberate OOB traps, input()
//   tapes, functions, recursion, break/continue) and ROTATES pass
//   configurations across seeds — every run exercises a different slice of
//   the pipeline, which is what makes regressions surface quickly. The
//   whole pipeline is also constexpr (MJZ_CX_ASSERT): a static_assert
//   compiles and runs a nontrivial program at build time.
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bit>
#include <chrono>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#define MJZ_HAVE_ISATTY 1
#endif

// CHECK bookkeeping: in the multi-cpp build these are REAL globals defined
// once in tu/main.cpp, so checks that fire in any TU (the fuzzer runs in
// its own TU, the demos in another) are counted by the reporting in main.
// Header-only / constexpr builds keep them TU-local statics (zero cost).
#ifdef MJZ_MULTI_CPP
extern int g_checks;
extern int g_fails;
#else
static int g_checks = 0, g_fails = 0;
#endif
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      std::printf("FAIL[%d]: %s\n", __LINE__, msg);                            \
      ++g_fails;                                                               \
    }                                                                          \
  } while (0)

// ============================================================================
// 1. values & wrapping/trapping arithmetic (shared by both interpreters)
// ============================================================================
using val_t = int64_t;
constexpr uint32_t NONE = ~uint32_t(0); // "no vreg/block/index" sentinel
struct Rv {                             // result of one arithmetic op
  val_t v{};
  bool trap{};
};

// the language caps recursion at 512 frames -> TRAP in every backend; the
// const-call evaluator, the frame guard and the VM share this one constant
inline constexpr uint32_t MJZ_MAX_CALL_DEPTH = 512;

// scratch buffer for the ONE dynamic compile diagnostic (call arity: it
// names the function and both counts). A plain inline variable: runtime
// only — the constexpr proofs never hit the error path — and program
// lifetime, so Program::error may point into it exactly like a literal.
inline char mjz_errbuf[128]{};

// the arena: 2^21 int64 slots = 16 MiB; CAP must match what the C/asm
// backends emit (see opt/analysis/alias.hpp)
inline constexpr val_t MJZ_ARENA_CAP = 2097152;

// the runner contract shared by EVERY backend (interpreters included):
// one "%lld\n" per print; "TRAP\n" appended if a div/mod traps; "TIMEOUT\n"
// if a step-limited runner hit its limit. This struct also carries the
// reference arena + stdin tape for the in-process interpreters.
struct RunOut {
  std::vector<val_t> out{};
  bool trapped{};
  bool timeout{};
  // source offset + category of the trapping instruction (SSA interpreter
  // and AST oracle only — the VM/ native builds keep plain TRAP): for
  // located runtime diagnostics ("division by zero at 7:3").
  // trap_kind points at a static string; NONE/null = unknown.
  uint32_t trap_src{NONE};
  const char *trap_kind{};
  // array arena (grows during execution; mirrors the C/asm static arenas)
  std::vector<val_t> a_data{}; // the slots (zero-initialized by resize)
  std::vector<val_t> a_lenv{}; // len[handle]; 0 = not an allocated handle
  // stdin tape for input() (empty tape reads 0, like EOF)
  std::vector<val_t> ins{};
  size_t in_pos{};

  constexpr Rv arena_alloc(val_t n) {
    if (n < 0 || n > MJZ_ARENA_CAP ||
        n > (val_t)MJZ_ARENA_CAP - (val_t)a_data.size())
      return {0, true}; // negative size / out of arena memory
    val_t h = (val_t)a_data.size();
    a_data.resize(a_data.size() + (size_t)n, 0);
    if ((size_t)h >= a_lenv.size())
      a_lenv.resize((size_t)h + 1, 0);
    a_lenv[(size_t)h] = n;
    return {h, false};
  }
  constexpr val_t arena_len(val_t h) const {
    if (h < 0 || (size_t)h >= a_lenv.size())
      return 0;
    return a_lenv[(size_t)h];
  }
  constexpr Rv arena_load(val_t h, val_t i) {
    val_t l = arena_len(h); // bogus handle -> len 0 -> always out of bounds
    if (i < 0 || i >= l)
      return {0, true};
    return {a_data[(size_t)(h + i)], false};
  }
  constexpr bool arena_store(val_t h, val_t i, val_t v) {
    val_t l = arena_len(h);
    if (i < 0 || i >= l)
      return false;
    a_data[(size_t)(h + i)] = v;
    return true;
  }
  constexpr val_t next_input() {
    if (in_pos < ins.size())
      return ins[in_pos++];
    return 0; // EOF (matches scanf-failure -> 0 in the native backends)
  }
};

constexpr Rv w_add(val_t a, val_t b) {
  return {(val_t)((uint64_t)a + (uint64_t)b), false};
}
constexpr Rv w_sub(val_t a, val_t b) {
  return {(val_t)((uint64_t)a - (uint64_t)b), false};
}
constexpr Rv w_mul(val_t a, val_t b) {
  return {(val_t)((uint64_t)a * (uint64_t)b), false};
}
constexpr Rv w_div(val_t a, val_t b) {
  if (b == 0 || (a == INT64_MIN && b == -1))
    return {0, true};
  return {a / b, false};
}
constexpr Rv w_mod(val_t a, val_t b) {
  if (b == 0 || (a == INT64_MIN && b == -1))
    return {0, true};
  return {a % b, false};
}
// bitwise / shift / high-multiply helpers (same wrapping discipline: shifts
// mask the amount to 6 bits so every runner agrees for any operand)
constexpr uint64_t w_shamt(val_t b) { return (uint64_t)b & 63; }
constexpr Rv w_and(val_t a, val_t b) {
  return {(val_t)((uint64_t)a & (uint64_t)b), false};
}
constexpr Rv w_or(val_t a, val_t b) {
  return {(val_t)((uint64_t)a | (uint64_t)b), false};
}
constexpr Rv w_xor(val_t a, val_t b) {
  return {(val_t)((uint64_t)a ^ (uint64_t)b), false};
}
constexpr Rv w_shl(val_t a, val_t b) {
  return {(val_t)((uint64_t)a << w_shamt(b)), false};
}
constexpr Rv w_lshr(val_t a, val_t b) {
  return {(val_t)((uint64_t)a >> w_shamt(b)), false};
}
constexpr Rv w_ashr(val_t a, val_t b) {
  return {(val_t)(a >> (int64_t)w_shamt(b)), false}; // arithmetic (C++20)
}
constexpr Rv w_mulhi(val_t a, val_t b) { // high 64 bits of the unsigned prod
  return {(val_t)((uint64_t)((((unsigned __int128)(uint64_t)a) * (uint64_t)b) >>
                             64)),
          false};
}
