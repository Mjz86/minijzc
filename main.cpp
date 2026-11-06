// ============================================================================
// minijz_compiler.cpp — a basic compiler for the MiniJZ imperative language,
// built on the graph-analysis algorithms of mjz_lib (mjz::graph_ns).
//
// EVERY piece of CFG mathematics is delegated to the library:
//   mjz::graph_ns::make_basic_forest / make_basic_inv_forest      (CSR graphs)
//   mjz::graph_ns::calculate_dominators_from_entry                (Lengauer–Tarjan)
//   mjz::graph_ns::make_dominator_forest_given_dominators        (dominator tree)
//   mjz::graph_ns::calculate_dominance_intervals_given_...       (O(1) dominates)
//   mjz::graph_ns::calculate_natural_loops_with_header_...       (natural loops)
//   mjz::graph_ns::calculate_nesting_of_natural_loop_headers     (loop nesting)
//   mjz::graph_ns::calculate_dominance_frontiers_given_dominators (DF)
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
//               + dead-store elimination for never-read arrays), trapelim
//               (interval range analysis + handle aliasing -> `nt` proofs:
//               div/mod/load/store traps PROVEN impossible), LICM, div/mod-
//               by-constant -> mulhi+shift+bitops (reciprocal from mjz_lib's
//               get_devide_inverse_and_shift), reassociation (+,-,*c chains
//               -> canonical linear forms) — fixpoint rounds after each
//             tier 2 “expensive”: full loop unrolling (exact constant trip
//               counts), closed-form loop evaluation — affine recurrences ->
//               additive/geometric series exact mod 2^64, non-affine bodies
//               simulated at compile time, a GENERAL any-condition simulator
//               (no induction variable needed: Euclid's `while (h != 0)` GCD
//               folds to a constant), RUNTIME trip counts get a guarded
//               closed form with the loop as the wrap fallback. CFG
//               restructuring runs ONLY after tiers 0/1 reach their fixpoint.
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
//   -> the ENTIRE pipeline (lex, parse, CFG, mjz graph analyses, SSA,
//      optimization incl. call-graph SCC + inlining + unrolling, SSA
//      destruction, bytecode emission, VM execution) then runs inside a
//      static_assert at compile time (functions, recursion, mutual
//      recursion and unrolled loops included).
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
//       element becomes a stack of SSA versions (phis at the element's IDF);
//       the Alloc itself STAYS in the arena (handle parity!).
//     * dead: never read, and every store provably non-trapping (constant
//       index in range, or `nt` from trapelim — the tier-1 fixpoint re-runs
//       mem2reg, so round 2 sees round 1's proofs) -> the stores are simply
//       deleted. "If the array is not read we just delete writes."
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
//   trip counts: guarded closed form, original loop kept as the wrap case.
//
// ------------------------------------------------------- 9e2 interprocedural
//   Call-graph SCC (lib Tarjan): multi-member SCC or self-edge = recursive =
// never inlined; otherwise inline bottom-up (callees first, constants flow
// through), re-optimizing each caller, then dead-function elimination.
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
#define MJZ_WITH_iostream false
#define MJZ_TEST_MODE_ 1
#define MJZ_LOG_NEW_ALLOCATIONS_ 0
#define MJZ_aggregate_tuple_workaround false
#include "mjz_lib/alllib_import.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

using namespace mjz;
namespace gns = mjz::graph_ns;
inline constexpr version_t V{};
using ulen = uintlen_t;

static int g_checks = 0, g_fails = 0;
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
struct Rv {                 // result of one arithmetic op
  val_t v{};
  bool trap{};
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
  return {(val_t)((uint64_t)((((unsigned __int128)(uint64_t)a) *
                              (uint64_t)b) >>
                             64)),
          false};
}

// ============================================================================
// 2. lexer
// ============================================================================
enum class Tk : uint8_t {
  End, Id, Num,
  KwLet, KwPrint, KwIf, KwElse, KwWhile, KwFn, KwRet, KwBreak, KwCont,
  Plus, Minus, Star, Slash, Percent, Comma,
  Lt, Le, Gt, Ge, EqEq, Ne, AndAnd, OrOr, Not,
  Assign, LP, RP, LB, RB, LBr, RBr, Semi
};
struct Tok {
  Tk k{};
  uint32_t pos{};     // offset into source (for diagnostics)
  val_t num{};
  uint32_t lo{}, hi{}; // identifier slice [lo,hi)
};

struct LexError {
  uint32_t pos{};
  const char *what{};
};

struct Lexed {
  std::vector<Tok> toks{};
  uint32_t nvars{};                       // vars discovered (parse assigns ids)
  bool ok{};
  LexError err{};
};

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool is_alpha_(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

constexpr Lexed lex(std::string_view src) {
  Lexed L{};
  uint32_t i = 0;
  const uint32_t n = (uint32_t)src.size();
  auto err = [&](uint32_t p, const char *w) {
    L.ok = false;
    L.err = {p, w};
    return L;
  };
  while (i < n) {
    char c = src[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
    if (c == '/' && i + 1 < n && src[i + 1] == '/') {   // line comment
      while (i < n && src[i] != '\n') ++i;
      continue;
    }
    uint32_t p = i;
    if (is_digit(c)) {
      val_t v = 0;
      while (i < n && is_digit(src[i])) {
        v = (val_t)((uint64_t)v * 10u + (uint64_t)(src[i] - '0'));
        ++i;
      }
      L.toks.push_back({Tk::Num, p, v, p, i});
      continue;
    }
    if (is_alpha_(c)) {
      while (i < n && (is_alpha_(src[i]) || is_digit(src[i]))) ++i;
      std::string_view w = src.substr(p, i - p);
      Tk k = Tk::Id;
      if (w == "let") k = Tk::KwLet;
      else if (w == "print") k = Tk::KwPrint;
      else if (w == "if") k = Tk::KwIf;
      else if (w == "else") k = Tk::KwElse;
      else if (w == "while") k = Tk::KwWhile;
      else if (w == "fn") k = Tk::KwFn;
      else if (w == "return") k = Tk::KwRet;
      else if (w == "break") k = Tk::KwBreak;
      else if (w == "continue") k = Tk::KwCont;
      L.toks.push_back({k, p, 0, p, i});
      continue;
    }
    ++i;
    Tk k{};
    if (c == '+') k = Tk::Plus;
    else if (c == '-') k = Tk::Minus;
    else if (c == '*') k = Tk::Star;
    else if (c == '/') k = Tk::Slash;
    else if (c == '%') k = Tk::Percent;
    else if (c == '=') {
      if (i < n && src[i] == '=') { ++i; k = Tk::EqEq; }
      else k = Tk::Assign;
    }
    else if (c == '<') {
      if (i < n && src[i] == '=') { ++i; k = Tk::Le; }
      else k = Tk::Lt;
    }
    else if (c == '>') {
      if (i < n && src[i] == '=') { ++i; k = Tk::Ge; }
      else k = Tk::Gt;
    }
    else if (c == '!' && i < n && src[i] == '=') { ++i; k = Tk::Ne; }
    else if (c == '!' && !(i < n && src[i] == '=')) k = Tk::Not;
    else if (c == '&' && i < n && src[i] == '&') { ++i; k = Tk::AndAnd; }
    else if (c == '|' && i < n && src[i] == '|') { ++i; k = Tk::OrOr; }
    else if (c == '(') k = Tk::LP;
    else if (c == ')') k = Tk::RP;
    else if (c == '{') k = Tk::LB;
    else if (c == '}') k = Tk::RB;
    else if (c == '[') k = Tk::LBr;
    else if (c == ']') k = Tk::RBr;
    else if (c == ';') k = Tk::Semi;
    else if (c == ',') k = Tk::Comma;
    else return err(p, "unknown character");
    L.toks.push_back({k, p, 0, p, i});
  }
  L.toks.push_back({Tk::End, n, 0, n, n});
  L.ok = true;
  return L;
}

// ============================================================================
// 3. AST (arena of nodes, indices instead of pointers — constexpr friendly)
// ============================================================================
enum class Ek : uint8_t { Num, Var, Neg, Not, Bin, And, Or, Alloc, Load, Len, Input, Call };
enum class Bk : uint8_t { Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne };

struct Expr {
  Ek k{};
  Bk b{};
  val_t num{};
  uint32_t var{};
  uint32_t l{}, r{};   // child indices into exprs arena
};
enum class Sk : uint8_t { Let, Assign, Print, If, While, Seq, StoreA, Ret, Brk, Cont };
struct Stmt {
  Sk k{};
  uint32_t var{};
  uint32_t e{};                    // primary expr
  uint32_t a{}, b{};               // Seq: seq_kids index in a; If/While bodies
};

struct FnDef { // a top-level `fn name(params) { body }`
  std::string_view name{};
  std::vector<uint32_t> params{}; // AST var ids (one scope level)
  uint32_t body{};                // Seq stmt index of the body block
  bool defined{false};
};
struct Ast {
  std::vector<Expr> es;
  std::vector<Stmt> ss;
  std::vector<FnDef> fns{};                   // parsed defs (hoist order)
  std::vector<std::string_view> hoisted_fns{}; // names, pre-scanned for calls
  // each Seq owns its OWN children list (a shared arena would interleave
  // nested blocks' children into the parent's [begin,begin+count) window)
  std::vector<std::vector<uint32_t>> seq_kids;
  std::vector<std::string_view> names; // var id -> name
  // scope table for the parser
  struct Scp { std::string_view name{}; uint32_t id{}; };
  std::vector<Scp> scope;
  std::vector<uint32_t> scope_marks;
  constexpr uint32_t new_var(std::string_view n) {
    // duplicate only within the CURRENT block scope
    uint32_t start = scope_marks.empty() ? 0 : scope_marks.back();
    for (uint32_t i = (uint32_t)scope.size(); i-- > start;)
      if (scope[i].name == n) return 0xDEAD'FFFF;
    // shadowing of an outer variable creates a FRESH id (distinct storage)
    names.push_back(n);
    scope.push_back({n, (uint32_t)(names.size() - 1)});
    return (uint32_t)(names.size() - 1);
  }
};

struct ParseError {
  uint32_t pos{};
  const char *what{};
};
struct Parsed {
  bool ok{};
  ParseError err{};
  Ast ast{};
  uint32_t root{}; // Seq index
  bool uses_input{}; // the program contains input()
};

// ============================================================================
// 4. recursive-descent parser
// ============================================================================
struct Parser {
  const Lexed &L;
  std::string_view src;
  Ast &a;
  uint32_t ti{}; // token index
  ParseError err{};
  bool ok{true};
  bool in_fn{false}; // 'return' is only legal inside a fn body
  int loop_depth{};  // 'break'/'continue' are only legal inside a loop body

  constexpr Parser(const Lexed &L_, std::string_view s, Ast &a_)
      : L(L_), src(s), a(a_) {}

  constexpr void fail(uint32_t pos, const char *w) {
    if (ok) { ok = false; err = {pos, w}; }
  }
  constexpr const Tok &peek() const { return L.toks[ti]; }
  constexpr const Tok &eat() { return L.toks[ti++]; }
  constexpr bool accept(Tk k) {
    if (peek().k == k) { ++ti; return true; }
    return false;
  }
  constexpr bool expect(Tk k, const char *w) {
    if (!accept(k)) { fail(peek().pos, w); return false; }
    return true;
  }

  constexpr uint32_t find_var(std::string_view n, uint32_t pos) {
    for (auto it = a.scope.rbegin(); it != a.scope.rend(); ++it)
      if (it->name == n) return it->id;
    fail(pos, "use of undeclared variable");
    return 0xFFFF'FFFF;
  }

  // expr := logic
  constexpr uint32_t expr() {
    uint32_t lhs = rel();
    if (!ok) return 0;
    for (;;) {
      if (peek().k == Tk::AndAnd) {
        ++ti;
        uint32_t rhs = rel();
        if (!ok) return 0;
        uint32_t idx = (uint32_t)a.es.size();
        a.es.push_back({Ek::And, {}, 0, 0, lhs, rhs});
        lhs = idx;
      } else if (peek().k == Tk::OrOr) {
        ++ti;
        uint32_t rhs = rel();
        if (!ok) return 0;
        uint32_t idx = (uint32_t)a.es.size();
        a.es.push_back({Ek::Or, {}, 0, 0, lhs, rhs});
        lhs = idx;
      } else return lhs;
    }
  }
  // rel := add (cmp add)?
  constexpr uint32_t rel() {
    uint32_t lhs = add();
    if (!ok) return 0;
    Bk b{};
    switch (peek().k) {
    case Tk::Lt: b = Bk::Lt; break;
    case Tk::Le: b = Bk::Le; break;
    case Tk::Gt: b = Bk::Gt; break;
    case Tk::Ge: b = Bk::Ge; break;
    case Tk::EqEq: b = Bk::Eq; break;
    case Tk::Ne: b = Bk::Ne; break;
    default: return lhs;
    }
    ++ti;
    uint32_t rhs = add();
    if (!ok) return 0;
    uint32_t i = (uint32_t)a.es.size();
    a.es.push_back({Ek::Bin, b, 0, 0, lhs, rhs});
    return i;
  }
  // add := mul (('+'|'-') mul)*
  constexpr uint32_t add() {
    uint32_t lhs = mul();
    if (!ok) return 0;
    for (;;) {
      Bk b{};
      if (peek().k == Tk::Plus) b = Bk::Add;
      else if (peek().k == Tk::Minus) b = Bk::Sub;
      else return lhs;
      ++ti;
      uint32_t rhs = mul();
      if (!ok) return 0;
      uint32_t idx = (uint32_t)a.es.size();
      a.es.push_back({Ek::Bin, b, 0, 0, lhs, rhs});
      lhs = idx;
    }
  }
  // mul := unary (('*'|'/'|'%') unary)*
  constexpr uint32_t mul() {
    uint32_t lhs = unary();
    if (!ok) return 0;
    for (;;) {
      Bk b{};
      if (peek().k == Tk::Star) b = Bk::Mul;
      else if (peek().k == Tk::Slash) b = Bk::Div;
      else if (peek().k == Tk::Percent) b = Bk::Mod;
      else return lhs;
      ++ti;
      uint32_t rhs = unary();
      if (!ok) return 0;
      uint32_t idx = (uint32_t)a.es.size();
      a.es.push_back({Ek::Bin, b, 0, 0, lhs, rhs});
      lhs = idx;
    }
  }
  constexpr uint32_t unary() {
    if (accept(Tk::Minus)) {
      uint32_t c = unary();
      uint32_t i = (uint32_t)a.es.size();
      a.es.push_back({Ek::Neg, {}, 0, 0, c, 0});
      return i;
    }
    if (accept(Tk::Not)) {
      uint32_t c = unary();
      uint32_t i = (uint32_t)a.es.size();
      a.es.push_back({Ek::Not, {}, 0, 0, c, 0});
      return i;
    }
    return primary();
  }
  // postfix `[expr]` indexing on any primary: a[i], a[i][j], array(3)[0]...
  // each index is Ek::Load(base = handle expr, idx = index expr)
  constexpr uint32_t postfix(uint32_t base) {
    uint32_t lhs = base;
    while (peek().k == Tk::LBr) {
      ++ti;
      uint32_t ix = expr();
      if (!ok) return 0;
      if (!expect(Tk::RBr, "expected ']'")) return 0;
      uint32_t i = (uint32_t)a.es.size();
      a.es.push_back({Ek::Load, {}, 0, 0, lhs, ix});
      lhs = i;
    }
    return lhs;
  }
  constexpr uint32_t primary() {
    if (peek().k == Tk::Num) {
      val_t v = eat().num;
      uint32_t i = (uint32_t)a.es.size();
      a.es.push_back({Ek::Num, {}, v, 0, 0, 0});
      return postfix(i);
    }
    if (peek().k == Tk::Id) {
      Tok t = eat();
      std::string_view w = src.substr(t.lo, t.hi - t.lo);
      // contextual builtins: only special when followed by '('. a variable
      // named array/len/input stays usable (`let array = 3; print array;`)
      if (w == "array" && peek().k == Tk::LP) { // array(size)
        ++ti;
        uint32_t sz = expr();
        if (!ok) return 0;
        if (!expect(Tk::RP, "expected ')' after array size")) return 0;
        uint32_t i = (uint32_t)a.es.size();
        a.es.push_back({Ek::Alloc, {}, 0, 0, sz, 0});
        return postfix(i);
      }
      if (w == "len" && peek().k == Tk::LP) { // len(handle)
        ++ti;
        uint32_t h = expr();
        if (!ok) return 0;
        if (!expect(Tk::RP, "expected ')' after len argument")) return 0;
        uint32_t i = (uint32_t)a.es.size();
        a.es.push_back({Ek::Len, {}, 0, 0, h, 0});
        return postfix(i);
      }
      if (w == "input" && peek().k == Tk::LP) { // input()
        ++ti;
        if (!expect(Tk::RP, "expected '()'")) return 0;
        uint32_t i = (uint32_t)a.es.size();
        a.es.push_back({Ek::Input, {}, 0, 0, 0, 0});
        return postfix(i);
      }
      if (peek().k == Tk::LP) { // user function call: name(args...)
        uint32_t fi = find_hoisted(w);
        if (fi != NONE) {
          ++ti;
          std::vector<uint32_t> kids{};
          if (peek().k != Tk::RP) {
            for (;;) {
              uint32_t ae = expr();
              if (!ok) return 0;
              kids.push_back(ae);
              if (accept(Tk::Comma)) continue;
              break;
            }
          }
          if (!expect(Tk::RP, "expected ')' after arguments")) return 0;
          if (kids.size() > 6) {
            fail(t.pos, "too many arguments (max 6: SysV integer regs)");
            return 0;
          }
          a.seq_kids.push_back(std::move(kids));
          uint32_t i = (uint32_t)a.es.size();
          a.es.push_back({Ek::Call, {}, 0, fi,
                          (uint32_t)(a.seq_kids.size() - 1), 0});
          return postfix(i);
        }
      }
      uint32_t v = find_var(w, t.pos);
      if (!ok) return 0;
      if (peek().k == Tk::LP && v != 0xFFFF'FFFF) {
        fail(t.pos, "not a function (variables cannot be called)");
        return 0;
      }
      uint32_t i = (uint32_t)a.es.size();
      a.es.push_back({Ek::Var, {}, 0, v, 0, 0});
      return postfix(i);
    }
    if (accept(Tk::LP)) {
      uint32_t e = expr();
      if (!ok) return 0;
      if (!expect(Tk::RP, "expected ')'")) return 0;
      return postfix(e);
    }
    fail(peek().pos, "expected expression");
    return 0;
  }

  // block := '{' stmt* '}'  (child Seq)
  constexpr uint32_t block() {
    if (!expect(Tk::LB, "expected '{'")) return 0;
    a.scope_marks.push_back((uint32_t)a.scope.size());
    uint32_t s = stmts_until_rb();
    a.scope.resize(a.scope_marks.back());
    a.scope_marks.pop_back();
    if (!ok) return 0;
    if (!expect(Tk::RB, "expected '}'")) return 0;
    return s;
  }
  constexpr uint32_t stmts_until_rb() {
    std::vector<uint32_t> kids{};
    while (peek().k != Tk::RB && peek().k != Tk::End && ok) {
      uint32_t s = stmt();
      if (!ok) return 0;
      kids.push_back(s);
    }
    a.seq_kids.push_back(std::move(kids));
    uint32_t i = (uint32_t)a.ss.size();
    a.ss.push_back(
        {Sk::Seq, 0, 0, (uint32_t)(a.seq_kids.size() - 1), 0});
    return i;
  }
  constexpr uint32_t stmt() {
    switch (peek().k) {
    case Tk::KwRet: {
      ++ti;
      if (!in_fn) {
        fail(peek().pos, "'return' outside of a function");
        return 0;
      }
      uint32_t e = expr();
      if (!ok) return 0;
      if (!expect(Tk::Semi, "expected ';'")) return 0;
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({Sk::Ret, 0, e, 0, 0});
      return i;
    }
    case Tk::KwLet: {
      ++ti;
      if (peek().k != Tk::Id) { fail(peek().pos, "expected name after let"); return 0; }
      Tok t = eat();
      std::string_view n = src.substr(t.lo, t.hi - t.lo);
      uint32_t id = a.new_var(n);
      if (id == 0xDEAD'FFFF) { fail(t.pos, "duplicate declaration"); return 0; }
      if (!expect(Tk::Assign, "expected '='")) return 0;
      uint32_t e = expr();
      if (!ok) return 0;
      if (!expect(Tk::Semi, "expected ';'")) return 0;
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({Sk::Let, id, e, 0, 0});
      return i;
    }
    case Tk::KwPrint: {
      ++ti;
      uint32_t e = expr();
      if (!ok) return 0;
      if (!expect(Tk::Semi, "expected ';'")) return 0;
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({Sk::Print, 0, e, 0, 0});
      return i;
    }
    case Tk::KwIf: {
      ++ti;
      if (!expect(Tk::LP, "expected '('")) return 0;
      uint32_t c = expr();
      if (!ok) return 0;
      if (!expect(Tk::RP, "expected ')'")) return 0;
      uint32_t then = block();
      if (!ok) return 0;
      uint32_t els = 0;
      if (accept(Tk::KwElse)) {
        els = block();
        if (!ok) return 0;
      } else {
        // no else clause: an EMPTY Seq arm (must not alias the then arm!)
        a.seq_kids.push_back({});
        els = (uint32_t)a.ss.size();
        a.ss.push_back(
            {Sk::Seq, 0, 0, (uint32_t)(a.seq_kids.size() - 1), 0});
      }
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({Sk::If, 0, c, then, els});
      return i;
    }
    case Tk::KwBreak:
    case Tk::KwCont: {
      bool is_break = (peek().k == Tk::KwBreak);
      ++ti;
      if (loop_depth <= 0) {
        fail(peek().pos, is_break ? "'break' outside of a loop"
                                  : "'continue' outside of a loop");
        return 0;
      }
      if (!expect(Tk::Semi, "expected ';'")) return 0;
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({is_break ? Sk::Brk : Sk::Cont, 0, 0, 0, 0});
      return i;
    }
    case Tk::KwWhile: {
      ++ti;
      if (!expect(Tk::LP, "expected '('")) return 0;
      uint32_t c = expr();
      if (!ok) return 0;
      if (!expect(Tk::RP, "expected ')'")) return 0;
      ++loop_depth;
      uint32_t body = block();
      --loop_depth;
      if (!ok) return 0;
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({Sk::While, 0, c, body, 0});
      return i;
    }
    case Tk::LB:
      return block();
    case Tk::Id: {
      Tok t = eat();
      std::string_view w = src.substr(t.lo, t.hi - t.lo);
      uint32_t v = find_var(w, t.pos);
      if (!ok) return 0;
      if (peek().k == Tk::LBr) { // array store: a[i]...[k] = expr;
        std::vector<uint32_t> idx{};
        while (accept(Tk::LBr)) {
          uint32_t e = expr();
          if (!ok) return 0;
          if (!expect(Tk::RBr, "expected ']'")) return 0;
          idx.push_back(e);
        }
        if (!expect(Tk::Assign, "expected '=' in array store")) return 0;
        uint32_t val = expr();
        if (!ok) return 0;
        if (!expect(Tk::Semi, "expected ';'")) return 0;
        a.seq_kids.push_back(std::move(idx));
        uint32_t i = (uint32_t)a.ss.size();
        // var = base array var; a = seq_kids slot of index exprs; b = value
        a.ss.push_back(
            {Sk::StoreA, v, 0, (uint32_t)(a.seq_kids.size() - 1), val});
        return i;
      }
      if (!expect(Tk::Assign, "expected '=' in assignment")) return 0;
      uint32_t e = expr();
      if (!ok) return 0;
      if (!expect(Tk::Semi, "expected ';'")) return 0;
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({Sk::Assign, v, e, 0, 0});
      return i;
    }
    default:
      fail(peek().pos, "expected statement");
      return 0;
    }
  }
  // pre-scan: collect top-level fn names so calls may reference functions
  // defined LATER in the file (mutual recursion needs it). Braces only occur
  // as block delimiters in this language, so brace depth finds "top level".
  constexpr void hoist_fns() {
    uint32_t depth = 0;
    for (size_t k = 0; k < L.toks.size(); ++k) {
      Tk t = L.toks[k].k;
      if (t == Tk::LB) ++depth;
      else if (t == Tk::RB && depth) --depth;
      else if (t == Tk::KwFn && depth == 0) {
        if (k + 1 < L.toks.size() && L.toks[k + 1].k == Tk::Id) {
          const Tok &nt = L.toks[k + 1];
          std::string_view n = src.substr(nt.lo, nt.hi - nt.lo);
          for (const auto &h : a.hoisted_fns)
            if (h == n) { fail(nt.pos, "duplicate function name"); return; }
          a.hoisted_fns.push_back(n);
        }
      }
    }
  }
  constexpr uint32_t find_hoisted(std::string_view n) const {
    for (size_t i = 0; i < a.hoisted_fns.size(); ++i)
      if (a.hoisted_fns[i] == n) return (uint32_t)i;
    return NONE;
  }
  // fn name(params) { body } -- params form one scope level (body may shadow)
  constexpr bool parse_fn() {
    (void)eat(); // 'fn'
    if (peek().k != Tk::Id) {
      fail(peek().pos, "expected function name after 'fn'");
      return false;
    }
    Tok t = eat();
    std::string_view n = src.substr(t.lo, t.hi - t.lo);
    uint32_t idx = find_hoisted(n);
    if (idx == NONE) { fail(t.pos, "internal: fn not hoisted"); return false; }
    if (a.fns[idx].defined) {
      fail(t.pos, "duplicate function definition");
      return false;
    }
    if (!expect(Tk::LP, "expected '(' after function name")) return false;
    std::vector<uint32_t> params{};
    a.scope_marks.push_back((uint32_t)a.scope.size());
    if (!accept(Tk::RP)) {
      for (;;) {
        if (peek().k != Tk::Id) {
          fail(peek().pos, "expected parameter name");
          return false;
        }
        Tok pt = eat();
        std::string_view pn = src.substr(pt.lo, pt.hi - pt.lo);
        uint32_t id = a.new_var(pn);
        if (id == 0xDEAD'FFFF) {
          fail(pt.pos, "duplicate parameter name");
          return false;
        }
        params.push_back(id);
        if (accept(Tk::Comma)) continue;
        break;
      }
      if (!expect(Tk::RP, "expected ')' after parameters")) return false;
    }
    bool saved = in_fn;
    in_fn = true;
    uint32_t body = block();
    in_fn = saved;
    a.scope.resize(a.scope_marks.back());
    a.scope_marks.pop_back();
    if (!ok) return false;
    a.fns[idx] = FnDef{n, params, body, true};
    return true;
  }
  constexpr uint32_t program() {
    hoist_fns();
    if (!ok) return 0;
    a.fns.assign(a.hoisted_fns.size(), FnDef{});
    std::vector<uint32_t> kids{};
    while (peek().k != Tk::End && ok) {
      if (peek().k == Tk::KwFn) {
        if (!parse_fn()) return 0;
        continue;
      }
      uint32_t s = stmt();
      if (!ok) return 0;
      kids.push_back(s);
    }
    if (!ok) return 0;
    for (const FnDef &f : a.fns)
      if (!f.defined) { fail(0, "internal: hoisted fn never defined"); return 0; }
    if (peek().k != Tk::End) { fail(peek().pos, "unexpected token"); return 0; }
    a.seq_kids.push_back(std::move(kids));
    uint32_t i = (uint32_t)a.ss.size();
    a.ss.push_back(
        {Sk::Seq, 0, 0, (uint32_t)(a.seq_kids.size() - 1), 0});
    return i;
  }
};

constexpr Parsed parse(std::string_view src) {
  Lexed L = lex(src);
  Parsed P{};
  if (!L.ok) { P.err = {L.err.pos, L.err.what}; return P; }
  P.ast = Ast{};
  Parser ps(L, src, P.ast);
  P.ok = ps.ok;
  P.root = ps.program();
  if (!ps.ok) { P.ok = false; P.err = ps.err; }
  if (P.ast.names.empty() && P.ok) {
    // no variables at all is fine (print-only programs)
  }
  for (const Expr &e : P.ast.es)
    if (e.k == Ek::Input) { P.uses_input = true; break; }
  return P;
}
// ============================================================================
// 5. IR: three-address code in basic blocks
// ============================================================================
enum class K : uint8_t {
  Const, Copy, Bin, Br, Cbr, Print, Phi, Exit,
  Alloc, Load, Store, Len, Input,  // arrays + stdin (arena handles)
  Call, Ret                        // functions: c = callee fn index (args in
                                   // Instr::args); Ret never appears in main
};
enum class B : uint8_t {
  Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne,
  And, Or, Xor, Shl, LShr, AShr, MulHi // bitwise/shift/mulhi: created by
                                          // the optimizer (div-by-const magic);
                                          // the surface language has no syntax
                                          // for them (yet)
};
constexpr const char *bname(B b) {
  switch (b) {
  case B::Add: return "+";  case B::Sub: return "-";
  case B::Mul: return "*";  case B::Div: return "/";
  case B::Mod: return "%";  case B::Lt: return "<";
  case B::Le: return "<="; case B::Gt: return ">";
  case B::Ge: return ">="; case B::Eq: return "==";
  case B::Ne: return "!=";
  case B::And: return "&";  case B::Or: return "|";
  case B::Xor: return "^";  case B::Shl: return "shl";
  case B::LShr: return "lshr"; case B::AShr: return "ashr";
  case B::MulHi: return "mulhi";
  }
  return "?";
}
constexpr B ir_op(Bk b) {   // AST binop -> IR binop (identical enumerators)
  switch (b) {
  case Bk::Add: return B::Add;
  case Bk::Sub: return B::Sub;
  case Bk::Mul: return B::Mul;
  case Bk::Div: return B::Div;
  case Bk::Mod: return B::Mod;
  case Bk::Lt: return B::Lt;
  case Bk::Le: return B::Le;
  case Bk::Gt: return B::Gt;
  case Bk::Ge: return B::Ge;
  case Bk::Eq: return B::Eq;
  case Bk::Ne: return B::Ne;
  }
  return B::Add;
}
// one B op on values (used by the compile-time loop simulator)
constexpr Rv w_bin(B b, val_t x, val_t y) {
  switch (b) {
  case B::Add: return w_add(x, y);
  case B::Sub: return w_sub(x, y);
  case B::Mul: return w_mul(x, y);
  case B::Div: return w_div(x, y);
  case B::Mod: return w_mod(x, y);
  case B::Lt: return {x < y, false};
  case B::Le: return {x <= y, false};
  case B::Gt: return {x > y, false};
  case B::Ge: return {x >= y, false};
  case B::Eq: return {x == y, false};
  case B::Ne: return {x != y, false};
  case B::And: return w_and(x, y);
  case B::Or: return w_or(x, y);
  case B::Xor: return w_xor(x, y);
  case B::Shl: return w_shl(x, y);
  case B::LShr: return w_lshr(x, y);
  case B::AShr: return w_ashr(x, y);
  case B::MulHi: return w_mulhi(x, y);
  }
  return {0, true};
}

struct Instr {
  K k{};
  B b{};                 // Bin opcode
  uint32_t dst{NONE};    // defined vreg (NONE if none)
  uint32_t a{NONE}, b2{NONE}; // operands (NONE = unused; 0 is a VALID vreg!)
  uint32_t t{NONE}, f{NONE};  // Br target / Cbr targets; Phi: pred ids of args
  int64_t c{};           // Const value; Phi: base var id; Call: callee fn idx
  uint32_t c2{NONE};     // third operand (Store: the stored value vreg)
  // nt = "no trap PROVEN": a range/alias analysis proved this Div/Mod
  // cannot trap (divisor excludes 0, and excludes -1 or the dividend
  // excludes INT64_MIN) or this Load/Store is provably in bounds (handle
  // provably one allocation + constant index in range). Backends may then
  // skip the guard; the pass pipeline may treat the op as pure/hoistable.
  bool nt{};
  std::vector<uint32_t> args{}; // Call: argument vregs, parameter order
};
struct Block {
  uint32_t begin{}, end{};     // instrs [begin,end)
};
struct Fn {
  std::vector<Instr> code{};
  std::vector<Block> blocks{};
  std::vector<uint32_t> succ{};               // 2 per block
  std::vector<std::vector<uint32_t>> preds{}; // ascending source order
  uint32_t nvars{};                           // original (pre-SSA) vregs
  uint32_t nparams{};                         // vregs 0..nparams-1 = params
  std::vector<std::string_view> var_names{};  // base names per vreg
  std::vector<uint32_t> version_base{};       // SSA versions: base(v)
  std::vector<uint32_t> version_no{};         // SSA versions: subscript
  // Arrays provably local to this function (C backend): one entry per
  // non-escaping constant-length Alloc — `root` is the Alloc's dst (the C
  // array is named after it), `len` the element count, `vregs` every vreg
  // holding that handle (the Alloc dst + transitive Copies; destruction's
  // copy fusion may rename the defining instr to any of them). Filled by
  // analyze_stack_allocs on the SSA form; vreg ids survive destruction.
  struct StkArr {
    uint32_t root{};
    val_t len{};
    std::vector<uint32_t> vregs{};
  };
  std::vector<StkArr> stk_arrays{};

  // ---- register allocation (C + bytecode backends) ------------------
  // Physical home of each vreg AFTER SSA destruction. Empty = identity
  // (one slot per vreg, the "SSA-like" emission). When register allocation
  // is on, slot_of maps every vreg to a color of the interference graph
  // (vregs with disjoint live ranges share a slot), nslots is the color
  // count, and the first nparams slots are the parameters (the VM writes
  // call arguments into slots 0..nargs-1). No spilling is ever needed:
  // the C backend has unlimited locals and the VM has unlimited slots.
  std::vector<uint32_t> slot_of{};
  uint32_t nslots{};

  constexpr uint32_t nblocks() const { return (uint32_t)blocks.size(); }
  constexpr uint32_t succ0(uint32_t b) const { return succ[2 * b]; }
  constexpr uint32_t succ1(uint32_t b) const { return succ[2 * b + 1]; }
  constexpr uint32_t base_of(uint32_t v) const {
    return v < nvars ? v : version_base[v - nvars];
  }
  constexpr uint32_t vern_of(uint32_t v) const {
    return v < nvars ? 0 : version_no[v - nvars];
  }
  constexpr void recompute_preds() {
    preds.assign(nblocks(), {});
    for (uint32_t b = 0; b < nblocks(); ++b) {
      if (succ0(b) != NONE) preds[succ0(b)].push_back(b);
      if (succ1(b) != NONE) preds[succ1(b)].push_back(b);
    }
  }
};

// Slot view used by the C + bytecode backends. Identity (empty slot_of,
// i.e. --no-regalloc or a never-run allocator) keeps one named local per
// vreg; a filled slot_of is the interference-coloring home (see 11z).
constexpr uint32_t slot_id(const Fn &fn, uint32_t v) {
  if (v == NONE) return 0;
  if (fn.slot_of.empty()) return v;
  return v < fn.slot_of.size() ? fn.slot_of[v] : 0;
}
constexpr uint32_t slot_count(const Fn &fn) {
  if (fn.slot_of.empty())
    return fn.nvars + (uint32_t)fn.version_base.size();
  return fn.nslots ? fn.nslots : (fn.nvars + (uint32_t)fn.version_base.size());
}

// ---- 6z. multi-predecessor normalization (binary merge trees) ----------------
// A Phi in this IR carries exactly TWO arguments (keyed by the predecessor
// ids in t/f), so every merge point must have at most two predecessors.
// `continue` adds a second back edge to a loop header and `break` out of
// nested arms adds exit edges, so break/continue programs can produce 3+
// pred blocks; SSA construction would then place 2-arg phis with missing
// edges. This pass rewrites each >2-pred block's extra incoming edges
// through a right-leaning chain of fresh 2-pred merge blocks:
//     preds p0 p1 p2 .. p(k-1)  of B
//     ->  p0 -> B,  p1,p2 -> M1,  M1,p3 -> M2, .. M(k-3),p(k-1) -> M(k-2) -> B
// Each chain block is a pure `br`; standard phi placement at the IDF then
// lowers every value merge into a chain of 2-arg phis, and the loop
// analyses see a single (merge) latch again. Runs until fixpoint (each
// chain block has exactly 2 preds, so one round per offending block).
constexpr void normalize_multi_preds(Fn &fn) {
  for (bool again = true; again;) {
    again = false;
    fn.recompute_preds();
    const uint32_t nb = fn.nblocks();
    for (uint32_t b = 0; b < nb && !again; ++b) {
      if (fn.preds[b].size() <= 2) continue;
      const std::vector<uint32_t> ps = fn.preds[b]; // copy: succ mutates
      const uint32_t k = (uint32_t)ps.size();
      // fresh chain blocks M1..M(k-2) (M[j], 1-based)
      std::vector<uint32_t> M(k, NONE);
      for (uint32_t j = 1; j + 1 < k; ++j) {
        M[j] = fn.nblocks();
        fn.blocks.push_back({});
        fn.succ.push_back(NONE);
        fn.succ.push_back(NONE);
      }
      // their code (appended; existing ranges stay valid)
      for (uint32_t j = 1; j + 1 < k; ++j) {
        uint32_t tgt = (j + 2 == k) ? b : M[j + 1];
        fn.blocks[M[j]].begin = (uint32_t)fn.code.size();
        fn.code.push_back({.k = K::Br, .t = tgt});
        fn.blocks[M[j]].end = (uint32_t)fn.code.size();
        fn.succ[2 * M[j]] = tgt;
      }
      // redirect the extra predecessor edges: p1,p2 -> M1; pj -> M(j-1)
      auto redirect = [&](uint32_t p, uint32_t to) {
        for (int s = 0; s < 2; ++s)
          if (fn.succ[2 * p + (uint32_t)s] == b) {
            fn.succ[2 * p + (uint32_t)s] = to;
            for (uint32_t i = fn.blocks[p].begin; i < fn.blocks[p].end;
                 ++i) {
              Instr &I = fn.code[i];
              if (I.k == K::Br && I.t == b)
                I.t = to;
              else if (I.k == K::Cbr) {
                if (I.t == b) I.t = to;
                if (I.f == b) I.f = to;
              }
            }
          }
      };
      redirect(ps[1], M[1]);
      if (k > 2) redirect(ps[2], M[1]);
      for (uint32_t j = 3; j < k; ++j) redirect(ps[j], M[j - 1]);
      again = true; // rescan with fresh preds
    }
  }
  fn.recompute_preds();
}

// ============================================================================
// 6. CFG builder: AST -> IR with short-circuit lowering.
//    Pre-SSA: each source variable is one vreg with possibly many defs;
//    temps are single-def except short-circuit results (two Const defs),
//    which the SSA pass turns into phis via standard IDF placement.
// ============================================================================
struct Builder {
  Fn &fn;
  uint32_t cur{NONE};
  // innermost-first stack of (header, exit) for `break`/`continue` targets
  std::vector<std::pair<uint32_t, uint32_t>> loop_stack{};
  // AST var id -> LOCAL vreg id. Each function compiles into its own vreg
  // space: params are vregs 0..P-1, then body locals and temps. (AST ids are
  // global across the whole program, so every reference goes through this.)
  std::vector<uint32_t> vmap{};

  constexpr Builder(Fn &f) : fn(f) {}

  constexpr uint32_t map_def(uint32_t ast_var, const Ast &a) {
    if (vmap[ast_var] == NONE) {
      vmap[ast_var] = fn.nvars++;
      fn.var_names.push_back(a.names[ast_var]);
    }
    return vmap[ast_var];
  }
  constexpr uint32_t fresh_temp() {
    uint32_t v = fn.nvars++;
    fn.var_names.push_back({});
    return v;
  }
  constexpr uint32_t new_block() {
    uint32_t b = fn.nblocks();
    fn.blocks.push_back({});
    fn.blocks.back().begin = NONE; // sentinel: real begin set on first emit
    fn.succ.push_back(NONE);
    fn.succ.push_back(NONE);
    return b;
  }
  constexpr void emit(Instr I) {
    if (fn.blocks[cur].begin == NONE)
      fn.blocks[cur].begin = (uint32_t)fn.code.size();
    fn.code.push_back(I);
  }
  constexpr void finish(uint32_t s0, uint32_t s1) {
    uint32_t b = cur;
    if (fn.blocks[b].begin == NONE)
      fn.blocks[b].begin = (uint32_t)fn.code.size(); // empty block
    fn.blocks[b].end = (uint32_t)fn.code.size();
    fn.succ[2 * b] = s0;
    fn.succ[2 * b + 1] = s1;
    cur = NONE;
  }
  constexpr void close_br(uint32_t tgt) {
    emit({.k = K::Br, .t = tgt});
    finish(tgt, NONE);
  }
  constexpr void close_cbr(uint32_t c, uint32_t tt, uint32_t ff) {
    emit({.k = K::Cbr, .a = c, .t = tt, .f = ff});
    finish(tt, ff);
  }
  constexpr void close_exit() {
    emit({.k = K::Exit});
    finish(NONE, NONE);
  }
  constexpr void ensure_open() {
    if (cur == NONE) cur = new_block();
  }
  constexpr uint32_t const_val(val_t v) {
    uint32_t d = fresh_temp();
    emit({.k = K::Const, .dst = d, .c = v});
    return d;
  }

  // returns vreg holding the expression value
  constexpr uint32_t ex(uint32_t ei, const Ast &a) {
    const Expr &e = a.es[ei];
    switch (e.k) {
    case Ek::Num: return const_val(e.num);
    case Ek::Var:
      // A use with no local mapping yet happens for `let x = ... x ...`
      // (the initializer sees the NEW binding, C-style). Storage is
      // zero-initialized, exactly like the AST oracle's env -- read 0.
      if (vmap[e.var] == NONE) return const_val(0);
      return vmap[e.var];
    case Ek::Neg: {
      uint32_t x = ex(e.l, a);
      uint32_t z = const_val(0);
      uint32_t d = fresh_temp();
      emit({.k = K::Bin, .b = B::Sub, .dst = d, .a = z, .b2 = x});
      return d;
    }
    case Ek::Not: {
      uint32_t x = ex(e.l, a);
      uint32_t z = const_val(0);
      uint32_t d = fresh_temp();
      emit({.k = K::Bin, .b = B::Eq, .dst = d, .a = x, .b2 = z});
      return d;
    }
    case Ek::Bin: {
      uint32_t x = ex(e.l, a);
      uint32_t y = ex(e.r, a);
      uint32_t d = fresh_temp();
      emit({.k = K::Bin, .b = ir_op(e.b), .dst = d, .a = x, .b2 = y});
      return d;
    }
    case Ek::Alloc: { // d = alloc(n): fresh handle into the bump arena
      uint32_t n = ex(e.l, a);
      uint32_t d = fresh_temp();
      emit({.k = K::Alloc, .dst = d, .a = n});
      return d;
    }
    case Ek::Load: { // d = h[i] (bounds-checked, TRAPs when out of range)
      uint32_t h = ex(e.l, a);
      uint32_t i = ex(e.r, a);
      uint32_t d = fresh_temp();
      emit({.k = K::Load, .dst = d, .a = h, .b2 = i});
      return d;
    }
    case Ek::Len: { // d = len(h): 0 unless h is an allocated handle
      uint32_t h = ex(e.l, a);
      uint32_t d = fresh_temp();
      emit({.k = K::Len, .dst = d, .a = h});
      return d;
    }
    case Ek::Input: { // d = input(): next integer from the stdin tape
      uint32_t d = fresh_temp();
      emit({.k = K::Input, .dst = d});
      return d;
    }
    case Ek::Call: { // d = f_k(a0, ..): args evaluated left to right
      std::vector<uint32_t> argv{};
      for (uint32_t ke : a.seq_kids[e.l]) argv.push_back(ex(ke, a));
      uint32_t d = fresh_temp();
      Instr ci{.k = K::Call, .dst = d, .c = (int64_t)e.var + 1};
      ci.args = std::move(argv);
      emit(ci);
      return d;
    }
    case Ek::And: {
      // short-circuit: l ? r : 0  — diamond, phi appears via SSA/IDF
      uint32_t res = fresh_temp();
      uint32_t l = ex(e.l, a);
      uint32_t rb = new_block(), tt = new_block(), ff = new_block(),
               mm = new_block();
      close_cbr(l, rb, ff);
      cur = rb;
      uint32_t r = ex(e.r, a);
      close_cbr(r, tt, ff);
      cur = tt; emit({.k = K::Const, .dst = res, .c = 1}); close_br(mm);
      cur = ff; emit({.k = K::Const, .dst = res, .c = 0}); close_br(mm);
      cur = mm;
      return res;
    }
    case Ek::Or: {
      uint32_t res = fresh_temp();
      uint32_t l = ex(e.l, a);
      uint32_t rb = new_block(), tt = new_block(), ff = new_block(),
               mm = new_block();
      close_cbr(l, tt, rb);        // true short-circuits
      cur = rb;
      uint32_t r = ex(e.r, a);
      close_cbr(r, tt, ff);
      cur = tt; emit({.k = K::Const, .dst = res, .c = 1}); close_br(mm);
      cur = ff; emit({.k = K::Const, .dst = res, .c = 0}); close_br(mm);
      cur = mm;
      return res;
    }
    }
    return NONE;
  }

  constexpr void st(uint32_t si, const Ast &a) {
    const Stmt &s = a.ss[si];
    switch (s.k) {
    case Sk::Let:
    case Sk::Assign: {
      ensure_open();
      uint32_t v = ex(s.e, a);
      emit({.k = K::Copy, .dst = map_def(s.var, a), .a = v});
      break;
    }
    case Sk::Print: {
      ensure_open();
      uint32_t v = ex(s.e, a);
      emit({.k = K::Print, .a = v});
      break;
    }
    case Sk::Ret: { // return expr;  -- terminates the block (no successors)
      ensure_open();
      uint32_t v = ex(s.e, a);
      emit({.k = K::Ret, .a = v});
      finish(NONE, NONE);
      break;
    }
    case Sk::StoreA: { // a[i1]...[ik] = value:  loads for k-1 indices, then Store
      ensure_open();
      const auto &idx = a.seq_kids[s.a];
      uint32_t h = vmap[s.var];         // base array variable (a handle vreg)
      for (size_t j = 0; j + 1 < idx.size(); ++j) {
        uint32_t i = ex(idx[j], a);     // index exprs left-to-right,
        uint32_t t = fresh_temp();      // intermediate handle loads
        emit({.k = K::Load, .dst = t, .a = h, .b2 = i});
        h = t;
      }
      uint32_t last = ex(idx.back(), a);
      uint32_t v = ex(s.b, a);          // value evaluated last (oracle order)
      emit({.k = K::Store, .a = h, .b2 = last, .c2 = v});
      break;
    }
    case Sk::Seq:
      for (uint32_t x : a.seq_kids[s.a]) st(x, a);
      break;
    case Sk::If: {
      ensure_open();
      uint32_t c = ex(s.e, a);
      uint32_t tb = new_block(), eb = new_block(), jb = new_block();
      close_cbr(c, tb, eb);
      cur = tb;
      st(s.a, a);
      ensure_open();
      close_br(jb);
      cur = eb;
      st(s.b, a);
      ensure_open();
      close_br(jb);
      cur = jb;
      break;
    }
    case Sk::Brk: { // break; -> unconditional jump to the loop's exit
      ensure_open();
      close_br(loop_stack.back().second);
      break;
    }
    case Sk::Cont: { // continue; -> jump back to the header (re-test)
      ensure_open();
      close_br(loop_stack.back().first);
      break;
    }
    case Sk::While: {
      ensure_open();
      uint32_t h = new_block(), body = new_block(), after = new_block();
      close_br(h);                       // preheader -> header
      cur = h;
      uint32_t c = ex(s.e, a);
      close_cbr(c, body, after);
      cur = body;
      loop_stack.push_back({h, after});
      st(s.a, a);
      loop_stack.pop_back();
      ensure_open();
      close_br(h);                       // back edge
      cur = after;
      break;
    }
    }
  }

  constexpr void start(const Ast &a) {
    fn.nvars = 0;
    fn.var_names.clear();
    fn.nparams = 0;
    vmap.assign(a.names.size(), NONE);
    new_block(); // entry = block 0
    cur = 0;
  }
  constexpr void build_main(const Ast &a, uint32_t root) {
    start(a);
    st(root, a);
    ensure_open();
    close_exit();
    fn.recompute_preds();
    normalize_multi_preds(fn); // break/continue can create 3+ pred blocks
  }
  constexpr void build_fn(const Ast &a, const FnDef &fd) {
    start(a);
    fn.nparams = (uint32_t)fd.params.size();
    for (uint32_t p : fd.params) { // params are the first vregs
      vmap[p] = fn.nvars++;
      fn.var_names.push_back(a.names[p]);
    }
    st(fd.body, a);
    ensure_open();
    // control falling off the end returns 0 (the block may be unreachable
    // when every path already returned -- unreachable removal cleans it)
    uint32_t z = const_val(0);
    emit({.k = K::Ret, .a = z});
    finish(NONE, NONE);
    fn.recompute_preds();
    normalize_multi_preds(fn);
  }
};

// ============================================================================
// 7. analysis driver — all the graph math comes from mjz::graph_ns
// ============================================================================
struct An {
  std::vector<std::vector<ulen>> adjs{}, prs{};
  gns::basic_forest_t<V, std::vector<ulen>> fs{}, fp{};
  std::vector<ulen> idom{}, ib{}, ie{};
  gns::basic_forest_t<V, std::vector<ulen>> dom_tree{};
  gns::basic_forest_t<V, std::vector<ulen>> loops{};
  std::vector<ulen> inner{}, outer{}, nest{};
  gns::basic_forest_t<V, std::vector<ulen>> df{};
  uint32_t n{};

  constexpr bool dominates(uint32_t d, uint32_t s) const {
    if (d >= n || s >= n) return false;
    if (ib[d] == 0 || ib[s] == 0) return false;
    return ib[d] <= ib[s] && ie[d] >= ie[s];
  }
};

constexpr An analyze(const Fn &fn) {
  An A{};
  A.n = fn.nblocks();
  A.adjs.assign(A.n, {});
  A.prs.assign(A.n, {});
  for (uint32_t b = 0; b < A.n; ++b) {
    if (fn.succ0(b) != NONE) A.adjs[b].push_back(fn.succ0(b));
    if (fn.succ1(b) != NONE) A.adjs[b].push_back(fn.succ1(b));
    for (uint32_t p : fn.preds[b]) A.prs[b].push_back(p);
  }
  // -- everything below is mjz_lib doing the heavy lifting --
  A.fs = gns::make_basic_forest<V>(A.adjs);            // CSR successor graph
  A.fp = gns::make_basic_inv_forest<V>(A.adjs);        // CSR predecessor graph
  A.idom = gns::calculate_dominators_from_entry<V>(0, A.fs, A.fp); // Lengauer–Tarjan
  A.dom_tree = gns::make_dominator_forest_given_dominators<V>(A.idom);
  auto iv = gns::calculate_dominance_intervals_given_dominators_and_forest<V>(
      A.dom_tree, A.idom);
  A.ib = iv.first;
  A.ie = iv.second;
  A.loops =
      gns::calculate_natural_loops_with_header_given_dominance_intervals<V>(
          A.ib, A.ie, A.fp, true);
  A.inner.assign(A.n, ulen(-1));
  A.outer.assign(A.n, 0);
  A.nest.assign(A.n, 0);
  gns::calculate_nesting_of_natural_loop_headers<V>(A.loops, A.inner, A.outer,
                                                    A.nest);
  A.df = gns::calculate_dominance_frontiers_given_dominators<V>(A.idom, A.fp);
  return A;
}

// ============================================================================
// 8. SSA construction
//    - phi placement at the iterated dominance frontier of each def set
//      (mjz::graph_ns::calculate_iterated_dominance_frontiers_...)
//    - renaming by dominator-tree walk (dom tree from the lib)
// ============================================================================
struct SsaStats {
  uint32_t phis{};
  uint32_t versions{};
};

constexpr bool ssa_construct(Fn &fn, const An &A, SsaStats &st) {
  st = {};
  const uint32_t nb = fn.nblocks();
  // ---- 8a. def blocks per original vreg
  std::vector<std::vector<ulen>> defs(fn.nvars);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst != NONE && fn.code[i].k != K::Phi)
        defs[fn.code[i].dst].push_back(b);
  // parameters are "defined at entry" (block 0): uses before any assignment
  // see the incoming argument; re-assignments get ordinary phi placement
  for (uint32_t v = 0; v < fn.nparams; ++v) defs[v].push_back(0);
  // ---- 8b. phi insertion at the IDF of each multi-def vreg's def set
  std::vector<std::vector<uint32_t>> phis_at(nb);
  for (uint32_t v = 0; v < fn.nvars; ++v) {
    if (defs[v].size() < 2) continue;
    std::vector<ulen> idf =
        gns::calculate_iterated_dominance_frontiers_given_dominance_frontiers<
            V>(A.df, defs[v]);
    for (ulen blk : idf) {
      uint32_t b = (uint32_t)blk;
      if (b >= nb || fn.preds[b].size() < 2) continue;
      bool dup = false;
      for (uint32_t x : phis_at[b]) dup |= (x == v);
      if (!dup) phis_at[b].push_back(v);
    }
  }
  // ---- 8c. rebuild the code vector with phis at block starts
  {
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size() + 32);
    for (uint32_t b = 0; b < nb; ++b) {
      uint32_t old_begin = fn.blocks[b].begin;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t v : phis_at[b]) {
        nc.push_back({.k = K::Phi, .a = NONE, .b2 = NONE, .c = (int64_t)v});
        st.phis++;
      }
      for (uint32_t i = old_begin; i < fn.blocks[b].end; ++i)
        nc.push_back(fn.code[i]);
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
  // ---- 8d. renaming: dominator-tree DFS with per-base version stacks
  fn.version_base.clear();
  fn.version_no.clear();
  std::vector<std::vector<uint32_t>> stacks(fn.nvars);
  for (uint32_t v = 0; v < fn.nparams; ++v)
    stacks[v].push_back(v); // version 0 of a param is the param itself
  auto fresh = [&](uint32_t base) -> uint32_t {
    uint32_t v = fn.nvars + (uint32_t)fn.version_base.size();
    fn.version_base.push_back(base);
    fn.version_no.push_back((uint32_t)stacks[base].size());
    st.versions++;
    return v;
  };
  // phi dsts are fresh versions, pushed when visiting their block
  // explicit-stack DFS over the dominator tree (children rows from the lib)
  struct Frame {
    uint32_t block, child_idx, pushed;
  };
  std::vector<Frame> stack;
  // visit block 0
  auto visit_prologue = [&](uint32_t b, uint32_t &pushed) -> bool {
    pushed = 0;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Phi) {
        uint32_t base = (uint32_t)I.c;
        I.dst = fresh(base);
        stacks[base].push_back(I.dst);
        ++pushed;
        continue;
      }
      // rewrite uses
      if (I.a != NONE && I.a < fn.nvars) {
        auto &s = stacks[I.a];
        if (s.empty()) return false; // use before def (should not happen)
        I.a = s.back();
      }
      if (I.b2 != NONE && I.b2 < fn.nvars) {
        auto &s = stacks[I.b2];
        if (s.empty()) return false;
        I.b2 = s.back();
      }
      if (I.c2 != NONE && I.c2 < fn.nvars) { // Store: the stored value
        auto &s = stacks[I.c2];
        if (s.empty()) return false;
        I.c2 = s.back();
      }
      for (uint32_t &av : I.args) { // Call arguments
        if (av != NONE && av < fn.nvars) {
          auto &s = stacks[av];
          if (s.empty()) return false;
          av = s.back();
        }
      }
      if (I.dst != NONE) {
        uint32_t base = I.dst;
        I.dst = fresh(base);
        stacks[base].push_back(I.dst);
        ++pushed;
      }
    }
    // seed phi args in successors
    for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
      if (s == NONE) continue;
      for (uint32_t i = fn.blocks[s].begin; i < fn.blocks[s].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi) break;
        uint32_t base = (uint32_t)I.c;
        const auto &st_ = stacks[base];
        // find slot of b in preds[s]
        uint32_t slot = NONE;
        for (uint32_t j = 0; j < fn.preds[s].size(); ++j)
          if (fn.preds[s][j] == b) slot = j;
        if (slot == NONE) return false;
        // Minimal (non-pruned) SSA: a phi arg may be UNDEFINED on an edge
        // (variable not live-in there). Scoping guarantees it is never read
        // on that path, so record the pred with arg NONE; DCE drops the phi.
        uint32_t arg = st_.empty() ? NONE : st_.back();
        // record the pred block id with the arg (t/f unused by Phi) so that
        // later CFG edits can prune phi args by pred identity
        if (slot == 0) { I.a = arg; I.t = b; }
        else           { I.b2 = arg; I.f = b; }
      }
    }
    return true;
  };
  auto visit_epilogue = [&](uint32_t b, uint32_t pushed) {
    // pop exactly `pushed` entries (one per def the prologue pushed; the
    // count equals the block's defining instrs — decrement ONLY on pops)
    for (uint32_t i = fn.blocks[b].begin;
         i < fn.blocks[b].end && pushed; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Phi) {
        stacks[(uint32_t)I.c].pop_back();
        --pushed;
      } else if (I.dst != NONE) {
        stacks[fn.base_of(I.dst)].pop_back();
        --pushed;
      }
    }
  };
  {
    uint32_t pushed0 = 0;
    if (!visit_prologue(0, pushed0)) return false;
    stack.push_back({0, 0, pushed0});
    auto children_of = [&](uint32_t b) {
      return A.dom_tree.range()[b];
    };
    while (!stack.empty()) {
      Frame &fr = stack.back();
      auto ch = children_of(fr.block);
      if (fr.child_idx < (uint32_t)ch.size()) {
        uint32_t next = (uint32_t)ch[fr.child_idx++];
        uint32_t pushed = 0;
        if (!visit_prologue(next, pushed)) return false;
        stack.push_back({next, 0, pushed});
      } else {
        visit_epilogue(fr.block, fr.pushed);
        stack.pop_back();
      }
    }
  }
  return true;
}
// ============================================================================
// 9. optimizer (all on SSA; analyses from the lib are recomputed on demand)
// ============================================================================
// fresh anonymous vreg (used by destruction temps, unroll copies, inlining)
constexpr uint32_t new_anon_vreg(Fn &fn, uint32_t base) {
  uint32_t v = fn.nvars + (uint32_t)fn.version_base.size();
  fn.version_base.push_back(base);
  fn.version_no.push_back(0x10000 + (uint32_t)fn.version_base.size());
  return v;
}

struct OptStats {
  uint32_t folds{};      // instrs turned into Const
  uint32_t copies{};     // instrs turned into Copy
  uint32_t brs_folded{}; // Cbr turned into Br
  uint32_t blocks_gone{};// unreachable blocks removed
  uint32_t dce{};        // dead instrs removed
  uint32_t hoisted{};    // LICM hoists
  uint32_t copyprops{};  // copies propagated away (uses rewritten to source)
  uint32_t cses{};       // recomputations replaced by dominating defs
  uint32_t splices{};    // single-pred/succ block pairs merged
  uint32_t cbr_same{};   // Cbr with identical arms turned into Br
  uint32_t phi_degens{}; // phis reduced to Copy/Const (dead pred / single pred)
  uint32_t canon_swaps{}; // commutative ops put in canonical operand order
  uint32_t canon_cmps{};  // >/>= comparisons rewritten to the </<= direction
  uint32_t inlines{};    // call sites inlined (callee body spliced into caller)
  uint32_t unrolls{};    // loops fully unrolled (constant trip count)
  uint32_t iters{};      // fixpoint iterations of the pass pipeline
  uint32_t reassocs{};   // +/-/*c chains reassociated to canonical forms
  uint32_t reassoc_gone{}; // instructions removed by reassociation
  uint32_t lcf_loops{};  // loops replaced by closed-form series evaluation
  uint32_t lcf_sims{};   // loops replaced by compile-time simulation
  uint32_t lcf_rt{};     // loops replaced by RUNTIME-trip-count closed forms
  uint32_t divmagics{};  // div/mod-by-const -> mulhi/shift/bitops lowerings
  uint32_t notcombs{};   // !(cmp) / (cmp)==0 / (cmp)!=1 compare negations folded
  uint32_t traps_killed{}; // div/mod/load/store traps PROVEN impossible (nt)
  uint32_t mem2reg_lifts{}; // arrays lifted to SSA vregs (registers)
  uint32_t mem2reg_dses{};  // stores deleted for never-read (dead) arrays
  uint32_t lcf_gensims{};  // loops folded by the general (any-condition) simulator
};

// --- 9a. constant folding / propagation / branch folding / identities -------
// known: vreg -> const value. iterates to fixpoint.
constexpr bool opt_constprop(Fn &fn, OptStats &os) {
  bool changed_any = false;
  for (int round = 0; round < 12; ++round) {
    bool changed = false;
    std::vector<char> known;
    std::vector<val_t> kval;
    auto is_known = [&](uint32_t v, val_t &out) -> bool {
      if (v == NONE) return false;
      if (v < known.size() && known[v]) { out = kval[v]; return true; }
      return false;
    };
    // collect consts
    known.assign(fn.nvars + fn.version_base.size(), 0);
    kval.assign(fn.nvars + fn.version_base.size(), 0);
    for (const Instr &I : fn.code)
      if (I.k == K::Const) { known[I.dst] = 1; kval[I.dst] = I.c; }
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        val_t av{}, bv{};
        bool ka = is_known(I.a, av);
        bool kb = is_known(I.b2, bv);
        switch (I.k) {
        case K::Const: break;
        case K::Copy:
          if (ka) { I.k = K::Const; I.c = av; os.folds++; changed = true; }
          break;
        case K::Bin: {
          // algebraic identities (never introduce traps: the operand-defining
          // instrs still execute; we only recompute THIS instr's value)
          if (I.b == B::Add || I.b == B::Sub) {
            if (kb && bv == 0) { // x+0 = x ; x-0 = x
              I.k = K::Copy; os.copies++; changed = true; break;
            }
            if (ka && av == 0 && I.b == B::Add) { // 0+x = x
              I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break;
            }
          }
          if (I.b == B::Mul) {
            if ((ka && av == 0) || (kb && bv == 0)) {
              I.k = K::Const; I.c = 0; os.folds++; changed = true; break;
            }
            if (ka && av == 1) { I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break; }
            if (kb && bv == 1) { I.k = K::Copy; os.copies++; changed = true; break; }
          }
          if (I.b == B::Div || I.b == B::Mod) {
            if (kb && bv == 1) { // x/1 = x ; x%1 = 0 (divisor known: no trap)
              if (I.b == B::Div) { I.k = K::Copy; os.copies++; }
              else { I.k = K::Const; I.c = 0; os.folds++; }
              changed = true; break;
            }
          }
          if (I.b == B::And) { // x & 0 = 0 ; x & -1 = x
            if ((ka && av == 0) || (kb && bv == 0)) {
              I.k = K::Const; I.c = 0; os.folds++; changed = true; break;
            }
            if (ka && av == -1) { I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break; }
            if (kb && bv == -1) { I.k = K::Copy; os.copies++; changed = true; break; }
          }
          if (I.b == B::Or) { // x | 0 = x ; x | -1 = -1
            if (ka && av == -1) { I.k = K::Const; I.c = -1; os.folds++; changed = true; break; }
            if (kb && bv == -1) { I.k = K::Const; I.c = -1; os.folds++; changed = true; break; }
            if (ka && av == 0) { I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break; }
            if (kb && bv == 0) { I.k = K::Copy; os.copies++; changed = true; break; }
          }
          if (I.b == B::Xor) { // x ^ 0 = x (x ^ x handled below)
            if (ka && av == 0) { I.k = K::Copy; I.a = I.b2; os.copies++; changed = true; break; }
            if (kb && bv == 0) { I.k = K::Copy; os.copies++; changed = true; break; }
          }
          if (I.b == B::Shl || I.b == B::LShr || I.b == B::AShr) {
            if (kb && (bv & 63) == 0) { // shift by 0: identity
              I.k = K::Copy; os.copies++; changed = true; break;
            }
          }
          if (I.b == B::MulHi) {
            if ((ka && av == 0) || (kb && bv == 0)) {
              I.k = K::Const; I.c = 0; os.folds++; changed = true; break;
            }
          }
          if (I.a != NONE && I.a == I.b2) { // same vreg both sides (no traps)
            int which = -1; // -1 keep, 0 -> Const 0, 1 -> Const 1
            switch (I.b) {
            case B::Sub: case B::Lt: case B::Gt: which = 0; break;
            case B::Eq: case B::Le: case B::Ge: which = 1; break;
            case B::Xor: which = 0; break; // x ^ x = 0
            default: break; // Add/Mul/Div/Mod/Ne handled elsewhere or unsafe
            }
            if (I.b == B::Ne) which = 0; // x != x is always false
            if (which >= 0) {
              I.k = K::Const; I.c = which;
              os.folds++; changed = true; break;
            }
          }
          if (ka && kb) {
            Rv r{};
            switch (I.b) {
            case B::Add: r = w_add(av, bv); break;
            case B::Sub: r = w_sub(av, bv); break;
            case B::Mul: r = w_mul(av, bv); break;
            case B::Div: r = w_div(av, bv); break;
            case B::Mod: r = w_mod(av, bv); break;
            case B::Lt: r = {av < bv, false}; break;
            case B::Le: r = {av <= bv, false}; break;
            case B::Gt: r = {av > bv, false}; break;
            case B::Ge: r = {av >= bv, false}; break;
            case B::Eq: r = {av == bv, false}; break;
            case B::Ne: r = {av != bv, false}; break;
            case B::And: r = w_and(av, bv); break;
            case B::Or: r = w_or(av, bv); break;
            case B::Xor: r = w_xor(av, bv); break;
            case B::Shl: r = w_shl(av, bv); break;
            case B::LShr: r = w_lshr(av, bv); break;
            case B::AShr: r = w_ashr(av, bv); break;
            case B::MulHi: r = w_mulhi(av, bv); break;
            }
            if (!r.trap) { // trapping folds stay unfolded: runtime trap preserved
              I.k = K::Const; I.c = r.v; os.folds++; changed = true;
            }
          }
          break;
        }
        case K::Phi: {
          // all args the same const -> const ; all args identical -> copy
          uint32_t x = I.a, y = I.b2;
          if (x != NONE && y != NONE) {
            if (x == y) { // both same vreg: copy
              I.k = K::Copy; I.a = x; os.copies++; changed = true; break;
            }
            val_t xv{}, yv{};
            if (is_known(x, xv) && is_known(y, yv) && xv == yv) {
              I.k = K::Const; I.c = xv; os.folds++; changed = true; break;
            }
          }
          break;
        }
        case K::Cbr: {
          if (I.t == I.f && I.t != NONE) { // both arms identical: unconditional
            I.k = K::Br;
            I.f = NONE;
            fn.succ[2 * b + 1] = NONE;
            os.cbr_same++;
            changed = true;
            break;
          }
          if (ka) { // fold branch
            I.k = K::Br;
            I.t = (av != 0) ? I.t : I.f;
            I.f = NONE;
            fn.succ[2 * b] = I.t;
            fn.succ[2 * b + 1] = NONE;
            os.brs_folded++; changed = true;
          }
          break;
        }
        default: break;
        }
      }
    }
    if (!changed) break;
    changed_any = true;
  }
  return changed_any;
}

// --- 9a2. copy propagation ----------------------------------------------------
// `d = s` (Copy): every use of d becomes s and the copy dies. SSA gives each
// vreg exactly one def, and the copy's source dominates the copy, hence the
// source dominates every replaced use — always sound. This is the pass that
// cleans up phi-to-copy degenerations and lets CSE see canonical names.
constexpr bool opt_copyprop(Fn &fn, OptStats &os) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<uint32_t> ren(nv, NONE);
  auto resolve = [&](uint32_t v) -> uint32_t {
    while (v != NONE && v < nv && ren[v] != NONE) v = ren[v];
    return v;
  };
  bool had = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Copy && I.dst != NONE && I.a != NONE && I.a != I.dst) {
        ren[I.dst] = resolve(I.a); // sources resolve transitively: chains die
        had = true;
      }
    }
  if (!had) return false;
  bool changed = false;
  std::vector<char> drop(fn.code.size(), 0);
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Copy && I.dst != NONE &&
          (I.a == I.dst || ren[I.dst] != NONE)) { // self copy or propagated
        drop[i] = 1;
        if (I.a != I.dst) os.copyprops++;
        changed = true;
        continue;
      }
      if (I.a != NONE) {
        uint32_t r = resolve(I.a);
        changed |= (r != I.a);
        I.a = r;
      }
      if (I.b2 != NONE) {
        uint32_t r = resolve(I.b2);
        changed |= (r != I.b2);
        I.b2 = r;
      }
      if (I.c2 != NONE) { // Store: the stored value
        uint32_t r = resolve(I.c2);
        changed |= (r != I.c2);
        I.c2 = r;
      }
      for (uint32_t &av : I.args) { // Call arguments
        uint32_t r = resolve(av);
        changed |= (r != av);
        av = r;
      }
    }
  if (changed) { // sweep dropped copies, preserving block ranges
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size());
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t i = ob; i < oe; ++i)
        if (!drop[i]) nc.push_back(fn.code[i]);
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
  return changed;
}

// --- 9a4. canonicalization ----------------------------------------------------
// Puts every pure Bin into ONE canonical shape so equivalent computations
// become identical (which is what makes CSE hit) and identities match:
//   * comparisons: `a > b` -> `b < a`, `a >= b` -> `b <= a`  (one direction)
//   * commutative ops (+ * == !=): known-constant operand on the RIGHT,
//     otherwise the smaller vreg id first — exactly the key order the CSE
//     pass uses, so a canonical instruction always enters the CSE table in
//     canonical order (`7*x` and `x*7` unify; `a>b` and `b<a` unify).
// Comparisons never trap and swapping commutative operands preserves
// wrapping two's-complement results, so both rewrites are always sound.
// The pass is idempotent and strictly progress-making (no oscillation).
constexpr bool opt_canon(Fn &fn, OptStats &os) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  auto key = [&](uint32_t v) -> uint64_t {
    if (v == NONE) return 0;
    if (v < nv && kc[v]) return ((uint64_t)1 << 63) ^ (uint64_t)kv[v];
    return (uint64_t)v;
  };
  bool changed = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k != K::Bin) continue;
      if (I.b == B::Gt || I.b == B::Ge) { // canonical comparison direction
        std::swap(I.a, I.b2);
        I.b = (I.b == B::Gt) ? B::Lt : B::Le;
        os.canon_cmps++;
        changed = true;
      } else if (I.b == B::Add || I.b == B::Mul || I.b == B::Eq ||
                 I.b == B::Ne || I.b == B::And || I.b == B::Or ||
                 I.b == B::Xor || I.b == B::MulHi) { // commutative canonical order
        if (key(I.b2) < key(I.a)) {
          std::swap(I.a, I.b2);
          os.canon_swaps++;
          changed = true;
        }
      }
    }
  // ---- NOT-of-compare combination ---------------------------------------
  // A comparison result is exactly 0/1, so testing it against 0/1 is just
  // the comparison (or its negation):
  //     t2 = (t1 == 0)  <->  t2 = <inverted t1 compare>   (that's !t1)
  //     t2 = (t1 != 0)  <->  t2 = <t1 compare, new dst>
  // (== 1 / != 1 likewise). Combined with branch fusion this is what turns
  // `if (!((a != b))) goto L;` into `if (a == b) goto L;` -- the negation of
  // a compare is trivially combinable into the compare itself.
  {
    std::vector<uint32_t> defi(nv, NONE), ndefs(nv, 0), uses(nv, 0);
    for (uint32_t i = 0; i < fn.code.size(); ++i) {
      const Instr &I = fn.code[i];
      auto bump = [&](uint32_t v) {
        if (v != NONE && v < nv) ++uses[v];
      };
      bump(I.a);
      bump(I.b2);
      bump(I.c2);
      for (uint32_t v : I.args) bump(v);
      if (I.dst != NONE && I.dst < nv) {
        ++ndefs[I.dst];
        defi[I.dst] = i;
      }
    }
    auto inv = [](B b) -> B {
      switch (b) {
      case B::Lt: return B::Ge;
      case B::Le: return B::Gt;
      case B::Gt: return B::Lt;
      case B::Ge: return B::Le;
      case B::Eq: return B::Ne;
      case B::Ne: return B::Eq;
      default: return b;
      }
    };
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Bin || (I.b != B::Eq && I.b != B::Ne)) continue;
        if (I.b2 == NONE || I.b2 >= nv || !kc[I.b2]) continue; // const right
        val_t cv = kv[I.b2];
        if (cv != 0 && cv != 1) continue;
        uint32_t t1 = I.a;
        if (t1 == NONE || t1 >= nv) continue;
        uint32_t d = defi[t1];
        if (d == NONE || ndefs[t1] != 1 || uses[t1] != 1) continue;
        const Instr &D = fn.code[d];
        if (D.k != K::Bin) continue;
        if (D.b != B::Lt && D.b != B::Le && D.b != B::Gt && D.b != B::Ge &&
            D.b != B::Eq && D.b != B::Ne)
          continue;
        bool invert = (I.b == B::Eq) ? (cv == 0) : (cv == 1);
        I.b = invert ? inv(D.b) : D.b;
        I.a = D.a;
        I.b2 = D.b2;
        if ((I.b == B::Eq || I.b == B::Ne) && key(I.b2) < key(I.a))
          std::swap(I.a, I.b2); // keep the canonical operand order
        os.notcombs++;
        changed = true;
      }
  }
  return changed;
}

// --- 9a3. dominator-scoped common subexpression elimination -------------------
// A pure, non-trapping Bin recomputed under a *dominating* identical Bin is
// replaced by the dominating def. Visibility is exactly "def dominates use",
// checked with the lib's dominance intervals: we walk the dominator tree (from
// the lib) in preorder, keeping a scoped table — entries pushed on block entry
// are truncated on exit. SSA guarantees a def dominates its uses, so the
// preorder walk always visits defs before the uses they can replace.
constexpr bool opt_cse(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks()) return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<uint32_t> ren(nv, NONE);
  auto resolve = [&](uint32_t v) -> uint32_t {
    while (v != NONE && v < nv && ren[v] != NONE) v = ren[v];
    return v;
  };
  // known constants: two temps holding 7 are the SAME operand. CSE keys
  // compare constants by VALUE, not by vreg, so i*7 hits i*7.
  std::vector<char> kconst(nv, 0);
  std::vector<val_t> kval(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
      kconst[I.dst] = 1;
      kval[I.dst] = I.c;
    }
  struct Ent {
    B op;
    uint32_t x, y;   // vreg operands (NONE if the operand is a known const)
    val_t xv, yv;    // const operand values
    uint32_t val;    // the defining vreg (the CSE replacement)
  };
  std::vector<Ent> table;
  struct Fr {
    uint32_t b, ci, mark;
  };
  std::vector<Fr> st;
  std::vector<char> drop(fn.code.size(), 0);
  bool changed = false;
  auto rows = A.dom_tree.range();
  st.push_back({0, 0, (uint32_t)table.size()});
  while (!st.empty()) {
    uint32_t b = st.back().b;
    uint32_t ci = st.back().ci;
    uint32_t mark = st.back().mark;
    if (ci == 0) { // entering the block: scan its instrs once
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.a != NONE) {
          uint32_t r = resolve(I.a);
          changed |= (r != I.a);
          I.a = r;
        }
        if (I.b2 != NONE) {
          uint32_t r = resolve(I.b2);
          changed |= (r != I.b2);
          I.b2 = r;
        }
        if (I.c2 != NONE) { // Store: the stored value
          uint32_t r = resolve(I.c2);
          changed |= (r != I.c2);
          I.c2 = r;
        }
        for (uint32_t &av : I.args) { // Call arguments
          uint32_t r = resolve(av);
          changed |= (r != av);
          av = r;
        }
        if (I.k != K::Bin || I.dst == NONE) continue;
        if (I.b == B::Div || I.b == B::Mod)
          if (!I.nt) continue; // may trap: keep (nt: proven safe, pure)
        Ent e{};
        e.op = I.b;
        e.x = I.a;
        e.y = I.b2;
        if (e.x != NONE && e.x < nv && kconst[e.x]) {
          e.x = NONE;
          e.xv = kval[I.a];
        }
        if (e.y != NONE && e.y < nv && kconst[e.y]) {
          e.y = NONE;
          e.yv = kval[I.b2];
        }
        bool comm = I.b == B::Add || I.b == B::Mul || I.b == B::Eq ||
                    I.b == B::Ne || I.b == B::And || I.b == B::Or ||
                    I.b == B::Xor || I.b == B::MulHi;
        auto lt = [](const Ent &e, int i) -> uint64_t { // orderable tag
          if (i == 0) return e.x == NONE ? ((uint64_t)1 << 63) ^
                                              (uint64_t)e.xv
                                         : (uint64_t)e.x;
          return e.y == NONE ? ((uint64_t)1 << 63) ^ (uint64_t)e.yv
                             : (uint64_t)e.y;
        };
        if (comm && lt(e, 1) < lt(e, 0)) {
          std::swap(e.x, e.y);
          std::swap(e.xv, e.yv);
        }
        e.val = I.dst;
        bool found = false;
        for (uint32_t t = 0; t < table.size(); ++t) // tables stay tiny
          if (table[t].op == e.op && table[t].x == e.x && table[t].y == e.y &&
              (e.x != NONE || table[t].xv == e.xv) &&
              (e.y != NONE || table[t].yv == e.yv)) {
            ren[I.dst] = table[t].val;
            found = true;
            break;
          }
        if (found) {
          drop[i] = 1;
          os.cses++;
          changed = true;
        } else {
          table.push_back(e);
        }
      }
    }
    auto ch = rows[b]; // children of b in the dominator tree (from the lib)
    if (ci < (uint32_t)ch.size()) {
      st.back().ci = ci + 1;
      uint32_t nb = (uint32_t)ch[ci];
      st.push_back({nb, 0, (uint32_t)table.size()});
      continue;
    }
    table.resize(mark); // leaving b: its entries go out of scope
    st.pop_back();
  }
  if (changed) { // sweep CSE'd instrs, preserving block ranges
    bool any_drop = false;
    for (char c : drop) any_drop |= (c != 0);
    if (any_drop) {
      std::vector<Instr> nc{};
      nc.reserve(fn.code.size());
      for (uint32_t b = 0; b < fn.nblocks(); ++b) {
        uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
        fn.blocks[b].begin = (uint32_t)nc.size();
        for (uint32_t i = ob; i < oe; ++i)
          if (!drop[i]) nc.push_back(fn.code[i]);
        fn.blocks[b].end = (uint32_t)nc.size();
      }
      fn.code = std::move(nc);
    }
  }
  return changed;
}

// --- 9b. unreachable block removal + phi arg pruning -------------------------
// Phi instrs carry their pred block ids in t/f (set during SSA construction),
// so pruning is by pred identity — no guesswork.
constexpr bool opt_remove_unreachable(Fn &fn, OptStats &os) {
  std::vector<char> reach(fn.nblocks(), 0);
  std::vector<uint32_t> wl{0};
  reach[0] = 1;
  while (!wl.empty()) {
    uint32_t b = wl.back(); wl.pop_back();
    for (uint32_t s : {fn.succ0(b), fn.succ1(b)})
      if (s != NONE && !reach[s]) { reach[s] = 1; wl.push_back(s); }
  }
  bool any = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    if (reach[b]) continue;
    if (b != 0 && fn.blocks[b].begin == fn.blocks[b].end &&
        fn.succ0(b) == NONE && fn.succ1(b) == NONE)
      continue; // isolated by an earlier round: old news, not a change
    any = true;  // genuinely new dead code (or a newly-folded edge)
    break;
  }
  if (any) {
    // compact code of reachable blocks; isolate unreachable ones
    std::vector<Instr> nc{};
    uint32_t gone = 0;
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      fn.blocks[b].begin = (uint32_t)nc.size();
      if (reach[b]) {
        for (uint32_t i = ob; i < oe; ++i) nc.push_back(fn.code[i]);
      } else {
        if (oe > ob || fn.succ[2 * b] != NONE || fn.succ[2 * b + 1] != NONE)
          ++gone; // only count blocks that actually died THIS round
        fn.succ[2 * b] = NONE;      // isolate: no stale edges into analyses
        fn.succ[2 * b + 1] = NONE;
      }
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
    os.blocks_gone += gone;
  }
  // succ may have changed (Cbr folds) even without dead blocks: always
  // refresh preds, then prune phi args by pred identity
  fn.recompute_preds();
  bool degenerated = false;
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    if (fn.preds[b].empty()) continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k != K::Phi) break;
      auto is_live_pred = [&](uint32_t p) {
        for (uint32_t q : fn.preds[b])
          if (q == p) return true;
        return false;
      };
      bool l0 = (I.a != NONE) && is_live_pred(I.t);
      bool l1 = (I.b2 != NONE) && is_live_pred(I.f);
      uint32_t keep = l0 ? I.a : (l1 ? I.b2 : NONE);
      if (l0 && l1) continue;         // both preds live: phi unchanged
      if (I.k != K::Phi) continue;    // already degenerated on an earlier visit
      if (keep == NONE) {             // no live pred (cannot happen in a live
        I.k = K::Const; I.c = 0;      // block, but be safe) -> undef, DCE food
        os.folds++;
      } else {                        // one pred left: phi degenerates to copy
        I.k = K::Copy; I.a = keep; I.b2 = NONE;
        os.copies++;
      }
      os.phi_degens++;
      degenerated = true;
      I.t = I.f = NONE;
    }
  }
  return any || degenerated;
}

// --- 9b2. block splicing ------------------------------------------------------
// If a single-succ block b ends in `br s`, and s has exactly one pred (b) and
// no phis, s's code is appended to b (b's Br dropped). This collapses the
// jump chains that branch folding leaves behind. Phi pred ids in s's
// successors are rekeyed s -> b so phi args stay pred-identified.
constexpr bool opt_splice(Fn &fn, OptStats &os) {
  uint32_t nb = fn.nblocks();
  std::vector<uint32_t> absorbed(nb, NONE); // block -> absorber
  std::vector<uint32_t> merges(nb, NONE);   // absorber -> absorbed block
  bool any = false;
  for (uint32_t b = 0; b < nb; ++b) {
    if (absorbed[b] != NONE || merges[b] != NONE)
      continue; // already in a pair this sweep (matchings only: no chains)
    uint32_t s = fn.succ0(b);
    if (fn.succ1(b) != NONE || s == NONE) continue; // dead or 2-succ
    if (s == b || s == 0) continue;         // self-loop / entry: keep
    if (absorbed[s] != NONE || merges[s] != NONE) continue;
    if (fn.preds[s].size() != 1 || fn.preds[s][0] != b) continue;
    bool s_has_phi = false;
    for (uint32_t i = fn.blocks[s].begin; i < fn.blocks[s].end; ++i)
      if (fn.code[i].k == K::Phi) { s_has_phi = true; break; }
    if (s_has_phi) continue;
    // absorb s into b
    absorbed[s] = b;
    merges[b] = s;
    any = true;
    os.splices++;
    // rekey phis in s's successors (pred s -> pred b) and refresh their preds
    for (uint32_t t : {fn.succ0(s), fn.succ1(s)}) {
      if (t == NONE) continue;
      for (uint32_t i = fn.blocks[t].begin; i < fn.blocks[t].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi) break;
        if (I.t == s) I.t = b;
        if (I.f == s) I.f = b;
      }
      for (uint32_t &p : fn.preds[t])
        if (p == s) p = b;
    }
  }
  if (!any) return false;
  // rebuild: absorbed blocks vanish; an absorber emits its own non-terminator
  // instrs, then its absorbed block's instrs, then (that block may itself
  // have absorbed someone in this sweep) the whole chain, ending at the final
  // terminator. Ranges AND succs are read from SNAPSHOTS: the rebuild
  // overwrites fn.blocks/fn.succ while chain ends still need the absorbed
  // blocks' ORIGINAL successors (an absorbed block isolated earlier in this
  // same loop must not poison its absorber with NONE succs).
  std::vector<Block> old_blocks = fn.blocks;
  std::vector<uint32_t> old_succ = fn.succ;
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size());
  for (uint32_t b = 0; b < nb; ++b) {
    if (absorbed[b] != NONE) {
      fn.blocks[b] = {(uint32_t)nc.size(), (uint32_t)nc.size()};
      fn.succ[2 * b] = NONE;
      fn.succ[2 * b + 1] = NONE;
      continue;
    }
    uint32_t begin = (uint32_t)nc.size();
    uint32_t x = b;
    for (;;) {
      uint32_t xb = old_blocks[x].begin, xe = old_blocks[x].end;
      uint32_t m = merges[x];
      if (m == NONE) { // chain end: emit everything incl. the terminator
        for (uint32_t i = xb; i < xe; ++i) nc.push_back(fn.code[i]);
        fn.succ[2 * b] = old_succ[2 * x];
        fn.succ[2 * b + 1] = old_succ[2 * x + 1];
        break;
      }
      // x absorbed m: emit x's instrs but its trailing Br, then follow
      if (xe > xb)
        for (uint32_t i = xb; i + 1 < xe; ++i) nc.push_back(fn.code[i]);
      x = m;
    }
    fn.blocks[b] = {begin, (uint32_t)nc.size()};
  }
  fn.code = std::move(nc);
  fn.recompute_preds();
  return true;
}

// --- 9c. SSA dead-code elimination -------------------------------------------
// roots: Print, Br, Cbr, Exit, and Div/Mod whose divisor is not a known
// nonzero constant (they may trap — removing them would erase the trap).
constexpr bool opt_dce(Fn &fn, OptStats &os) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::vector<char> known(nv, 0);
  std::vector<val_t> kval(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const) { known[I.dst] = 1; kval[I.dst] = I.c; }
  // def index of each vreg
  std::vector<uint32_t> def_idx(nv, NONE);
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst != NONE) def_idx[fn.code[i].dst] = i;
  auto may_trap = [&](const Instr &I) {
    if (I.k != K::Bin || (I.b != B::Div && I.b != B::Mod)) return false;
    if (I.nt) return false; // proven trap-free: pure, removable
    return !(I.b2 < nv && known[I.b2] && kval[I.b2] != 0);
  };
  std::vector<char> live(fn.code.size(), 0);
  std::vector<uint32_t> wl{};
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      // roots: side effects (Print is output, Store mutates the arena,
      // Input consumes the stdin tape), control flow, and anything that may
      // TRAP (Div/Mod with a non-constant divisor, Alloc: n<0 or OOM,
      // Load: out of bounds) — removing them would erase observable behavior.
      if (I.k == K::Print || I.k == K::Br || I.k == K::Cbr || I.k == K::Exit ||
          I.k == K::Store || I.k == K::Input || I.k == K::Alloc ||
          I.k == K::Load || I.k == K::Ret || I.k == K::Call || may_trap(I)) {
        live[i] = 1;
        wl.push_back(i);
      }
    }
  while (!wl.empty()) {
    uint32_t i = wl.back(); wl.pop_back();
    for (uint32_t v : fn.code[i].args) { // Call arguments
      if (v == NONE || v >= nv) continue;
      uint32_t d = def_idx[v];
      if (d != NONE && !live[d]) { live[d] = 1; wl.push_back(d); }
    }
    for (uint32_t v : {fn.code[i].a, fn.code[i].b2, fn.code[i].c2}) {
      if (v == NONE || v >= nv) continue;
      uint32_t d = def_idx[v];
      if (d != NONE && !live[d]) { live[d] = 1; wl.push_back(d); }
    }
  }
  // sweep
  bool changed = false;
  {
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size());
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t i = ob; i < oe; ++i) {
        if (live[i]) nc.push_back(fn.code[i]);
        else { changed = true; os.dce++; }
      }
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
  // trivial-phi collapse (x = phi(a, a) / phi(self, a) -> copy), cascading
  for (int round = 0; round < 10; ++round) {
    bool ch = false;
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi) continue;
        uint32_t x = (I.a != NONE && I.a != I.dst) ? I.a : NONE;
        uint32_t y = (I.b2 != NONE && I.b2 != I.dst) ? I.b2 : NONE;
        if (x == NONE && y == NONE) continue;         // all self/undef
        if (x != NONE && y != NONE && x != y) continue; // genuine phi
        uint32_t pick = (x != NONE) ? x : y;            // phi(a), phi(a,self)
        I.k = K::Copy; I.a = pick; I.b2 = NONE;
        I.t = I.f = NONE;
        os.copies++; ch = true;
      }
    if (!ch) break;
    changed |= ch;
  }
  return changed;
}

// def instr index of vreg v (linear scan; diagnostics only)
constexpr uint32_t def_index_of(const Fn &fn, uint32_t v) {
  for (uint32_t b = 0; b < fn.nblocks(); ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst == v) return i;
  return NONE;
}

// --- 9d. loop-invariant code motion -------------------------------------------
// Uses the lib's natural loops + dominance intervals:
//   - loop bodies from calculate_natural_loops_with_header_...
//   - ordering by nesting depth from calculate_nesting_of_natural_loop_headers
//   - "def dominates preheader" via the dominance intervals
// Hoist pure non-trapping instrs whose operands are all defined outside the
// loop (and dominate the preheader) into the preheader. Only loops with a
// unique outside predecessor are eligible (preheader must dominate header).
constexpr bool opt_licm(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks()) return false;
  bool changed_any = false;
  // headers sorted innermost-first (nest count descending)
  std::vector<uint32_t> headers;
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h) headers.push_back(h);
  for (size_t i = 0; i + 1 < headers.size(); ++i)   // tiny sorts are fine here
    for (size_t j = i + 1; j < headers.size(); ++j)
      if (A.nest[headers[j]] > A.nest[headers[i]])
        std::swap(headers[i], headers[j]);
  for (uint32_t h : headers) {
    auto row = A.loops.range()[h];
    if (row.empty()) continue;
    bool inL_flag_dummy = false;
    (void)inL_flag_dummy;
    std::vector<char> inL(A.n, 0);
    for (ulen x : row) inL[(uint32_t)x] = 1;
    inL[h] = 1;
    // unique outside pred?
    uint32_t pre = NONE;
    uint32_t outside = 0;
    for (uint32_t p : fn.preds[h]) {
      if (!inL[p]) { pre = p; ++outside; }
    }
    if (outside != 1 || pre == NONE) continue;
    // def block of every vreg (recomputed each hoist round)
    for (int round = 0; round < 10; ++round) {
      const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
      std::vector<uint32_t> defb(nv, NONE);
      for (uint32_t b = 0; b < fn.nblocks(); ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst != NONE) defb[fn.code[i].dst] = b;
      auto invariant = [&](uint32_t v) -> bool {
        if (v == NONE) return true;
        if (v >= nv) return false;
        uint32_t d = defb[v];
        if (d == NONE) return false;
        if (inL[d]) return false;              // defined inside: not invariant
        return A.dominates(d, pre);            // reachable into preheader
      };
      bool changed = false;
      for (ulen xb : row) {
        uint32_t B = (uint32_t)xb;
        if (B == h) continue;                  // header holds the phi+cond
        for (uint32_t i = fn.blocks[B].begin; i < fn.blocks[B].end; ++i) {
          Instr &I = fn.code[i];
          if (I.dst == NONE) continue;                 // control/print: no
          if (I.k == K::Phi || I.k == K::Cbr || I.k == K::Br ||
              I.k == K::Print || I.k == K::Exit)
            continue;
          if (I.k == K::Bin && (I.b == B::Div || I.b == B::Mod) && !I.nt)
            continue; // may trap: no hoist (nt: proven safe, hoistable)
          if (I.k == K::Const) continue;               // nothing to gain
          // arena/stdin ops are NOT loop-invariant: Alloc returns a fresh
          // handle per execution, Load/Store touch mutable memory, Len can
          // change when an Alloc lands in between, Input consumes the tape.
          if (I.k == K::Alloc || I.k == K::Load || I.k == K::Store ||
              I.k == K::Len || I.k == K::Input)
            continue;
          if (I.k == K::Call)
            continue; // callee may print/trap/read stdin: never invariant
          if (!invariant(I.a) || !invariant(I.b2)) continue;
          // hoist: move to the preheader, right before its terminator.
          // The terminator is always the last instr, so this is guaranteed
          // to be after any phis AND after every operand def that lives in
          // the preheader itself.
          uint32_t pe_old = fn.blocks[pre].end - 1;   // terminator, pre-erase
          Instr copy = I;
          fn.code.erase(fn.code.begin() + (long)i);   // remove from B
          // insertion index in the post-erase vector:
          uint32_t pe = (pe_old > i) ? pe_old - 1 : pe_old;
          fn.code.insert(fn.code.begin() + (long)pe, copy);
          // range fixup 1: everything after the erased index shifts down
          for (uint32_t x = 0; x < fn.nblocks(); ++x) {
            if (fn.blocks[x].begin > i) fn.blocks[x].begin--;
            if (fn.blocks[x].end > i) fn.blocks[x].end--;
          }
          // range fixup 2: the hoisted instr now lives inside `pre` (before
          // its terminator); blocks after it shift up
          for (uint32_t x = 0; x < fn.nblocks(); ++x) {
            if (x == pre) { fn.blocks[x].end++; continue; }
            if (fn.blocks[x].begin >= pe + 1) fn.blocks[x].begin++;
            if (fn.blocks[x].end >= pe + 1) fn.blocks[x].end++;
          }
          os.hoisted++;
          changed = true;
          break;                                      // rescan this block
        }
        if (changed) break;
      }
      if (!changed) break;
      changed_any = true;
    }
  }
  return changed_any;
}

// ============================================================================
// 9d3. HANDLE ALIAS ANALYSIS (allocation-identity + value ranges)
// ============================================================================
// The arena is one flat int64 array; a handle is the index of an
// allocation's first slot. alias_analyze computes, for every vreg, whether
// it provably holds the handle of EXACTLY ONE allocation site:
//    single[v] = id   for the Alloc dst, Copies of it, and Phis whose live
//                     args are all that same id
//    single[v] = NONE otherwise ("obscure": constants, arithmetic results,
//                     loads, params, input(), call results, mixed phis --
//                     the value may name ANY array, or no array at all).
// Two program-wide gates are derived from this (see prog_handle_gates):
//   lift_ok   -- no Load/Store anywhere reads through an obscure handle.
//                Otherwise a runtime value could observe the stale arena
//                contents of a promoted (mem2reg'd) array, so promotion is
//                disabled globally. (Promotion KEEPS the allocation in the
//                arena, so handle VALUES and lengths never change.)
//   stkarr_ok -- additionally no handle value is observable outside handle
//                position and no Len reads an obscure handle. Moving an
//                array OUT of the arena (the C backend's stack arrays)
//                shifts every LATER handle value; that must be provably
//                unobservable. [This also fixes a soundness bug: printing
//                the second of two arrays used to print a shifted handle
//                in the C backend when the first was stack-allocated.]
struct AliasInfo {
  std::vector<uint32_t> single{}; // vreg -> alloc id, NONE = obscure
  uint32_t nallocs{};
};

constexpr AliasInfo alias_analyze(const Fn &fn) {
  AliasInfo AI{};
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  AI.single.assign(nv, NONE);
  if (!nv) return AI;
  for (const Instr &I : fn.code)
    if (I.k == K::Alloc && I.dst != NONE && I.dst < nv)
      AI.single[I.dst] = AI.nallocs++;
  // Copies and Phis reference each other (loop phis), so iterate to a
  // fixpoint. Each vreg has ONE def, so single[v] is a pure function of its
  // def instr; the loop just resolves the reference chains. Transitions are
  // monotone NONE->id (an id, once assigned, is stable), so this terminates.
  bool grew = true;
  for (uint32_t guard = 0; grew && guard < 64; ++guard) {
    grew = false;
    for (const Instr &I : fn.code) {
      if (I.dst == NONE || I.dst >= nv) continue;
      if (I.k == K::Copy) {
        if (I.a != NONE && I.a < nv &&
            AI.single[I.dst] != AI.single[I.a]) {
          AI.single[I.dst] = AI.single[I.a];
          grew = true;
        }
      } else if (I.k == K::Phi) {
        // single iff BOTH args exist, are that same single, and are not
        // obscure; an undef (NONE) arg is treated as obscure (conservative)
        bool ok = I.a != NONE && I.a < nv && AI.single[I.a] != NONE &&
                  I.b2 != NONE && I.b2 < nv &&
                  AI.single[I.b2] == AI.single[I.a];
        uint32_t want = ok ? AI.single[I.a] : NONE;
        if (AI.single[I.dst] != want) {
          AI.single[I.dst] = want;
          grew = true;
        }
      }
    }
  }
  return AI;
}

// Program-wide gate computation over every function (pre-optimization):
// the optimizer only ever removes or duplicates instructions, and copies
// propagate single-ness, so a gate that holds now holds for the whole run.
constexpr void prog_handle_gates(const std::vector<Fn> &fns, bool &lift_ok,
                                 bool &stkarr_ok) {
  lift_ok = true;
  stkarr_ok = true;
  for (const Fn &fn : fns) {
    if (!fn.nblocks()) continue;
    AliasInfo AI = alias_analyze(fn);
    const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
    auto sgl = [&](uint32_t v) -> bool {
      return v != NONE && v < nv && AI.single[v] != NONE;
    };
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Load || I.k == K::Store) {
          if (!sgl(I.a)) lift_ok = false; // could hit ANY array's contents
        } else if (I.k == K::Len) {
          if (!sgl(I.a)) stkarr_ok = false; // length table would shift
        }
        if (!stkarr_ok) continue;
        // observable uses of a handle VALUE (numbers that shift when an
        // earlier allocation leaves the arena)
        switch (I.k) {
        case K::Bin:   // arithmetic/comparison on a handle
        case K::Cbr:   // branching on a handle value
        case K::Print: // printing a handle
        case K::Ret:   // returning a handle to the caller
        case K::Alloc: // a handle as an allocation size
          if (sgl(I.a) || sgl(I.b2)) stkarr_ok = false;
          break;
        case K::Load:  // handle used as an INDEX
          if (sgl(I.b2)) stkarr_ok = false;
          break;
        case K::Store: // handle used as an index or stored as a value
          if (sgl(I.b2) || sgl(I.c2)) stkarr_ok = false;
          break;
        case K::Call:  // handle passed to a function
          for (uint32_t v : I.args)
            if (sgl(v)) stkarr_ok = false;
          break;
        default: break; // Copy/Phi: plumbing; Len handle position: fine
        }
      }
  }
}

// The array arena: identical bump-allocator semantics in EVERY backend
// (interpreters, bytecode VM, emitted C, emitted x86-64). A handle is the
// index of the array's first element; allocation is monotonic (no free), so
// handle values, aliasing, and allocation order are bit-identical everywhere.
// CAP must match MJZ_ARENA_CAP emitted into the C/asm backends.
inline constexpr val_t MJZ_ARENA_CAP = 2097152; // 2^21 int64 slots = 16 MiB

// ============================================================================
// 9d3b. VALUE RANGE ANALYSIS (signed intervals + branch refinement)
// ============================================================================
// Per-block dense interval states over SSA vregs; every vreg has exactly one
// def, so a vreg's "value" is a single runtime integer and its interval is a
// property of the reaching paths, refined per block entry:
//   * interval transfer for +,-,* (saturating; any overflow -> top)
//   * sign-aware Div/Mod transfers: x>=0,y>=1 => x%y in [0, min(x.hi,y.hi-1)]
//     (this is what keeps Euclid's loop provably non-negative)
//   * branch refinement on each out-edge of a Cbr whose condition is a
//     comparison: on the taken edge the comparison HOLDS as a fact about the
//     operand values (SSA: the same values, not recomputations), so the
//     operands' intervals can be tightened -- `x != 0` chops 0 off an
//     endpoint, `x < y` caps x.hi, `x == c` pins both sides.
//   * plain iteration first, growing intervals UP from concrete seeds
//     (loop counters start as [0,0] and widen only when they keep growing:
//     `while (i < 1000)` converges to i in [0,999] at the body entry);
//     then widening at blocks that still grow; then narrowing rounds.
//     If no fixpoint is reached at all, every state falls back to TOP
//     (sound over-approximation, no proofs). The result is always a sound
//     over-approximation of the reachable values.
struct Iv {
  val_t lo{INT64_MIN}, hi{INT64_MAX};
  bool bot{}; // empty interval: unreachable program point
};
constexpr bool iv_eq(const Iv &a, const Iv &b) {
  return a.bot == b.bot && a.lo == b.lo && a.hi == b.hi;
}
constexpr Iv iv_hull(const Iv &a, const Iv &b) { // join (may-analysis)
  if (a.bot) return b;
  if (b.bot) return a;
  return {a.lo < b.lo ? a.lo : b.lo, a.hi > b.hi ? a.hi : b.hi, false};
}
constexpr uint64_t u64_abs(val_t v) { // |v| saturating at 2^63
  uint64_t u = (uint64_t)v;
  return (u >> 63) ? (uint64_t)0 - u : u;
}
// one interval per operand of a Bin; all results are sound supersets
constexpr Iv iv_bin(B b, const Iv &x, const Iv &y) {
  if (x.bot || y.bot) return {0, 0, true};
  switch (b) {
  case B::Add: {
    __int128 lo = (__int128)x.lo + y.lo, hi = (__int128)x.hi + y.hi;
    if (lo < (__int128)INT64_MIN || hi > (__int128)INT64_MAX)
      return {INT64_MIN, INT64_MAX, false}; // wraps: anything is possible
    return {(val_t)lo, (val_t)hi, false};
  }
  case B::Sub: {
    __int128 lo = (__int128)x.lo - y.hi, hi = (__int128)x.hi - y.lo;
    if (lo < (__int128)INT64_MIN || hi > (__int128)INT64_MAX)
      return {INT64_MIN, INT64_MAX, false};
    return {(val_t)lo, (val_t)hi, false};
  }
  case B::Mul: {
    __int128 c[4] = {(__int128)x.lo * y.lo, (__int128)x.lo * y.hi,
                     (__int128)x.hi * y.lo, (__int128)x.hi * y.hi};
    __int128 lo = c[0], hi = c[0];
    for (int k = 1; k < 4; ++k) {
      if (c[k] < lo) lo = c[k];
      if (c[k] > hi) hi = c[k];
    }
    if (lo < (__int128)INT64_MIN || hi > (__int128)INT64_MAX)
      return {INT64_MIN, INT64_MAX, false};
    return {(val_t)lo, (val_t)hi, false};
  }
  case B::Div: {
    if (!(y.lo >= 1 || y.hi <= -1)) return {INT64_MIN, INT64_MAX, false};
    // single-sign divisor: the extrema of x/y are at the four corners
    val_t cl = INT64_MAX, ch = INT64_MIN; // careful with init directions
    cl = INT64_MIN; ch = INT64_MAX;
    val_t lo = INT64_MAX, hi = INT64_MIN;
    for (int k = 0; k < 4; ++k) {
      val_t a = (k & 1) ? x.hi : x.lo, d = (k & 2) ? y.hi : y.lo;
      Rv r = w_div(a, d); // corner may trap numerically: then top
      if (r.trap) return {INT64_MIN, INT64_MAX, false};
      if (r.v < lo) lo = r.v;
      if (r.v > hi) hi = r.v;
    }
    (void)cl;
    (void)ch;
    return {lo, hi, false};
  }
  case B::Mod: {
    if (!(y.lo >= 1 || y.hi <= -1)) return {INT64_MIN, INT64_MAX, false};
    // |r| < |y| and sign(r) == sign(x); also |r| <= |x|
    val_t M{};
    if (y.lo >= 1) {
      M = (y.hi == INT64_MAX) ? INT64_MAX : (y.hi - 1);
    } else {
      uint64_t m = u64_abs(y.lo) > u64_abs(y.hi) ? u64_abs(y.lo)
                                                 : u64_abs(y.hi);
      M = (m >= (uint64_t)INT64_MAX) ? INT64_MAX : (val_t)(m - 1);
    }
    if (x.lo >= 0) return {0, x.hi < M ? x.hi : M, false};
    if (x.hi <= 0) return {x.lo < -M ? -M : x.lo, 0, false};
    return {x.lo < -M ? -M : x.lo, x.hi < M ? x.hi : M, false};
  }
  case B::Lt: case B::Le: case B::Gt: case B::Ge: case B::Eq: case B::Ne:
    return {0, 1, false}; // comparisons yield 0/1
  default:
    return {INT64_MIN, INT64_MAX, false}; // bit ops / shifts / mulhi: top
  }
}
// tighten (x,y) given that `x cmp y` is known to be `taken`
constexpr void iv_refine(Iv &x, Iv &y, B cmp, bool taken) {
  if (x.bot || y.bot) return;
  if (cmp == B::Gt || cmp == B::Ge) { // normalize to the Lt/Le direction
    Iv t = x;
    x = y;
    y = t;
    cmp = (cmp == B::Gt) ? B::Lt : B::Le;
  }
  auto exclude = [](Iv &i, val_t c) { // remove the point c if it is an endpoint
    if (i.lo == c && i.hi == c) { i.bot = true; return; }
    if (i.lo == c && c != INT64_MAX) i.lo = c + 1;
    else if (i.hi == c && c != INT64_MIN) i.hi = c - 1;
  };
  if (cmp == B::Lt) {
    if (taken) {
      if (y.hi != INT64_MIN && x.hi > y.hi - 1) x.hi = y.hi - 1;
      if (x.lo != INT64_MAX && y.lo < x.lo + 1) y.lo = x.lo + 1;
    } else { // x >= y
      if (x.lo < y.lo) x.lo = y.lo;
      if (y.hi > x.hi) y.hi = x.hi;
    }
  } else if (cmp == B::Le) {
    if (taken) {
      if (x.hi > y.hi) x.hi = y.hi;
      if (y.lo < x.lo) y.lo = x.lo;
    } else { // x > y
      if (y.lo != INT64_MAX && x.lo < y.lo + 1) x.lo = y.lo + 1;
      if (x.hi != INT64_MIN && y.hi > x.hi - 1) y.hi = x.hi - 1;
    }
  } else if (cmp == B::Eq) {
    if (taken) {
      val_t lo = x.lo > y.lo ? x.lo : y.lo;
      val_t hi = x.hi < y.hi ? x.hi : y.hi;
      if (lo > hi) { x.bot = y.bot = true; } // contradictory: dead edge
      else { x = {lo, hi, false}; y = {lo, hi, false}; }
    } else {
      if (y.lo == y.hi) exclude(x, y.lo);
      else if (x.lo == x.hi) exclude(y, x.lo);
    }
  } else { // B::Ne
    if (taken) {
      if (y.lo == y.hi) exclude(x, y.lo);
      else if (x.lo == x.hi) exclude(y, x.lo);
    } else {
      val_t lo = x.lo > y.lo ? x.lo : y.lo;
      val_t hi = x.hi < y.hi ? x.hi : y.hi;
      if (lo > hi) { x.bot = y.bot = true; }
      else { x = {lo, hi, false}; y = {lo, hi, false}; }
    }
  }
}

struct Ranges {
  bool ok{};
  uint32_t nb{}, nv{};
  std::vector<Iv> entry{}; // per block: nv intervals at block entry
};

// Apply instr i's definition to state st (phis are pre-substituted by the
// caller via the block-entry phi rule; they are skipped here).
constexpr void iv_transfer(const Fn &fn, uint32_t i, uint32_t nv,
                           std::vector<Iv> &st) {
  const Instr &I = fn.code[i];
  if (I.dst == NONE || I.dst >= nv) return;
  switch (I.k) {
  case K::Const:
    st[I.dst] = {I.c, I.c, false};
    return;
  case K::Copy:
    if (I.a != NONE && I.a < nv) st[I.dst] = st[I.a];
    else st[I.dst] = {INT64_MIN, INT64_MAX, false};
    return;
  case K::Bin:
    st[I.dst] = iv_bin(I.b, st[I.a], st[I.b2]);
    return;
  case K::Phi:
    return; // handled by the block-entry rule
  case K::Alloc:
    st[I.dst] = {INT64_MIN, INT64_MAX, false}; // handle value: unknown
    return;
  case K::Len:
    st[I.dst] = {0, MJZ_ARENA_CAP, false};
    return;
  default:
    st[I.dst] = {INT64_MIN, INT64_MAX, false}; // input/call/load: top
    return;
  }
}

constexpr Ranges range_analyze(const Fn &fn, const An &A) {
  Ranges RA{};
  RA.nb = fn.nblocks();
  RA.nv = fn.nvars + (uint32_t)fn.version_base.size();
  RA.ok = RA.nb && RA.nv &&
          (uint64_t)RA.nb * RA.nv <= 4u * 1024u * 1024u; // memory cap
  if (!RA.ok) return RA;
  const uint32_t nb = RA.nb, nv = RA.nv;
  // comparison def behind each Cbr (same predicate the emission plan uses
  // for branch fusion: single-use, single-def comparison)
  std::vector<uint32_t> raw(nv, 0), ndefs(nv, 0), defi(nv, NONE);
  for (uint32_t i = 0; i < fn.code.size(); ++i) {
    const Instr &I = fn.code[i];
    if (I.a != NONE && I.a < nv) raw[I.a]++;
    if (I.b2 != NONE && I.b2 < nv) raw[I.b2]++;
    if (I.c2 != NONE && I.c2 < nv) raw[I.c2]++;
    if (I.dst != NONE && I.dst < nv) {
      ++ndefs[I.dst];
      defi[I.dst] = i;
    }
  }
  auto is_cmp_def = [&](uint32_t c) -> uint32_t {
    if (c == NONE || c >= nv) return NONE;
    if (raw[c] != 1 || ndefs[c] != 1) return NONE;
    uint32_t d = defi[c];
    if (d == NONE) return NONE;
    const Instr &D = fn.code[d];
    if (D.k != K::Bin) return NONE;
    if (D.b == B::Lt || D.b == B::Le || D.b == B::Gt || D.b == B::Ge ||
        D.b == B::Eq || D.b == B::Ne)
      return d;
    return NONE;
  };
  std::vector<Iv> entry((size_t)nb * nv, Iv{0, 0, true});  // bottom: grow up
  std::vector<Iv> exit_((size_t)nb * nv, Iv{0, 0, true});
  std::vector<char> touched(nb, 0);
  std::vector<Iv> st(nv), edge(nv);
  const uint32_t plain = 4 * nb + 16;
  const uint32_t widened_cap = 4 * nb + 16;
  bool widening = false;
  uint32_t round = 0;
  bool converged = false;
  for (;;) {
    bool changed = false;
    for (uint32_t b = 0; b < nb; ++b) {
      // ---- entry = hull over incoming (refined) pred exits + phi rule
      for (uint32_t v = 0; v < nv; ++v) st[v] = {0, 0, true};
      bool any_pred = false;
      for (uint32_t p : fn.preds[b]) {
        if (p >= nb || !touched[p]) continue;
        any_pred = true;
        // refined exit of p along the edge p -> b
        for (uint32_t v = 0; v < nv; ++v) edge[v] = exit_[(size_t)p * nv + v];
        const Instr &term = fn.code[fn.blocks[p].end - 1];
        if (term.k == K::Cbr) {
          uint32_t d = is_cmp_def(term.a);
          if (d != NONE) {
            const Instr &D = fn.code[d];
            Iv x = (D.a != NONE && D.a < nv) ? edge[D.a] : Iv{INT64_MIN, INT64_MAX, false};
            Iv y = (D.b2 != NONE && D.b2 < nv) ? edge[D.b2] : Iv{INT64_MIN, INT64_MAX, false};
            bool taken = (term.t == b);
            iv_refine(x, y, D.b, taken);
            if (D.a != NONE && D.a < nv) edge[D.a] = x;
            if (D.b2 != NONE && D.b2 < nv) edge[D.b2] = y;
          }
        }
        for (uint32_t v = 0; v < nv; ++v) st[v] = iv_hull(st[v], edge[v]);
      }
      if (!any_pred && b != 0) { // unreachable: keep bottom
        for (uint32_t v = 0; v < nv; ++v) st[v] = {0, 0, true};
      }
      if (b == 0) { // entry: everything top (params included)
        for (uint32_t v = 0; v < nv; ++v) st[v] = {INT64_MIN, INT64_MAX, false};
      } else {
        // phi rule: the phi's value is one of its args, per incoming edge
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (I.k != K::Phi) break;
          Iv acc{0, 0, true};
          for (uint32_t p : fn.preds[b]) {
            if (p >= nb || !touched[p]) continue;
            uint32_t arg = NONE;
            if (I.t == p && I.a != NONE) arg = I.a;
            else if (I.f == p && I.b2 != NONE) arg = I.b2;
            if (arg != NONE && arg < nv)
              acc = iv_hull(acc, exit_[(size_t)p * nv + arg]);
          }
          st[I.dst] = acc;
        }
      }
      // ---- convergence bookkeeping
      {
        bool diff = false;
        for (uint32_t v = 0; v < nv; ++v)
          if (!iv_eq(st[v], entry[(size_t)b * nv + v])) { diff = true; break; }
        if (diff) {
          // WIDEN AT LOOP HEADERS ONLY (Cousot discipline). Widening at
          // every block re-inflates refined body states (e.g. a body entry
          // [0,999] "grew" from [0,31] only because the header widened),
          // and the re-widened [0,MAX] feeds i+1 -> overflow -> TOP into
          // the phi, poisoning the lo side to MIN forever (+1 per round,
          // never converging). Non-header blocks keep the joined value and
          // descend freely.
          if (widening && b < A.n && A.inner[b] == b) { // widen growing sides
            for (uint32_t v = 0; v < nv; ++v) {
              Iv &old = entry[(size_t)b * nv + v];
              if (st[v].bot) continue;
              if (st[v].lo < old.lo) st[v].lo = INT64_MIN;
              if (st[v].hi > old.hi) st[v].hi = INT64_MAX;
              if (st[v].lo > st[v].hi) st[v] = old; // paranoia
            }
          }
          for (uint32_t v = 0; v < nv; ++v)
            entry[(size_t)b * nv + v] = st[v];
          changed = true;
        }
      }
      // ---- transfer through the block
      for (uint32_t v = 0; v < nv; ++v) st[v] = entry[(size_t)b * nv + v];
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
        iv_transfer(fn, i, nv, st);
      for (uint32_t v = 0; v < nv; ++v) exit_[(size_t)b * nv + v] = st[v];
      touched[b] = 1;
    }
    if (!changed) {
      converged = true;
      break;
    }
    ++round;
    if (!widening && round >= plain) widening = true;
    if (widening && round >= plain + widened_cap) break; // give up (below)
  }
  if (!converged) {
    // no fixpoint (pathological CFG): every claim would rest on an
    // under-approximation, so fall back to the trivially sound TOP
    entry.assign((size_t)nb * nv, Iv{INT64_MIN, INT64_MAX, false});
    RA.entry = std::move(entry);
    return RA;
  }
  // ---- narrowing: re-run transfers and intersect with the widened state
  for (uint32_t n = 0; n < 3; ++n) {
    for (uint32_t b = 0; b < nb; ++b) {
      for (uint32_t v = 0; v < nv; ++v) st[v] = {0, 0, true};
      bool any_pred = false;
      for (uint32_t p : fn.preds[b]) {
        if (p >= nb || !touched[p]) continue;
        any_pred = true;
        for (uint32_t v = 0; v < nv; ++v) edge[v] = exit_[(size_t)p * nv + v];
        const Instr &term = fn.code[fn.blocks[p].end - 1];
        if (term.k == K::Cbr) {
          uint32_t d = is_cmp_def(term.a);
          if (d != NONE) {
            const Instr &D = fn.code[d];
            Iv x = (D.a != NONE && D.a < nv) ? edge[D.a] : Iv{INT64_MIN, INT64_MAX, false};
            Iv y = (D.b2 != NONE && D.b2 < nv) ? edge[D.b2] : Iv{INT64_MIN, INT64_MAX, false};
            iv_refine(x, y, D.b, term.t == b);
            if (D.a != NONE && D.a < nv) edge[D.a] = x;
            if (D.b2 != NONE && D.b2 < nv) edge[D.b2] = y;
          }
        }
        for (uint32_t v = 0; v < nv; ++v) st[v] = iv_hull(st[v], edge[v]);
      }
      (void)any_pred;
      if (b == 0)
        for (uint32_t v = 0; v < nv; ++v) st[v] = {INT64_MIN, INT64_MAX, false};
      else
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (I.k != K::Phi) break;
          Iv acc{0, 0, true};
          for (uint32_t p : fn.preds[b]) {
            if (p >= nb || !touched[p]) continue;
            uint32_t arg = NONE;
            if (I.t == p && I.a != NONE) arg = I.a;
            else if (I.f == p && I.b2 != NONE) arg = I.b2;
            if (arg != NONE && arg < nv)
              acc = iv_hull(acc, exit_[(size_t)p * nv + arg]);
          }
          st[I.dst] = acc;
        }
      // intersect with the previous (sound) approximation
      for (uint32_t v = 0; v < nv; ++v) {
        Iv &old = entry[(size_t)b * nv + v];
        if (st[v].bot || old.bot) continue;
        val_t lo = st[v].lo > old.lo ? st[v].lo : old.lo;
        val_t hi = st[v].hi < old.hi ? st[v].hi : old.hi;
        if (lo <= hi) old = {lo, hi, false};
      }
      for (uint32_t v = 0; v < nv; ++v) st[v] = entry[(size_t)b * nv + v];
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
        iv_transfer(fn, i, nv, st);
      for (uint32_t v = 0; v < nv; ++v) exit_[(size_t)b * nv + v] = st[v];
    }
  }
  RA.entry = std::move(entry);
  return RA;
}

// --- 9d3c. trap elimination ----------------------------------------------------
// A Div/Mod whose divisor provably excludes 0 (and excludes -1, or whose
// dividend provably excludes INT64_MIN) can never trap: mark it `nt` so
// every backend drops the guard, DCE/LICM may treat it as pure, and the
// closed-form/simulation loop passes may delete the loop computing it.
// A Load/Store whose handle provably names ONE constant-length allocation
// with a constant in-range index can never trap either.
constexpr bool opt_trapelim(Fn &fn, const An &A, OptStats &os) {
  Ranges RA = range_analyze(fn, A);
  if (!RA.ok) return false;
  AliasInfo AI = alias_analyze(fn);
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  std::vector<uint32_t> defi(nv, NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst != NONE && fn.code[i].dst < nv)
        defi[fn.code[i].dst] = i;
  // allocation lengths (constant ones only)
  std::vector<val_t> alen(AI.nallocs, val_t(-1));
  for (const Instr &I : fn.code)
    if (I.k == K::Alloc && I.a != NONE && I.a < nv && defi[I.a] != NONE &&
        fn.code[defi[I.a]].k == K::Const && I.dst != NONE && I.dst < nv &&
        AI.single[I.dst] != NONE && fn.code[defi[I.a]].c >= 0)
      alen[AI.single[I.dst]] = fn.code[defi[I.a]].c;
  auto nz = [](const Iv &i) { return i.bot || i.hi < 0 || i.lo > 0; };
  auto nm1 = [](const Iv &i) { return i.bot || i.hi < -1 || i.lo > -1; };
  auto nmin = [](const Iv &i) { return i.bot || i.lo > INT64_MIN; };
  bool changed = false;
  std::vector<Iv> st(nv);
  for (uint32_t b = 0; b < nb; ++b) {
    for (uint32_t v = 0; v < nv; ++v) st[v] = RA.entry[(size_t)b * nv + v];
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Phi) continue; // entry rule already covered phis
      if (I.k == K::Bin && (I.b == B::Div || I.b == B::Mod) && !I.nt) {
        Iv dv = (I.b2 != NONE && I.b2 < nv) ? st[I.b2] : Iv{};
        Iv dd = (I.a != NONE && I.a < nv) ? st[I.a] : Iv{};
        if (nz(dv) && (nm1(dv) || nmin(dd))) {
          I.nt = true;
          os.traps_killed++;
          changed = true;
        }
      } else if ((I.k == K::Load || I.k == K::Store) && !I.nt) {
        // provably one constant-length allocation + provably in-range index
        // (a constant index, or the range analysis's interval for a runtime
        // index, must fit inside [0, len))
        if (I.a != NONE && I.a < nv && AI.single[I.a] != NONE &&
            alen[AI.single[I.a]] >= 0 && I.b2 != NONE && I.b2 < nv) {
          val_t L = alen[AI.single[I.a]];
          bool safe = false;
          if (defi[I.b2] != NONE && fn.code[defi[I.b2]].k == K::Const) {
            val_t idx = fn.code[defi[I.b2]].c;
            safe = idx >= 0 && idx < L;
          } else {
            const Iv &ix = st[I.b2]; // range at this program point
            safe = !ix.bot && ix.lo >= 0 && ix.hi < L;
          }
          if (safe) {
            I.nt = true;
            os.traps_killed++;
            changed = true;
          }
        }
      }
      iv_transfer(fn, i, nv, st);
    }
  }
  return changed;
}

// ============================================================================
// 9d4. mem2reg: array memory -> SSA registers ("memory to register lifting")
// ============================================================================
// An `array(L)` with constant L whose every access is through a handle that
// provably names ONLY that allocation, with a constant in-range index, does
// not need memory at all: slot j of the array becomes a pseudo-variable,
//     init   : L x `Const 0` right after the Alloc (arena arrays are
//              zero-filled; the Alloc itself is KEPT so the arena bump,
//              handle values, lengths and OOM traps stay bit-identical)
//     store  : a versioned def (a Copy) of the slot's pseudo-variable
//     load   : a use of the currently reaching version
// with phis placed at the iterated dominance frontier of each slot's def
// blocks and renaming by dominator-tree walk -- full SSA, so every later
// pass (constprop, CSE, LICM, closed forms...) sees plain registers.
// An array that is never READ (no Load at all) is dead regardless of index
// shape: all its Stores are deleted ("if the array is not read we just
// delete writes, cuz why not") and Len(h) folds to the constant L.
constexpr bool opt_mem2reg(Fn &fn, const An &A, OptStats &os, bool lift_ok) {
  if (!lift_ok) return false;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nb || !nv) return false;
  AliasInfo AI = alias_analyze(fn);
  if (!AI.nallocs) return false;
  std::vector<uint32_t> ndefs(nv, 0);
  for (const Instr &I : fn.code)
    if (I.dst != NONE && I.dst < nv) ++ndefs[I.dst];
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv && ndefs[I.dst] == 1) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  constexpr val_t MJZ_MEM2REG_MAX_ELEMS = 256;
  constexpr uint32_t MJZ_MEM2REG_MAX_SLOTS = 2048;
  // ---- classify allocations -----------------------------------------------
  // liftable: constant length, every access single-handle + const index
  //           in range (loads exist)          -> slot machinery
  // dead:     constant length, NO loads, and every store provably unable to
  //           trap (constant in-range index, or `nt` from the range
  //           analysis) -> drop the stores; Len -> L; keep the Alloc (an
  //           earlier arena slot must not shift)
  struct Cls {
    uint32_t id{};
    val_t len{};
    bool lift{};
    bool dead{};
  };
  std::vector<Cls> cls(AI.nallocs, {0, -1, false, false});
  {
    std::vector<char> has_load(AI.nallocs, 0), has_store(AI.nallocs, 0),
        bad(AI.nallocs, 0), bad_dead(AI.nallocs, 0);
    std::vector<uint32_t> alloc_instr(AI.nallocs, NONE);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        uint32_t id = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
        if (I.k == K::Alloc && I.dst != NONE && I.dst < nv &&
            AI.single[I.dst] != NONE) {
          if (id == NONE) id = AI.single[I.dst];
          if (I.a != NONE && I.a < nv && kc[I.a] && kv[I.a] >= 0) {
            cls[id].len = kv[I.a];
            alloc_instr[id] = i;
          } else {
            cls[id].len = -1; // runtime length: not classifiable
          }
          continue;
        }
        if (id == NONE) continue;
        if (I.k == K::Load || I.k == K::Store) {
          bool idx_ok = I.b2 != NONE && I.b2 < nv && kc[I.b2] &&
                        kv[I.b2] >= 0 && kv[I.b2] < cls[id].len;
          if (!idx_ok) bad[id] = 1; // slot machinery needs compile-time elems
          if (I.k == K::Load) {
            has_load[id] = 1;
          } else {
            has_store[id] = 1;
            if (!idx_ok && !I.nt)
              bad_dead[id] = 1; // a may-trap store must stay (trap parity)
          }
        }
      }
    uint32_t budget = MJZ_MEM2REG_MAX_SLOTS;
    for (uint32_t id = 0; id < AI.nallocs; ++id) {
      if (cls[id].len < 0 || alloc_instr[id] == NONE) continue;
      if (has_load[id] && !bad[id] && cls[id].len <= MJZ_MEM2REG_MAX_ELEMS &&
          budget >= (uint32_t)cls[id].len) {
        cls[id].lift = true;
        budget -= (uint32_t)cls[id].len;
      } else if (!has_load[id] && !bad_dead[id]) {
        cls[id].dead = true; // never read: stores are pure dead weight
      }
    }
  }
  bool any_lift = false, any_dead = false;
  for (const Cls &c : cls) {
    any_lift |= c.lift;
    any_dead |= c.dead;
  }
  if (!any_lift && !any_dead) return false;
  // ---- defensive pre-check (before ANY mutation): every access of a ------
  // lifted array must be dominated by the allocation's block, and ordered
  // after the allocation when both share a block. SSA's "def dominates use"
  // makes this a theorem for the handle's uses; verify it anyway so the
  // renaming walk below can never encounter an empty slot stack.
  {
    std::vector<uint32_t> alloc_idx(AI.nallocs, NONE),
        alloc_blk(AI.nallocs, NONE);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Alloc && I.dst != NONE && I.dst < nv &&
            AI.single[I.dst] != NONE) {
          alloc_idx[AI.single[I.dst]] = i;
          alloc_blk[AI.single[I.dst]] = b;
        }
      }
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k != K::Load && I.k != K::Store) continue;
        uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
        if (h == NONE || !cls[h].lift) continue;
        uint32_t ab = alloc_blk[h];
        if (ab == NONE || !A.dominates(ab, b) ||
            (ab == b && i < alloc_idx[h]))
          cls[h].lift = false;
      }
    any_lift = false;
    for (const Cls &c : cls) any_lift |= c.lift;
    if (!any_lift && !any_dead) return false;
  }
  // ---- lifted arrays: slot bookkeeping --------------------------------------
  uint32_t total_slots = 0;
  for (const Cls &c : cls)
    if (c.lift) total_slots += (uint32_t)c.len;
  // slot id = slot_base[id] + element index
  std::vector<uint32_t> slot_base(AI.nallocs, NONE);
  {
    uint32_t next = 0;
    for (uint32_t id = 0; id < AI.nallocs; ++id)
      if (cls[id].lift) {
        slot_base[id] = next;
        next += (uint32_t)cls[id].len;
      }
  }
  // def blocks per slot: the Alloc block (init) + each Store's block
  std::vector<std::vector<ulen>> sdefs(total_slots);
  {
    std::vector<uint32_t> alloc_block(AI.nallocs, NONE);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Alloc && I.dst < nv && AI.single[I.dst] != NONE)
          alloc_block[AI.single[I.dst]] = b;
      }
    for (uint32_t id = 0; id < AI.nallocs; ++id)
      if (cls[id].lift)
        for (val_t j = 0; j < cls[id].len; ++j)
          sdefs[slot_base[id] + (uint32_t)j].push_back(alloc_block[id]);
    for (uint32_t b = 0; b < nb; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Store && I.a < nv && AI.single[I.a] != NONE &&
            cls[AI.single[I.a]].lift)
          sdefs[slot_base[AI.single[I.a]] + (uint32_t)kv[I.b2]].push_back(b);
      }
  }
  // ---- phi placement at each slot's IDF -------------------------------------
  std::vector<std::vector<uint32_t>> phis_at(nb);
  for (uint32_t s = 0; s < total_slots; ++s) {
    if (sdefs[s].size() < 2) continue;
    std::vector<ulen> idf =
        gns::calculate_iterated_dominance_frontiers_given_dominance_frontiers<
            V>(A.df, sdefs[s]);
    for (ulen blk : idf) {
      uint32_t b = (uint32_t)blk;
      if (b >= nb || fn.preds[b].size() < 2) continue;
      bool dup = false;
      for (uint32_t x : phis_at[b]) dup |= (x == s);
      if (!dup) phis_at[b].push_back(s);
    }
  }
  // ---- rebuild: slot phis at block starts, init consts after Allocs,
  //      drop dead-array stores -----------------------------------------------
  std::vector<uint32_t> init_vreg(total_slots, NONE);
  std::vector<char> drop(fn.code.size(), 0);
  {
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size() + total_slots + 16);
    for (uint32_t b = 0; b < nb; ++b) {
      uint32_t old_begin = fn.blocks[b].begin;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t s : phis_at[b]) // slot phis go FIRST (before var phis)
        nc.push_back({.k = K::Phi, .c = (int64_t)s});
      for (uint32_t i = old_begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Store && I.a < nv && AI.single[I.a] != NONE &&
            cls[AI.single[I.a]].dead) {
          drop[i] = 1;
          os.mem2reg_dses++;
          continue;
        }
        nc.push_back(I);
        if (I.k == K::Alloc && I.dst < nv && AI.single[I.dst] != NONE &&
            cls[AI.single[I.dst]].lift) {
          uint32_t base = slot_base[AI.single[I.dst]];
          for (val_t j = 0; j < cls[AI.single[I.dst]].len; ++j) {
            uint32_t v = new_anon_vreg(fn, 0);
            init_vreg[base + (uint32_t)j] = v;
            nc.push_back({.k = K::Const, .dst = v, .c = 0});
          }
          os.mem2reg_lifts++;
        }
      }
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
  // ---- renaming: dominator-tree walk with per-slot version stacks ----------
  // (SSA guarantees every access of a lifted array is dominated by the
  // allocation's block -- the handle's def dominates its uses -- so the
  // init version is always on the stack when an access is rewritten.)
  {
    std::vector<std::vector<uint32_t>> stacks(total_slots);
    for (uint32_t s = 0; s < total_slots; ++s)
      if (init_vreg[s] != NONE) stacks[s].push_back(init_vreg[s]);
    struct Frame {
      uint32_t block, child_idx;
      std::vector<uint32_t> pushed; // slots this block pushed (pop on exit)
    };
    std::vector<Frame> wstack;
    // visit: rewrite this block's accesses; seed slot-phi args in successors
    auto visit = [&](uint32_t b, std::vector<uint32_t> &pushed) {
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k == K::Phi) {
          if (I.dst == NONE) { // slot-phi placeholder: assign its result
            uint32_t s = (uint32_t)I.c;
            I.dst = new_anon_vreg(fn, 0);
            stacks[s].push_back(I.dst);
            pushed.push_back(s);
          }
          continue; // variable phis: args are plain vreg references
        }
        uint32_t h = (I.a != NONE && I.a < nv) ? AI.single[I.a] : NONE;
        if (I.k == K::Load && h != NONE && cls[h].lift) {
          uint32_t s = slot_base[h] + (uint32_t)kv[I.b2];
          if (stacks[s].empty()) return false; // cannot happen (dominance)
          I.k = K::Copy;
          I.a = stacks[s].back();
          I.b2 = NONE;
        } else if (I.k == K::Store && h != NONE && cls[h].lift) {
          uint32_t s = slot_base[h] + (uint32_t)kv[I.b2];
          uint32_t v = new_anon_vreg(fn, 0);
          I.k = K::Copy;
          I.dst = v;
          I.a = I.c2; // the stored value
          I.b2 = NONE;
          I.c2 = NONE;
          stacks[s].push_back(v);
          pushed.push_back(s);
        } else if (I.k == K::Len && h != NONE &&
                   (cls[h].lift || cls[h].dead)) {
          I.k = K::Const;
          I.c = cls[h].len;
          I.a = NONE;
        }
      }
      // seed slot-phi args in successors: the value at the END of this
      // block reaches the successor along this edge. Slot phis are exactly
      // the first phis_at[s].size() phi instrs of s (insertion order).
      for (uint32_t s2 : {fn.succ0(b), fn.succ1(b)}) {
        if (s2 == NONE || s2 >= nb) continue;
        uint32_t k = 0;
        for (uint32_t i = fn.blocks[s2].begin; i < fn.blocks[s2].end; ++i) {
          Instr &I = fn.code[i];
          if (I.k != K::Phi) break;
          if (k < phis_at[s2].size()) {
            uint32_t s = phis_at[s2][k];
            uint32_t slot = NONE; // b's position among s2's preds
            for (uint32_t j = 0; j < fn.preds[s2].size(); ++j)
              if (fn.preds[s2][j] == b) slot = j;
            if (slot == NONE) return false;
            uint32_t arg = stacks[s].empty() ? NONE : stacks[s].back();
            if (slot == 0) { I.a = arg; I.t = b; }
            else           { I.b2 = arg; I.f = b; }
          }
          ++k;
        }
      }
      return true;
    };
    std::vector<uint32_t> pu{};
    if (!visit(0, pu)) return false; // defensive: refuse (nothing mutated
                                     // by visit at that point on failure)
    wstack.push_back({0, 0, std::move(pu)});
    while (!wstack.empty()) {
      Frame &fr = wstack.back();
      auto ch = A.dom_tree.range()[fr.block];
      if (fr.child_idx < (uint32_t)ch.size()) {
        uint32_t next = (uint32_t)ch[fr.child_idx++];
        std::vector<uint32_t> pu2{};
        visit(next, pu2);
        wstack.push_back({next, 0, std::move(pu2)});
      } else {
        for (auto it = fr.pushed.rbegin(); it != fr.pushed.rend(); ++it)
          stacks[*it].pop_back();
        wstack.pop_back();
      }
    }
  }
  return true;
}

// --- 9b2. linear forms: shared by reassociation & loop closed forms ------
// A linear form is  sum(coeff_i * v_i) + K  with terms sorted by vreg id.
// Two's-complement +,-,* are exact mod 2^64 and never trap, so collecting
// and re-emitting them is always sound.
struct LinForm {
  std::vector<std::pair<uint32_t, val_t>> t{}; // (vreg, coeff != 0), sorted
  val_t c{};
};
constexpr void lin_term(LinForm &L, val_t k, uint32_t v) {
  if (k == 0) return;
  for (size_t i = 0; i < L.t.size(); ++i) {
    if (L.t[i].first == v) {
      L.t[i].second = (val_t)((uint64_t)L.t[i].second + (uint64_t)k);
      if (L.t[i].second == 0) L.t.erase(L.t.begin() + (long)i);
      return;
    }
  }
  size_t i = 0;
  while (i < L.t.size() && L.t[i].first < v) ++i;
  L.t.insert(L.t.begin() + (long)i, {v, k});
}
constexpr void lin_const(LinForm &L, val_t k, val_t cv) {
  L.c = (val_t)((uint64_t)L.c + (uint64_t)k * (uint64_t)cv);
}
// Accumulate k*v into L. Constants fold; SINGLE-USE +,-,*by-const (and Copy)
// defs located in `scope` blocks are absorbed recursively (their one user is
// the chain being expanded); everything else becomes a leaf term. `use_idx`
// (or NONE) enforces def-before-use when scopes are block-local. Absorbed
// instruction indices are recorded in `absorbed`.
constexpr void lin_expand(const Fn &fn, uint32_t nv,
                          const std::vector<uint32_t> &defi,
                          const std::vector<uint32_t> &defb,
                          const std::vector<char> &kc,
                          const std::vector<val_t> &kv,
                          const std::vector<uint32_t> &uses,
                          const std::vector<uint32_t> &ndefs,
                          const std::vector<char> &scope, uint32_t use_idx,
                          LinForm &L, val_t k, uint32_t v,
                          std::vector<uint32_t> &absorbed) {
  if (v == NONE || k == 0) return;
  if (v < nv && kc[v]) { lin_const(L, k, kv[v]); return; }
  if (v < nv && defi[v] != NONE && ndefs[v] == 1 && uses[v] == 1 &&
      defb[v] < (uint32_t)scope.size() && scope[defb[v]] &&
      (use_idx == NONE || defi[v] < use_idx)) {
    const Instr &D = fn.code[defi[v]];
    if (D.k == K::Bin && D.b == B::Add) {
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L, k,
                 D.a, absorbed);
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L, k,
                 D.b2, absorbed);
      absorbed.push_back(defi[v]);
      return;
    }
    if (D.k == K::Bin && D.b == B::Sub) {
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L, k,
                 D.a, absorbed);
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L,
                 (val_t)(0 - (uint64_t)k), D.b2, absorbed);
      absorbed.push_back(defi[v]);
      return;
    }
    if (D.k == K::Bin && D.b == B::Mul) {
      if (D.b2 != NONE && D.b2 < nv && kc[D.b2]) {
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L,
                   (val_t)((uint64_t)k * (uint64_t)kv[D.b2]), D.a, absorbed);
        absorbed.push_back(defi[v]);
        return;
      }
      if (D.a != NONE && D.a < nv && kc[D.a]) {
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L,
                   (val_t)((uint64_t)k * (uint64_t)kv[D.a]), D.b2, absorbed);
        absorbed.push_back(defi[v]);
        return;
      }
    }
    if (D.k == K::Copy && D.a != NONE) { // identity through a copy
      lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, defi[v], L, k,
                 D.a, absorbed);
      absorbed.push_back(defi[v]);
      return;
    }
  }
  lin_term(L, k, v); // leaf
}
// minimal-instruction cost of materializing L (see lin_emit)
constexpr uint32_t lin_cost(const LinForm &L) {
  uint32_t n = 0, terms = 0;
  bool first = true;
  for (const auto &p : L.t) {
    if (p.second == 0) continue;
    ++terms;
    if (first) {
      n += (p.second == 1) ? 0 : 2; // Mul by const k (any sign) or bare v
      first = false;
    } else {
      n += (p.second == 1 || p.second == -1) ? 1 : 3; // (Const+Mul) + Add/Sub
    }
  }
  if (!terms) return 1;                          // pure constant
  if (L.c != 0) n += 2;                          // Const + Add
  return n ? n : 1;                              // single +x term: Copy
}
// materialize L into `out`; the LAST instruction defines `dst`
constexpr void lin_emit(Fn &fn, std::vector<Instr> &out, const LinForm &L,
                        uint32_t dst) {
  std::vector<std::pair<uint32_t, val_t>> ts{};
  for (const auto &p : L.t)
    if (p.second != 0) ts.push_back(p);
  if (ts.empty()) {
    out.push_back({.k = K::Const, .dst = dst, .c = L.c});
    return;
  }
  auto mkconst = [&](val_t c) {
    uint32_t v = new_anon_vreg(fn, 0);
    out.push_back({.k = K::Const, .dst = v, .c = c});
    return v;
  };
  uint32_t acc = NONE;
  for (size_t j = 0; j < ts.size(); ++j) {
    uint32_t v = ts[j].first;
    val_t k = ts[j].second;
    uint32_t t = v;
    if (j == 0) {
      if (k == 1) {
        acc = v;
      } else if (k == -1) { // 0 - v
        uint32_t z = mkconst(0);
        uint32_t r = new_anon_vreg(fn, fn.base_of(v));
        out.push_back({.k = K::Bin, .b = B::Sub, .dst = r, .a = z, .b2 = v});
        acc = r;
      } else { // Mul by the (possibly negative) constant itself
        uint32_t cv = mkconst(k);
        uint32_t r = new_anon_vreg(fn, fn.base_of(v));
        out.push_back({.k = K::Bin, .b = B::Mul, .dst = r, .a = v, .b2 = cv});
        acc = r;
      }
      continue;
    }
    // subsequent terms: k == 1 -> Add(v); k == -1 -> Sub(v); otherwise
    // scale by k itself (the Mul carries the sign) and Add — exact even for
    // k == INT64_MIN, whose "magnitude" would wrap back to itself
    if (k != 1 && k != -1) {
      uint32_t cv = mkconst(k);
      uint32_t r = new_anon_vreg(fn, fn.base_of(v));
      out.push_back({.k = K::Bin, .b = B::Mul, .dst = r, .a = v, .b2 = cv});
      t = r;
    }
    uint32_t r = new_anon_vreg(fn, fn.base_of(v));
    out.push_back({.k = K::Bin, .b = (k == -1) ? B::Sub : B::Add, .dst = r,
                   .a = acc, .b2 = t});
    acc = r;
  }
  if (L.c != 0) {
    uint32_t cv = mkconst(L.c);
    uint32_t r = new_anon_vreg(fn, 0);
    out.push_back({.k = K::Bin, .b = B::Add, .dst = r, .a = acc, .b2 = cv});
    acc = r;
  }
  // acc must land in dst: rewrite the last emitted def, or Copy
  if (!out.empty() && out.back().dst == acc && acc != dst) {
    out.back().dst = dst;
  } else if (acc != dst) {
    out.push_back({.k = K::Copy, .dst = dst, .a = acc});
  }
}

// --- 9b3. reassociation: block-local linear-combination canonicalization -----
// Chains of +, - and *-by-constant inside ONE basic block are collected into
// a canonical linear form and re-emitted minimally at the chain's root:
//   ((s + i*1) - 1 + i*2 - 2 + ... + i*9) - 9   ->   (s + i*45) - 45
// Only SINGLE-USE intermediates are absorbed, so no value ever moves past
// another use; a rewrite is committed only when it strictly reduces the
// instruction count (progress guaranteed; the rebuilt chain is already
// minimal, so a second run is a no-op).
constexpr bool opt_reassoc(Fn &fn, OptStats &os) {
  const uint32_t nb = fn.nblocks();
  if (!nb) return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  std::vector<uint32_t> uses(nv, 0), ndefs(nv, 0), defi(nv, NONE),
      defb(nv, NONE);
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < nv) {
        ++ndefs[I.dst];
        defi[I.dst] = i;
        defb[I.dst] = b;
      }
      auto bump = [&](uint32_t v) {
        if (v != NONE && v < nv) ++uses[v];
      };
      bump(I.a);
      bump(I.b2);
      bump(I.c2);
      for (uint32_t v : I.args) bump(v);
    }
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const && I.dst != NONE && I.dst < nv &&
          ndefs[I.dst] == 1) {
        kc[I.dst] = 1;
        kv[I.dst] = I.c;
      }
    }
  bool changed = false;
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size());
  std::vector<Block> nbl(nb);
  std::vector<char> scope(nb, 0);
  for (uint32_t b = 0; b < nb; ++b) {
    nbl[b].begin = (uint32_t)nc.size();
    scope.assign(nb, 0);
    scope[b] = 1;
    std::vector<uint32_t> dropped{}; // instrs absorbed by earlier roots
    auto was_dropped = [&](uint32_t i) {
      for (uint32_t x : dropped) if (x == i) return true;
      return false;
    };
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      if (was_dropped(i)) continue;
      const Instr &I = fn.code[i];
      const bool root = I.k == K::Bin && (I.b == B::Add || I.b == B::Sub) &&
                        I.dst != NONE && I.dst < nv && ndefs[I.dst] == 1;
      if (root) {
        LinForm L{};
        std::vector<uint32_t> absr{};
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, i, L,
                   (val_t)1, I.a, absr);
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, i, L,
                   I.b == B::Add ? (val_t)1 : (val_t)-1, I.b2, absr);
        if (lin_cost(L) < (uint32_t)absr.size() + 1) {
          lin_emit(fn, nc, L, I.dst);
          for (uint32_t x : absr) dropped.push_back(x);
          os.reassocs++;
          os.reassoc_gone += (uint32_t)absr.size() + 1 - lin_cost(L);
          changed = true;
          continue; // root replaced by the materialized chain
        }
      }
      nc.push_back(I);
    }
    nbl[b].end = (uint32_t)nc.size();
  }
  if (changed) {
    fn.code = std::move(nc);
    fn.blocks = std::move(nbl);
  }
  return changed;
}

// --- 9a6. division/modulo by constant -> multiply + shift + bit ops ----------
// The reciprocal (inv, shift) for |D| comes from mjz_lib's
// get_devide_inverse_and_shift (uintN.hpp), sized for the range [0, 2^63]
// (every |int64|, INT64_MIN's bit pattern included). Soundness gate, checked
// per divisor (eps = inv*D - 2^shift >= 0, M' = (2^63+1)/D):
//     M'*eps < inv   <=>   floor(n*inv / 2^shift) == n/D  for all n <= 2^63
// (verified against 119M brute-force cases across odd/even/huge divisors).
// Signed lowering (truncating; no trap is possible: D is a known safe const):
//   |n| = (n ^ (n>>a63)) - (n>>a63);  q = mulhi(|n|, inv) >> (shift-64);
//   sign re-applied by xor/sub;  D<0 negates;  r = n - q*D (wrapping-exact).
// Powers of two: q = (n + ((n>>a63) & (2^t-1))) >>a t;
//   r = (n & (2^t-1)) - ((n>>a63) & ((n & (2^t-1)) != 0 ? 2^t : 0)).
constexpr bool divmagic_of(val_t D, uint64_t &inv, uint32_t &shift) {
  uint64_t Dam = (uint64_t)(D < 0 ? (val_t)(0 - (uint64_t)D) : D);
  if (Dam < 3 || (Dam & (Dam - 1)) == 0) return false; // pow2: shifts only
  auto [invN, sh, bits, ss2, thr] =
      get_devide_inverse_and_shift<V, 64>((uint64_t)1 << 63, Dam, false,
                                          (uint64_t)0, false);
  (void)bits;
  (void)ss2;
  (void)thr;
  if (invN.nth_word(1) != 0 || invN.nth_word(2) != 0) return false;
  uint32_t s = (uint32_t)sh;
  if (s < 64 || s > 127) return false;
  uint64_t m = (uint64_t)invN.nth_word(0);
  // exactness gate (128-bit math; every term fits)
  unsigned __int128 prod = (unsigned __int128)m * Dam;
  unsigned __int128 two_s = (unsigned __int128)1 << s;
  if (prod < two_s) return false; // eps < 0: reciprocal too small
  unsigned __int128 eps = prod - two_s;
  unsigned __int128 Mp = (((unsigned __int128)1 << 63) + 1) / Dam;
  if (Mp * eps >= (unsigned __int128)m) return false;
  inv = m;
  shift = s;
  return true;
}
// Build the lowering of `d(n) = I.a <op> D` into `out` (last def = I.dst).
constexpr bool build_divmagic(Fn &fn, const Instr &I, val_t D,
                              std::vector<Instr> &out) {
  const uint32_t n = I.a;
  const bool is_mod = (I.b == B::Mod);
  const uint32_t base = fn.base_of(n);
  auto tmp = [&]() { return new_anon_vreg(fn, base); };
  auto konst = [&](val_t c) {
    uint32_t v = new_anon_vreg(fn, 0);
    out.push_back({.k = K::Const, .dst = v, .c = c});
    return v;
  };
  auto bin = [&](B b, uint32_t dst, uint32_t a, uint32_t b2) {
    out.push_back({.k = K::Bin, .b = b, .dst = dst, .a = a, .b2 = b2});
  };
  uint64_t Dam = (uint64_t)(D < 0 ? (val_t)(0 - (uint64_t)D) : D);
  const uint32_t c63 = konst(63);
  const uint32_t sg = tmp(); // n >>a 63 (all-ones when negative)
  bin(B::AShr, sg, n, c63);
  uint32_t q = NONE;
  if ((Dam & (Dam - 1)) == 0) { // |D| = 2^t
    uint32_t t = 63 - (uint32_t)__builtin_clzll(Dam);
    uint32_t cm = konst((val_t)(Dam - 1));
    uint32_t bias = tmp();
    bin(B::And, bias, sg, cm);
    uint32_t nb = tmp();
    bin(B::Add, nb, n, bias);
    uint32_t cs = konst((val_t)t);
    if (D > 0 && !is_mod) {
      bin(B::AShr, I.dst, nb, cs); // q = (n + bias) >>a t
      return true;
    }
    q = tmp();
    bin(B::AShr, q, nb, cs);
    if (D < 0) { // q = 0 - q
      uint32_t z = konst(0);
      uint32_t nq = tmp();
      bin(B::Sub, nq, z, q);
      q = nq;
    }
    if (is_mod) {
      uint32_t tt = tmp();
      bin(B::And, tt, n, cm);       // low bits of n
      uint32_t z = konst(0);
      uint32_t nz = tmp();
      bin(B::Ne, nz, tt, z);        // != 0 -> 0/1
      uint32_t cd = konst((val_t)Dam);
      uint32_t m2 = tmp();
      bin(B::Mul, m2, nz, cd);
      uint32_t sub = tmp();
      bin(B::And, sub, sg, m2);
      bin(B::Sub, I.dst, tt, sub);  // r = tt - (neg ? 2^t : 0)
      return true;
    }
    // div, D < 0: q was negated above
    out.push_back({.k = K::Copy, .dst = I.dst, .a = q});
    return true;
  }
  // general: exact reciprocal multiply
  uint64_t inv{};
  uint32_t sh{};
  if (!divmagic_of(D, inv, sh)) return false;
  uint32_t ap = tmp();
  bin(B::Xor, ap, n, sg);
  uint32_t ap2 = tmp();
  bin(B::Sub, ap2, ap, sg); // |n| (pattern 2^63 for INT64_MIN)
  uint32_t ci = konst((val_t)inv);
  uint32_t mh = tmp();
  bin(B::MulHi, mh, ap2, ci);
  uint32_t q0 = mh;
  if (sh > 64) {
    uint32_t cs = konst((val_t)(sh - 64));
    q0 = tmp();
    bin(B::LShr, q0, mh, cs);
  }
  uint32_t xq = tmp();
  bin(B::Xor, xq, q0, sg);
  uint32_t qq = tmp();
  bin(B::Sub, qq, xq, sg); // sign re-applied
  if (D > 0 && !is_mod) {
    out.back().dst = I.dst;
    return true;
  }
  q = qq;
  if (D < 0) {
    uint32_t z = konst(0);
    uint32_t nq = tmp();
    bin(B::Sub, nq, z, q);
    q = nq;
  }
  if (is_mod) {
    uint32_t cd = konst(D);
    uint32_t p = tmp();
    bin(B::Mul, p, q, cd);
    bin(B::Sub, I.dst, n, p); // r = n - q*D (wrapping-exact)
    return true;
  }
  out.push_back({.k = K::Copy, .dst = I.dst, .a = q});
  return true;
}
constexpr bool opt_divmagic(Fn &fn, OptStats &os) {
  const uint32_t nb = fn.nblocks();
  if (!nb) return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  std::vector<uint32_t> ndefs(nv, 0);
  for (const Instr &I : fn.code)
    if (I.dst != NONE && I.dst < nv) ++ndefs[I.dst];
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv && ndefs[I.dst] == 1) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  bool changed = false;
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size());
  std::vector<Block> nbl(nb);
  for (uint32_t b = 0; b < nb; ++b) {
    nbl[b].begin = (uint32_t)nc.size();
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Bin && (I.b == B::Div || I.b == B::Mod) &&
          I.b2 != NONE && I.b2 < nv && kc[I.b2]) {
        val_t D = kv[I.b2];
        const bool aconst = I.a != NONE && I.a < nv && kc[I.a];
        if (!aconst && D != 0 && D != 1 && D != -1) {
          std::vector<Instr> seq{};
          if (build_divmagic(fn, I, D, seq)) {
            for (const Instr &x : seq) nc.push_back(x);
            os.divmagics++;
            changed = true;
            continue; // original Div/Mod replaced
          }
        }
      }
      nc.push_back(I);
    }
    nbl[b].end = (uint32_t)nc.size();
  }
  if (changed) {
    fn.code = std::move(nc);
    fn.blocks = std::move(nbl);
  }
  return changed;
}

// --- 9d2. loop unrolling -------------------------------------------------------
// Full unroll of simple counted loops with a CONSTANT trip count, computed
// EXACTLY (the induction advances with the program's wrapping add/sub, so
// even wrap-around trip counts are honored; the count is capped at `limit`).
// Shape required (all from the lib's natural-loop analysis):
//   * one back edge: the header's unique in-loop predecessor (the latch)
//   * one outside predecessor (the preheader)
//   * the ONLY exit edge is the header's comparison (no `break`s)
//   * induction phi i = phi(i0, i+c) with constant i0 and c != 0
//   * header test `i < bound` / `i <= bound` (either operand order), bound
//     constant
// The loop becomes K body copies + a tail copy of the header (whose test is
// provably false -> Br exit). Header phis turn into per-copy constants (the
// induction) and chained values (accumulators); uses outside the loop are
// rewritten to the tail's exit values; the exit block's phis are rekeyed
// from the header to the tail. constprop + DCE then fold pure arithmetic
// loops into constants at compile time.
constexpr bool opt_unroll(Fn &fn, const An &A, OptStats &os,
                          uint32_t limit) {
  if (!limit) return false;
  if (A.n != fn.nblocks() || !fn.nblocks()) return false;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  std::vector<uint32_t> defi(nv, NONE), defb(nv, NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < nv) {
        defi[I.dst] = i;
        defb[I.dst] = b;
      }
      if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
        kc[I.dst] = 1;
        kv[I.dst] = I.c;
      }
    }
  std::vector<uint32_t> headers;
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h) headers.push_back(h);
  for (size_t i = 0; i + 1 < headers.size(); ++i)
    for (size_t j = i + 1; j < headers.size(); ++j)
      if (A.nest[headers[j]] > A.nest[headers[i]])
        std::swap(headers[i], headers[j]);
  for (uint32_t h : headers) {
    auto row = A.loops.range()[h];
    if (row.empty()) continue;
    std::vector<char> inL(nb, 0);
    for (ulen x : row) inL[(uint32_t)x] = 1;
    inL[h] = 1;
    // unique outside pred (preheader) + unique latch
    uint32_t pre = NONE, latch = NONE, outside = 0;
    for (uint32_t p : fn.preds[h]) {
      if (inL[p]) latch = p;
      else { pre = p; ++outside; }
    }
    if (outside != 1 || pre == NONE || latch == NONE) continue;
    if (fn.blocks[h].begin == fn.blocks[h].end) continue;
    const Instr hterm = fn.code[fn.blocks[h].end - 1];
    if (hterm.k != K::Cbr) continue;
    // the only exit edge is the header's; find the exit and the body arm
    uint32_t X = NONE, body = NONE;
    {
      bool bad = false;
      for (uint32_t b = 0; b < nb && !bad; ++b) {
        if (!inL[b]) continue;
        for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
          if (s == NONE) continue;
          if (!inL[s]) {
            if (b != h) { bad = true; break; } // a break: not unrollable
            if (X == NONE) X = s;
            else if (X != s) { bad = true; break; }
          }
        }
      }
      if (bad || X == NONE) continue;
    }
    if (hterm.t == X && hterm.f != NONE && inL[hterm.f]) body = hterm.f;
    else if (hterm.f == X && inL[hterm.t]) body = hterm.t;
    else continue;
    if (body == h) continue; // self-loop header: skip this rare shape
    // ---- header phis: (dst, pre_arg, latch_arg); ALL of them must match --
    struct HPhi { uint32_t dst, pre_arg, latch_arg; };
    std::vector<HPhi> hphis;
    {
      uint32_t nphi = 0;
      for (uint32_t i = fn.blocks[h].begin; i < fn.blocks[h].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k != K::Phi) break;
        ++nphi;
        uint32_t pa = (I.t == pre) ? I.a : ((I.f == pre) ? I.b2 : NONE);
        uint32_t la = (I.t == latch) ? I.a : ((I.f == latch) ? I.b2 : NONE);
        if (pa == NONE || la == NONE) continue;
        hphis.push_back({I.dst, pa, la});
      }
      if (!nphi || hphis.size() != nphi) continue;
    }
    // ---- find the induction phi + the header test ------------------------
    uint32_t ind = NONE, ind_idx = NONE, cond = hterm.a;
    val_t step_mag = 0;
    bool step_sub = false;
    if (cond == NONE || cond >= nv || defi[cond] == NONE ||
        defb[cond] != h)
      continue;
    for (size_t pi = 0; pi < hphis.size() && ind == NONE; ++pi) {
      uint32_t L = hphis[pi].latch_arg;
      if (L >= nv || defi[L] == NONE || !inL[defb[L]]) continue;
      const Instr D = fn.code[defi[L]];
      if (D.k != K::Bin) continue;
      val_t mag = 0;
      bool sub = false, okst = false;
      if (D.b == B::Add) {
        if (D.a == hphis[pi].dst && D.b2 < nv && kc[D.b2] && kv[D.b2] != 0) {
          mag = kv[D.b2]; okst = true;
        } else if (D.b2 == hphis[pi].dst && D.a < nv && kc[D.a] &&
                   kv[D.a] != 0) {
          mag = kv[D.a]; okst = true;
        }
      } else if (D.b == B::Sub) {
        if (D.a == hphis[pi].dst && D.b2 < nv && kc[D.b2] && kv[D.b2] != 0) {
          mag = kv[D.b2]; sub = true; okst = true;
        }
      }
      if (!okst) continue;
      // the header test compares THIS phi against a constant bound
      const Instr C = fn.code[defi[cond]];
      if (C.k != K::Bin || (C.b != B::Lt && C.b != B::Le)) continue;
      bool uses_it = false;
      if (C.a == hphis[pi].dst && C.b2 < nv && kc[C.b2]) uses_it = true;
      else if (C.b2 == hphis[pi].dst && C.a < nv && kc[C.a]) uses_it = true;
      if (!uses_it) continue;
      if (!kc[hphis[pi].pre_arg]) continue; // i0 must be constant
      ind = hphis[pi].dst;
      ind_idx = (uint32_t)pi;
      step_mag = mag;
      step_sub = sub;
    }
    if (ind == NONE) continue;
    const HPhi IP = hphis[ind_idx];
    const val_t i0 = kv[IP.pre_arg];
    const Instr C = fn.code[defi[cond]];
    const bool lhs_is_ind = (C.a == ind);
    const val_t bound = lhs_is_ind ? kv[C.b2] : kv[C.a];
    uint32_t K = NONE;
    std::vector<val_t> ivk{}; // ivk[k] = induction value AT iteration k,
                              // including the failing K-th test (the exit
                              // value the loop leaves in i)
    {
      val_t iv = i0;
      for (uint32_t k = 0;; ++k) {
        val_t lhs = lhs_is_ind ? iv : bound;
        val_t rhs = lhs_is_ind ? bound : iv;
        bool go = (C.b == B::Lt) ? (lhs < rhs) : (lhs <= rhs);
        ivk.push_back(iv);
        if (!go) { K = k; break; }
        if (k >= limit) break; // more than `limit` iterations: skip
        iv = step_sub ? w_sub(iv, step_mag).v : w_add(iv, step_mag).v;
      }
    }
    if (K == NONE || K > limit) continue;
    uint32_t loop_instrs = 0;
    for (uint32_t b = 0; b < nb; ++b)
      if (inL[b]) loop_instrs += fn.blocks[b].end - fn.blocks[b].begin;
    if ((K + 1) * loop_instrs > 400) continue; // code growth cap
    // ---- safety: outside uses may only reference header-defined values ----
    {
      std::vector<char> loopdef(nv, 0);
      for (uint32_t v = 0; v < nv; ++v)
        if (defi[v] != NONE && inL[defb[v]]) loopdef[v] = 1;
      bool bad = false;
      for (uint32_t b = 0; b < nb && !bad; ++b) {
        if (inL[b]) continue;
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          auto chk = [&](uint32_t v) {
            if (v != NONE && v < nv && loopdef[v] && defb[v] != h)
              bad = true;
          };
          if (I.k == K::Phi) { // only the exit edge from h may carry it
            chk(I.a);
            chk(I.b2);
            if (bad && b == X) {
              // args keyed to preds other than h must not be loop values
              bad = false;
              if (I.t != NONE && I.t != h && I.a != NONE && I.a < nv &&
                  loopdef[I.a])
                bad = true;
              if (I.f != NONE && I.f != h && I.b2 != NONE && I.b2 < nv &&
                  loopdef[I.b2])
                bad = true;
            }
          } else {
            chk(I.a);
            chk(I.b2);
            chk(I.c2);
            for (uint32_t v : I.args) chk(v);
          }
          if (bad) break;
        }
      }
      if (bad) continue;
    }
    // ================= commit =================
    os.unrolls++;
    if (K == 0) {
      // zero iterations: the (provably false) test becomes Br exit; the
      // unreachable body dies and the header phis degenerate to their
      // preheader args in the next unreachable/phi-degen round
      Instr &T = fn.code[fn.blocks[h].end - 1];
      T.k = K::Br;
      T.t = X;
      T.f = NONE;
      T.a = NONE;
      fn.succ[2 * h] = X;
      fn.succ[2 * h + 1] = NONE;
      fn.recompute_preds();
      return true;
    }
    // block / vreg maps for copies 0..K (copy K = header-only tail)
    std::vector<std::vector<uint32_t>> bmap(K + 1,
                                            std::vector<uint32_t>(nb, NONE));
    std::vector<std::vector<uint32_t>> fresh(K + 1,
                                             std::vector<uint32_t>(nv, NONE));
    for (uint32_t k = 0; k <= K; ++k)
      for (uint32_t b = 0; b < nb; ++b) {
        if (!inL[b]) continue;
        if (k == K && b != h) continue; // tail: header only
        uint32_t id = (uint32_t)fn.blocks.size();
        fn.blocks.push_back({});
        fn.succ.push_back(NONE);
        fn.succ.push_back(NONE);
        bmap[k][b] = id;
      }
    for (uint32_t k = 0; k <= K; ++k)
      for (uint32_t v = 0; v < nv; ++v) {
        if (defi[v] == NONE || !inL[defb[v]]) continue;
        if (k == K && defb[v] != h) continue;
        fresh[k][v] = new_anon_vreg(fn, fn.base_of(v));
      }
    // entry value of each header phi at copy k (uses of the phi see this)
    //   induction: the constant i0 + k*step (materialized in the header)
    //   accumulator: k == 0 ? pre_arg : map(latch_arg, k-1)
    std::vector<std::vector<uint32_t>> entry(K + 1,
                                             std::vector<uint32_t>(
                                                 hphis.size(), NONE));
    // map a vreg at copy k (uses): phi -> entry, loop def -> fresh, else v
    std::vector<uint32_t> phi_idx(nv, NONE);
    for (size_t pi = 0; pi < hphis.size(); ++pi) phi_idx[hphis[pi].dst] = (uint32_t)pi;
    auto mapv = [&](uint32_t v, uint32_t k) -> uint32_t {
      if (v == NONE) return NONE;
      if (v < nv && phi_idx[v] != NONE) return entry[k][phi_idx[v]];
      if (v < nv && fresh[k][v] != NONE) return fresh[k][v];
      return v;
    };
    // fill entry rows in k order (the induction gets its const vreg)
    for (uint32_t k = 0; k <= K; ++k) {
      // induction const first (it IS the entry value of the induction)
      entry[k][ind_idx] = fresh[k][ind];
      for (size_t pi = 0; pi < hphis.size(); ++pi) {
        if ((uint32_t)pi == ind_idx) continue;
        entry[k][pi] = (k == 0) ? hphis[pi].pre_arg
                                : mapv(hphis[pi].latch_arg, k - 1);
      }
    }
    // emit copies (append-only: existing block ranges stay valid)
    auto remap_args_of = [&](Instr &c, uint32_t k) {
      for (uint32_t &av : c.args) av = mapv(av, k);
    };
    for (uint32_t k = 0; k <= K; ++k) {
      bool tail = (k == K);
      // header copy: [const i_k] + h's non-phi instrs + Br(body/X)
      {
        uint32_t Hk = bmap[k][h];
        fn.blocks[Hk].begin = (uint32_t)fn.code.size();
        fn.code.push_back({.k = K::Const, .dst = fresh[k][ind],
                           .c = ivk[k]});
        for (uint32_t i = fn.blocks[h].begin; i + 1 < fn.blocks[h].end;
             ++i) {
          const Instr &I = fn.code[i];
          if (I.k == K::Phi) continue;
          Instr c = I;
          if (c.dst != NONE && c.dst < nv) c.dst = fresh[k][c.dst];
          c.a = mapv(c.a, k);
          c.b2 = mapv(c.b2, k);
          if (c.c2 != NONE) c.c2 = mapv(c.c2, k);
          remap_args_of(c, k);
          fn.code.push_back(c);
        }
        uint32_t tgt = tail ? X : bmap[k][body];
        fn.code.push_back({.k = K::Br, .t = tgt});
        fn.blocks[Hk].end = (uint32_t)fn.code.size();
        fn.succ[2 * Hk] = tgt;
        fn.succ[2 * Hk + 1] = NONE;
      }
      if (tail) continue;
      // body copies in original block order
      for (uint32_t b = 0; b < nb; ++b) {
        if (!inL[b] || b == h) continue;
        uint32_t Bk = bmap[k][b];
        fn.blocks[Bk].begin = (uint32_t)fn.code.size();
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (i + 1 == fn.blocks[b].end) { // terminator
            if (I.k == K::Ret || I.k == K::Exit) {
              Instr c = I;
              c.a = mapv(c.a, k);
              fn.code.push_back(c);
            } else if (I.k == K::Br || I.k == K::Cbr) {
              Instr c = I;
              c.a = mapv(c.a, k);
              c.t = (I.t == h) ? bmap[k + 1][h]
                               : ((I.t != NONE && bmap[k][I.t] != NONE)
                                      ? bmap[k][I.t]
                                      : I.t);
              if (I.k == K::Cbr)
                c.f = (I.f == h) ? bmap[k + 1][h]
                                 : ((I.f != NONE && bmap[k][I.f] != NONE)
                                        ? bmap[k][I.f]
                                        : I.f);
              fn.code.push_back(c);
            } else {
              fn.code.push_back(I); // unreachable-block junk: copy as-is
            }
            continue;
          }
          Instr c = I;
          if (I.k == K::Phi) {
            if (c.dst != NONE && c.dst < nv) c.dst = fresh[k][c.dst];
            c.a = mapv(c.a, k);
            c.b2 = mapv(c.b2, k);
            if (c.t != NONE && bmap[k][c.t] != NONE) c.t = bmap[k][c.t];
            if (c.f != NONE && bmap[k][c.f] != NONE) c.f = bmap[k][c.f];
          } else {
            if (c.dst != NONE && c.dst < nv) c.dst = fresh[k][c.dst];
            c.a = mapv(c.a, k);
            c.b2 = mapv(c.b2, k);
            if (c.c2 != NONE) c.c2 = mapv(c.c2, k);
            remap_args_of(c, k);
          }
          fn.code.push_back(c);
        }
        fn.blocks[Bk].end = (uint32_t)fn.code.size();
        fn.succ[2 * Bk] = (bmap[k][fn.succ0(b)] != NONE && inL[fn.succ0(b)])
                              ? bmap[k][fn.succ0(b)]
                              : fn.succ0(b);
        fn.succ[2 * Bk + 1] =
            (fn.succ1(b) != NONE && inL[fn.succ1(b)] &&
             bmap[k][fn.succ1(b)] != NONE)
                ? bmap[k][fn.succ1(b)]
                : fn.succ1(b);
        // the back edge from this copy's latch goes to the NEXT copy
        if (b == latch) fn.succ[2 * Bk] = bmap[k + 1][h];
      }
    }
    // rewire the preheader edge h -> first copy
    for (uint32_t i = fn.blocks[pre].begin; i < fn.blocks[pre].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Br && I.t == h) I.t = bmap[0][h];
      else if (I.k == K::Cbr) {
        if (I.t == h) I.t = bmap[0][h];
        if (I.f == h) I.f = bmap[0][h];
      }
    }
    if (fn.succ[2 * pre] == h) fn.succ[2 * pre] = bmap[0][h];
    if (fn.succ[2 * pre + 1] == h) fn.succ[2 * pre + 1] = bmap[0][h];
    // exit-block phis: the h-edge now arrives from the tail
    for (uint32_t i = fn.blocks[X].begin; i < fn.blocks[X].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k != K::Phi) break;
      if (I.t == h) {
        I.t = bmap[K][h];
        I.a = mapv(I.a, K);
      } else if (I.f == h) {
        I.f = bmap[K][h];
        I.b2 = mapv(I.b2, K);
      }
    }
    // uses outside the loop -> tail values (h-defined only, per the scan)
    for (uint32_t b = 0; b < nb; ++b) {
      if (inL[b]) continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.a != NONE && I.a < nv && defb[I.a] == h) I.a = mapv(I.a, K);
        if (I.b2 != NONE && I.b2 < nv && defb[I.b2] == h) I.b2 = mapv(I.b2, K);
        if (I.c2 != NONE && I.c2 < nv && defb[I.c2] == h) I.c2 = mapv(I.c2, K);
        for (uint32_t &av : I.args)
          if (av < nv && defb[av] == h) av = mapv(av, K);
      }
    }
    fn.recompute_preds();
    return true; // one unroll per call: the fixpoint driver re-analyzes
  }
  return false;
}

// --- 9d5. loop closed-form evaluation (constant trip counts) -----------------
// A counted loop with a constant trip count N whose body is pure scalar math
// (no memory, calls, prints, or inner control flow) does not need to run at
// all: every loop-carried variable x evolves as an AFFINE RECURRENCE
//     x_{k+1} = a*x_k + b*i_k + g      (i = the induction, i_k = i0 + d*k)
// whose closed form is a pair of compile-time constants (K1, K0):
//     x_N = K1 * x0 + K0,
//     K1 = a^N,  K0 = (C + B*(N-1))*S - B*T,  C = b*i0 + g,  B = b*d,
//     S(n) = sum_{j<n} a^j (geometric series),  T(n) = sum_{j<n} j*a^j
// — clang-style additive (a=1) AND geometric (a!=1) series, computed EXACTLY
// mod 2^64 by bit-doubling (no modular inverses: wrapping loops are fine).
// The loop is replaced by `x = K1*x0 + K0` in the preheader (x0 may be a
// runtime value!). Bodies that are not affine (x*x, mutual dependencies)
// fall back to exact compile-time SIMULATION of the N iterations (traps
// abort the rewrite and keep the loop). One loop per call; the fixpoint
// driver re-analyzes (inner loops fold first, enabling their parents).
inline constexpr uint64_t LCF_TRIP_BUDGET = (uint64_t)1 << 21; // ~2.1M trips
inline constexpr uint64_t LCF_SIM_BUDGET = 40000000ull;        // sim steps
constexpr uint64_t lcf_pow(uint64_t a, uint64_t n) { // a^n mod 2^64
  uint64_t r = 1;
  while (n) {
    if (n & 1) r *= a;
    a *= a;
    n >>= 1;
  }
  return r;
}
// S(n) = sum_{j<n} a^j, T(n) = sum_{j<n} j*a^j, out P = a^n, all mod 2^64,
// exact by doubling: S(2m)=S(m)(1+a^m); T(2m)=T(m)(1+a^m)+m*a^m*S(m);
// S(2m+1)=S(2m)+a^(2m); T(2m+1)=T(2m)+(2m)*a^(2m)
constexpr void lcf_geo(uint64_t a, uint64_t n, uint64_t &S, uint64_t &T,
                       uint64_t &P) {
  S = 0;
  T = 0;
  P = 1;
  if (!n) return;
  uint64_t m = 0;
  int hi = 63;
  while (!((n >> hi) & 1)) --hi;
  for (int bit = hi; bit >= 0; --bit) {
    const uint64_t Sm = S, Tm = T, Pm = P;
    S = Sm * (Pm + 1);
    T = Tm * (Pm + 1) + Pm * Sm * m;
    P = Pm * Pm;
    m <<= 1;
    if ((n >> bit) & 1) {
      S += P;
      T += m * P;
      P *= a;
      ++m;
    }
  }
}
constexpr bool opt_lcf(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks()) return false;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  std::vector<uint32_t> defi(nv, NONE), defb(nv, NONE);
  std::vector<uint32_t> uses(nv, 0), ndefs(nv, 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < nv) {
        ++ndefs[I.dst];
        defi[I.dst] = i;
        defb[I.dst] = b;
      }
      auto bump = [&](uint32_t v) {
        if (v != NONE && v < nv) ++uses[v];
      };
      bump(I.a);
      bump(I.b2);
      bump(I.c2);
      for (uint32_t v : I.args) bump(v);
    }
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const && I.dst != NONE && I.dst < nv &&
          ndefs[I.dst] == 1) {
        kc[I.dst] = 1;
        kv[I.dst] = I.c;
      }
    }
  std::vector<uint32_t> headers;
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h) headers.push_back(h);
  for (size_t i = 0; i + 1 < headers.size(); ++i)
    for (size_t j = i + 1; j < headers.size(); ++j)
      if (A.nest[headers[j]] > A.nest[headers[i]])
        std::swap(headers[i], headers[j]);
  for (uint32_t h : headers) {
    auto row = A.loops.range()[h];
    if (row.empty()) continue;
    std::vector<char> inL(nb, 0);
    for (ulen x : row) inL[(uint32_t)x] = 1;
    inL[h] = 1;
    uint32_t pre = NONE, latch = NONE, outside = 0;
    for (uint32_t p : fn.preds[h]) {
      if (inL[p]) latch = p;
      else { pre = p; ++outside; }
    }
    if (outside != 1 || pre == NONE || latch == NONE) continue;
    if (fn.blocks[h].begin == fn.blocks[h].end) continue;
    const Instr hterm = fn.code[fn.blocks[h].end - 1];
    if (hterm.k != K::Cbr) continue;
    uint32_t X = NONE, body = NONE;
    {
      bool bad = false;
      for (uint32_t b = 0; b < nb && !bad; ++b) {
        if (!inL[b]) continue;
        for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
          if (s == NONE) continue;
          if (!inL[s]) {
            if (b != h) { bad = true; break; }
            if (X == NONE) X = s;
            else if (X != s) { bad = true; break; }
          }
        }
      }
      if (bad || X == NONE) continue;
    }
    if (hterm.t == X && hterm.f != NONE && inL[hterm.f]) body = hterm.f;
    else if (hterm.f == X && inL[hterm.t]) body = hterm.t;
    else continue;
    if (body == h) continue; // self-loop header: rare shape, skip
    struct HPhi { uint32_t dst, pre_arg, latch_arg; };
    std::vector<HPhi> hphis;
    {
      uint32_t nphi = 0;
      for (uint32_t i = fn.blocks[h].begin; i < fn.blocks[h].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k != K::Phi) break;
        ++nphi;
        uint32_t pa = (I.t == pre) ? I.a : ((I.f == pre) ? I.b2 : NONE);
        uint32_t la = (I.t == latch) ? I.a : ((I.f == latch) ? I.b2 : NONE);
        if (pa == NONE || la == NONE) continue;
        hphis.push_back({I.dst, pa, la});
      }
      if (!nphi || hphis.size() != nphi) continue;
    }
    // ---- induction phi + header test (same shape unroll requires) ----------
    uint32_t ind = NONE, ind_idx = NONE, cond = hterm.a;
    val_t step_mag = 0;
    bool step_sub = false;
    if (cond == NONE || cond >= nv || defi[cond] == NONE ||
        defb[cond] != h)
      continue;
    for (size_t pi = 0; pi < hphis.size() && ind == NONE; ++pi) {
      uint32_t L = hphis[pi].latch_arg;
      if (L >= nv || defi[L] == NONE || !inL[defb[L]]) continue;
      const Instr D = fn.code[defi[L]];
      if (D.k != K::Bin) continue;
      val_t mag = 0;
      bool sub = false, okst = false;
      if (D.b == B::Add) {
        if (D.a == hphis[pi].dst && D.b2 < nv && kc[D.b2] && kv[D.b2] != 0) {
          mag = kv[D.b2];
          okst = true;
        } else if (D.b2 == hphis[pi].dst && D.a < nv && kc[D.a] &&
                   kv[D.a] != 0) {
          mag = kv[D.a];
          okst = true;
        }
      } else if (D.b == B::Sub) {
        if (D.a == hphis[pi].dst && D.b2 < nv && kc[D.b2] && kv[D.b2] != 0) {
          mag = kv[D.b2];
          sub = true;
          okst = true;
        }
      }
      if (!okst) continue;
      const Instr C = fn.code[defi[cond]];
      if (C.k != K::Bin || (C.b != B::Lt && C.b != B::Le)) continue;
      bool uses_it = false;
      if (C.a == hphis[pi].dst && C.b2 < nv) uses_it = true;
      else if (C.b2 == hphis[pi].dst && C.a < nv) uses_it = true;
      if (!uses_it) continue;
      ind = hphis[pi].dst;
      ind_idx = (uint32_t)pi;
      step_mag = mag;
      step_sub = sub;
    }
    if (ind == NONE) {
      // ---- GENERAL SIMULATOR --------------------------------------------
      // The test is not a counted `<`/`<=` induction comparison (e.g.
      // `while (h != 0)` -- Euclid's GCD). If every loop input is a known
      // constant and the body is straight-line pure math, the loop's whole
      // execution is determined: simulate header test + body chain per
      // iteration with the program's exact wrapping/trapping arithmetic; a
      // trap or budget overflow aborts (the loop stays). This is the pass
      // that folds the GCD demo to the constant 21 -- note it needs no
      // induction variable at all, just constants and purity.
      bool ok = true;
      for (const HPhi &p : hphis)
        if (!(p.pre_arg < nv && kc[p.pre_arg])) ok = false;
      { // purity: header = phis + Const/Copy/Bin + Cbr; others + Br only
        for (uint32_t b2 = 0; b2 < nb && ok; ++b2) {
          if (!inL[b2]) continue;
          for (uint32_t i = fn.blocks[b2].begin; i < fn.blocks[b2].end;
               ++i) {
            const Instr &I = fn.code[i];
            if (i + 1 == fn.blocks[b2].end) {
              if (b2 == h) {
                if (I.k != K::Cbr) ok = false;
              } else if (I.k != K::Br)
                ok = false;
              break;
            }
            if (I.k == K::Phi) {
              if (b2 != h) ok = false;
            } else if (I.k == K::Const || I.k == K::Copy) {
            } else if (I.k == K::Bin) {
            } else
              ok = false;
            if (!ok) break;
          }
        }
      }
      { // outside uses may reference ONLY header phi dsts
        std::vector<char> phid(nv, 0);
        for (const HPhi &p : hphis) phid[p.dst] = 1;
        for (uint32_t b2 = 0; b2 < nb && ok; ++b2) {
          if (inL[b2]) continue;
          for (uint32_t i = fn.blocks[b2].begin; i < fn.blocks[b2].end && ok;
               ++i) {
            const Instr &I = fn.code[i];
            auto chk = [&](uint32_t v) {
              if (v != NONE && v < nv && defi[v] != NONE && inL[defb[v]] &&
                  !phid[v])
                ok = false;
            };
            chk(I.a);
            chk(I.b2);
            chk(I.c2);
            for (uint32_t v : I.args) chk(v);
          }
        }
      }
      { // pre must not already reach the exit (phi rekey would dup preds)
        for (uint32_t p : fn.preds[X])
          if (p == pre) ok = false;
      }
      std::vector<uint32_t> chain{};
      if (ok) { // the straight-line body chain from `body` to the latch
        uint32_t cur2 = body, guard = 0;
        while (cur2 != h && guard++ <= nb) {
          chain.push_back(cur2);
          cur2 = fn.succ0(cur2);
        }
        if (cur2 != h) ok = false;
      }
      if (ok) { // every outside-defined operand used inside is a constant
        for (uint32_t b2 = 0; b2 < nb && ok; ++b2) {
          if (!inL[b2]) continue;
          for (uint32_t i = fn.blocks[b2].begin;
               i < fn.blocks[b2].end && ok; ++i) {
            const Instr &I = fn.code[i];
            auto ck = [&](uint32_t v) {
              if (v != NONE && v < nv &&
                  !(defi[v] != NONE && inL[defb[v]]) && !kc[v])
                ok = false;
            };
            ck(I.a);
            ck(I.b2);
            ck(I.c2);
          }
        }
      }
      if (ok) {
        std::vector<val_t> val(nv, 0);
        for (const HPhi &p : hphis) val[p.dst] = kv[p.pre_arg];
        std::vector<val_t> nxt(hphis.size());
        bool trap = false, done = false;
        uint64_t steps = 0;
        auto eval = [&](const Instr &I) -> bool {
          switch (I.k) {
          case K::Const:
            val[I.dst] = I.c;
            return true;
          case K::Copy:
            val[I.dst] = val[I.a];
            return true;
          case K::Bin: {
            Rv r = w_bin(I.b, val[I.a], val[I.b2]);
            if (r.trap) return false;
            val[I.dst] = r.v;
            return true;
          }
          default:
            return false;
          }
        };
        for (uint64_t it = 0; it < LCF_TRIP_BUDGET && !trap && !done; ++it) {
          for (uint32_t i = fn.blocks[h].begin; i + 1 < fn.blocks[h].end;
               ++i) {
            if (fn.code[i].k == K::Phi) continue;
            if (!eval(fn.code[i])) trap = true;
          }
          if (trap) break;
          if (val[hterm.a] == 0) { // header test fails: exit with the
            done = true;           // current phi values as final values
            break;
          }
          for (uint32_t cb : chain) {
            for (uint32_t i = fn.blocks[cb].begin; i + 1 < fn.blocks[cb].end;
                 ++i) // skip the trailing Br (purity-checked above)
              if (!eval(fn.code[i])) trap = true;
            if (trap) break;
          }
          if (trap) break;
          if ((steps += 1 + chain.size()) > LCF_SIM_BUDGET) break;
          for (size_t pi = 0; pi < hphis.size(); ++pi)
            nxt[pi] = val[hphis[pi].latch_arg];
          for (size_t pi = 0; pi < hphis.size(); ++pi)
            val[hphis[pi].dst] = nxt[pi];
        }
        if (done && !trap) {
          // ---- commit: consts into the preheader; the loop dies --------
          std::vector<uint32_t> finalv(hphis.size(), NONE);
          std::vector<Instr> batch{};
          for (size_t pi = 0; pi < hphis.size(); ++pi) {
            const uint32_t fv = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
            finalv[pi] = fv;
            batch.push_back(
                {.k = K::Const, .dst = fv, .c = val[hphis[pi].dst]});
          }
          { // splice into the preheader, before its terminator
            uint32_t pe = fn.blocks[pre].end - 1;
            fn.code.insert(fn.code.begin() + (long)pe, batch.begin(),
                           batch.end());
            const uint32_t kk = (uint32_t)batch.size();
            for (uint32_t x = 0; x < nb; ++x) {
              if (x == pre) {
                fn.blocks[x].end += kk;
                continue;
              }
              if (fn.blocks[x].begin >= pe + 1) fn.blocks[x].begin += kk;
              if (fn.blocks[x].end >= pe + 1) fn.blocks[x].end += kk;
            }
          }
          os.lcf_gensims++;
          // rewire: the preheader jumps straight to the exit; loop dies
          for (uint32_t i = fn.blocks[pre].begin; i < fn.blocks[pre].end;
               ++i) {
            Instr &I = fn.code[i];
            if (I.k == K::Br && I.t == h) I.t = X;
            else if (I.k == K::Cbr) {
              if (I.t == h) I.t = X;
              if (I.f == h) I.f = X;
            }
          }
          if (fn.succ[2 * pre] == h) fn.succ[2 * pre] = X;
          if (fn.succ[2 * pre + 1] == h) fn.succ[2 * pre + 1] = X;
          for (uint32_t b2 = 0; b2 < nb; ++b2) {
            if (inL[b2]) continue;
            for (uint32_t i = fn.blocks[b2].begin; i < fn.blocks[b2].end;
                 ++i) {
              Instr &I = fn.code[i];
              auto rw = [&](uint32_t &v) {
                if (v == NONE || v >= nv) return;
                for (size_t pi = 0; pi < hphis.size(); ++pi)
                  if (v == hphis[pi].dst) {
                    v = finalv[pi];
                    return;
                  }
              };
              rw(I.a);
              rw(I.b2);
              rw(I.c2);
              for (uint32_t &av : I.args) rw(av);
            }
          }
          for (uint32_t i = fn.blocks[X].begin; i < fn.blocks[X].end; ++i) {
            Instr &I = fn.code[i]; // exit phis: h-edge departs from pre now
            if (I.k != K::Phi) break;
            if (I.t == h) I.t = pre;
            else if (I.f == h) I.f = pre;
          }
          fn.recompute_preds();
          return true; // one loop per call: the driver re-analyzes
        }
      }
      continue; // not general-simulable: try nothing else for this loop
    }
    const HPhi IP = hphis[ind_idx];
    const Instr C = fn.code[defi[cond]];
    const bool lhs_is_ind = (C.a == ind);
    const uint32_t Bv = lhs_is_ind ? C.b2 : C.a; // the bound operand
    const bool i0_const = kc[IP.pre_arg];
    const bool bound_const = (Bv != NONE && Bv < nv && kc[Bv]);
    // a runtime bound must be defined outside the loop (otherwise the trip
    // count depends on the body and no closed form exists)
    if (!bound_const) {
      if (Bv == NONE || Bv >= nv) continue;
      if (defi[Bv] != NONE && inL[defb[Bv]]) continue;
    }
    // ---- exact compile-time trip count N (when both sides are constant) ----
    uint64_t N = ~0ull;
    if (i0_const && bound_const) {
      val_t i0 = kv[IP.pre_arg];
      val_t bound = kv[Bv];
      val_t iv = i0;
      for (uint64_t k = 0;; ++k) {
        val_t lhs = lhs_is_ind ? iv : bound;
        val_t rhs = lhs_is_ind ? bound : iv;
        bool go = (C.b == B::Lt) ? (lhs < rhs) : (lhs <= rhs);
        if (!go) { N = k; break; }
        if (k >= LCF_TRIP_BUDGET) break;
        iv = step_sub ? w_sub(iv, step_mag).v : w_add(iv, step_mag).v;
      }
    }
    const bool ct_trip = (N != ~0ull);
    if (ct_trip && N == 0) { // zero trips: test provably false -> exit now
      Instr &T = fn.code[fn.blocks[h].end - 1];
      T.k = K::Br;
      T.t = X;
      T.f = NONE;
      T.a = NONE;
      fn.succ[2 * h] = X;
      fn.succ[2 * h + 1] = NONE;
      fn.recompute_preds();
      return true;
    }
    // ---- safety: outside uses may reference ONLY header phi dsts ----------
    {
      std::vector<char> phid(nv, 0);
      for (const HPhi &p : hphis) phid[p.dst] = 1;
      bool bad = false;
      for (uint32_t b = 0; b < nb && !bad; ++b) {
        if (inL[b]) continue;
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          auto chk = [&](uint32_t v) {
            if (v != NONE && v < nv && defi[v] != NONE && inL[defb[v]] &&
                !phid[v])
              bad = true;
          };
          chk(I.a);
          chk(I.b2);
          chk(I.c2);
          for (uint32_t v : I.args) chk(v);
          if (bad) break;
        }
      }
      if (bad) continue;
    }
    // pre must not already reach the exit (phi rekey would duplicate preds)
    {
      bool bad = false;
      for (uint32_t p : fn.preds[X])
        if (p == pre) bad = true;
      if (bad) continue;
    }
    // ---- body purity: straight-line scalar math only ----------------------
    // (header: phis + Const/Copy/Bin + the Cbr; other blocks: Const/Copy/Bin
    //  ending in Br — no memory, calls, prints, or inner branches)
    {
      bool ok = true;
      for (uint32_t b = 0; b < nb && ok; ++b) {
        if (!inL[b]) continue;
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (i + 1 == fn.blocks[b].end) { // terminator
            if (b == h) {
              if (I.k != K::Cbr) ok = false;
            } else if (I.k != K::Br)
              ok = false;
            break;
          }
          if (I.k == K::Phi) {
            if (b != h) ok = false;
          } else if (I.k == K::Const || I.k == K::Copy) {
          } else if (I.k == K::Bin) {
          } else
            ok = false;
          if (!ok) break;
        }
      }
      if (!ok) continue;
    }
    // ---- may-trap scan: Div/Mod that is not PROVEN trap-free ------------
    // The analytic closed form cannot preserve a runtime trap (it deletes the
    // loop); such loops are only foldable by the trap-checking SIMULATOR, or
    // once trap-elimination has marked every Div/Mod in the loop `nt`.
    bool loop_maytrap = false;
    for (uint32_t b = 0; b < nb && !loop_maytrap; ++b) {
      if (!inL[b]) continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k != K::Bin || (I.b != B::Div && I.b != B::Mod)) continue;
        if (I.nt) continue; // proven trap-free by the range analysis
        if (!(I.b2 < nv && kc[I.b2] && kv[I.b2] != 0)) {
          loop_maytrap = true;
          break;
        }
      }
    }
    // ---- analytic: every phi must evolve as  x' = a*x + b*i + g ------------
    struct Evo { bool ok; uint64_t a, b, g; };
    std::vector<Evo> evo(hphis.size(), {false, 0, 0, 0});
    bool analytic = true;
    {
      std::vector<char> scope(nb, 0);
      for (uint32_t b = 0; b < nb; ++b) scope[b] = inL[b];
      for (size_t pi = 0; pi < hphis.size() && analytic; ++pi) {
        LinForm L{};
        std::vector<uint32_t> absr{};
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, NONE, L,
                   (val_t)1, hphis[pi].latch_arg, absr);
        (void)absr; // informational: the whole loop is removed anyway
        Evo e{true, 0, 0, (uint64_t)L.c};
        for (const auto &p : L.t) {
          if (p.first == hphis[pi].dst)
            e.a = (uint64_t)p.second;
          else if (p.first == ind)
            e.b = (uint64_t)p.second;
          else
            e.ok = false;
        }
        evo[pi] = e;
        analytic &= e.ok;
      }
    }
    std::vector<uint32_t> finalv(hphis.size(), NONE);
    bool folded = false;
    if (!ct_trip) {
      // ---- runtime trip count: guarded closed form + the loop as fallback -
      // Shape: `while (i < B) { pure body; i += d; }` with constant d >= 1
      // and a RUNTIME B (or a constant bound too big to step at compile
      // time). The trip count is computed at runtime together with a
      // wrap-safety guard; the guard fails exactly when the induction would
      // leave the signed range before reaching B (the honest answer is then
      // the original loop), so the closed form only ever runs when exact:
      //   c0 = (i0 < B);  D = B - i0 (wrapping);  Dm = c0 ? D : 0;
      //   N = ceil(Dm/d) [Lt]  or  Dm/d + c0 [Le];   iN = i0 + N*d;
      //   gok = !(c0 && ((D < 0) || (iN < B)))   [Lt; Le: iN <= B]
      // Every loop-carried x evolves additively (a == 1):
      //   x_N = x0 + (b*i0 + g)*N + (b*d) * N(N-1)/2
      // (T = N(N-1)/2 exactly mod 2^64 via the even-factor trick).
      bool rt_ok = !step_sub && step_mag >= 1 && lhs_is_ind;
      if (rt_ok) { // exit must have exactly one pred (the header), no phis
        if (fn.preds[X].size() != 1 || fn.preds[X][0] != h) rt_ok = false;
        for (uint32_t i = fn.blocks[X].begin; i < fn.blocks[X].end && rt_ok;
             ++i)
          if (fn.code[i].k == K::Phi) rt_ok = false;
        const Instr &pt = fn.code[fn.blocks[pre].end - 1];
        if (pt.k != K::Br || pt.t != h) rt_ok = false; // also idempotence
        for (const Evo &e : evo)
          if (!e.ok || e.a != 1) rt_ok = false; // additive recurrences only
      }
      if (!rt_ok) continue;
      const bool is_le = (C.b == B::Le);
      const uint32_t i0v = IP.pre_arg;
      const uint32_t G = fn.nblocks();
      const uint32_t CF = G + 1;
      fn.blocks.push_back({});
      fn.blocks.push_back({});
      fn.succ.push_back(NONE);
      fn.succ.push_back(NONE);
      fn.succ.push_back(NONE);
      fn.succ.push_back(NONE);
      std::vector<Instr> gi{}, cfi{};
      auto konst = [&](std::vector<Instr> &out, val_t c) {
        uint32_t v = new_anon_vreg(fn, 0);
        out.push_back({.k = K::Const, .dst = v, .c = c});
        return v;
      };
      auto bin = [&](std::vector<Instr> &out, B b, uint32_t dst, uint32_t a,
                     uint32_t b2) {
        out.push_back({.k = K::Bin, .b = b, .dst = dst, .a = a, .b2 = b2});
      };
      auto nt = [&]() { return new_anon_vreg(fn, 0); };
      // ---- guard block G: N, iN, gok ----
      const uint32_t z0 = konst(gi, 0);
      const uint32_t c0 = nt();
      bin(gi, is_le ? B::Le : B::Lt, c0, i0v, Bv);
      const uint32_t Dv = nt();
      bin(gi, B::Sub, Dv, Bv, i0v);
      const uint32_t msk = nt();
      bin(gi, B::Sub, msk, z0, c0);
      const uint32_t Dm = nt();
      bin(gi, B::And, Dm, Dv, msk);
      const uint32_t kd = konst(gi, step_mag);
      const uint32_t qv = nt();
      bin(gi, B::Div, qv, Dm, kd);
      const uint32_t Nv = nt();
      if (is_le) {
        bin(gi, B::Add, Nv, qv, c0);
      } else {
        const uint32_t rv = nt();
        bin(gi, B::Mod, rv, Dm, kd);
        const uint32_t rnz = nt();
        bin(gi, B::Ne, rnz, rv, z0);
        bin(gi, B::Add, Nv, qv, rnz);
      }
      const uint32_t diN = nt();
      bin(gi, B::Mul, diN, Nv, kd);
      const uint32_t iN = nt();
      bin(gi, B::Add, iN, i0v, diN);
      const uint32_t dneg = nt();
      bin(gi, B::Lt, dneg, Dv, z0);
      const uint32_t bneg = nt();
      bin(gi, is_le ? B::Le : B::Lt, bneg, iN, Bv);
      const uint32_t ov = nt();
      bin(gi, B::Or, ov, dneg, bneg);
      const uint32_t badv = nt();
      bin(gi, B::And, badv, c0, ov);
      const uint32_t gok = nt();
      bin(gi, B::Eq, gok, badv, z0);
      gi.push_back({.k = K::Cbr, .a = gok, .t = CF, .f = h});
      // ---- closed-form block CF ----
      uint32_t Tv = NONE;
      for (const Evo &e : evo) // T is needed iff some increment uses i
        if (e.b != 0) {
          const uint32_t one = konst(cfi, 1);
          const uint32_t e0 = nt();
          bin(cfi, B::And, e0, Nv, one);
          const uint32_t two = nt();
          bin(cfi, B::Add, two, e0, e0);
          const uint32_t Nm = nt();
          bin(cfi, B::Sub, Nm, Nv, e0);
          const uint32_t h2 = nt();
          bin(cfi, B::LShr, h2, Nm, one);
          const uint32_t t1 = nt();
          bin(cfi, B::Add, t1, Nm, two);
          const uint32_t o1 = nt();
          bin(cfi, B::Sub, o1, t1, one);
          Tv = nt();
          bin(cfi, B::Mul, Tv, h2, o1);
          break;
        }
      std::vector<uint32_t> cfv(hphis.size(), NONE);
      for (size_t pi = 0; pi < hphis.size(); ++pi) {
        const Evo &e = evo[pi];
        const uint32_t x0 = hphis[pi].pre_arg;
        if (e.b == 0 && e.g == 0) {
          cfv[pi] = x0; // untouched by the loop
          continue;
        }
        uint32_t Cv = NONE; // b*i0 + g
        if (e.b == 0) {
          Cv = konst(cfi, (val_t)e.g);
        } else {
          uint32_t bi = i0v;
          if (e.b != 1) {
            uint32_t kb = konst(cfi, (val_t)e.b);
            uint32_t t = nt();
            bin(cfi, B::Mul, t, i0v, kb);
            bi = t;
          }
          if (e.g == 0) {
            Cv = bi;
          } else {
            uint32_t kg = konst(cfi, (val_t)e.g);
            Cv = nt();
            bin(cfi, B::Add, Cv, bi, kg);
          }
        }
        uint32_t acc = x0;
        { // acc += C * N
          uint32_t cn = nt();
          bin(cfi, B::Mul, cn, Cv, Nv);
          uint32_t a2 = nt();
          bin(cfi, B::Add, a2, acc, cn);
          acc = a2;
        }
        if (e.b != 0) { // acc += (b*d) * T
          uint32_t kbd = konst(cfi, (val_t)(e.b * (uint64_t)step_mag));
          uint32_t bt = nt();
          bin(cfi, B::Mul, bt, Tv, kbd);
          uint32_t a3 = nt();
          bin(cfi, B::Add, a3, acc, bt);
          acc = a3;
        }
        cfv[pi] = acc;
      }
      cfi.push_back({.k = K::Br, .t = X});
      // ---- commit: append G/CF, rewrite outside uses, insert merge phis ----
      fn.blocks[G].begin = (uint32_t)fn.code.size();
      for (const Instr &x : gi) fn.code.push_back(x);
      fn.blocks[G].end = (uint32_t)fn.code.size();
      fn.blocks[CF].begin = (uint32_t)fn.code.size();
      for (const Instr &x : cfi) fn.code.push_back(x);
      fn.blocks[CF].end = (uint32_t)fn.code.size();
      fn.succ[2 * G] = CF;
      fn.succ[2 * G + 1] = h;
      fn.succ[2 * CF] = X;
      fn.succ[2 * CF + 1] = NONE;
      { // outside uses of the loop phis -> merge-phi values (below); done
        // BEFORE the phis exist so their own loop-side args stay untouched
        std::vector<uint32_t> phidst(hphis.size(), NONE);
        for (size_t pi = 0; pi < hphis.size(); ++pi)
          phidst[pi] = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
        for (uint32_t b = 0; b < nb; ++b) {
          if (inL[b]) continue;
          for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
            Instr &I = fn.code[i];
            auto rw = [&](uint32_t &v) {
              if (v == NONE || v >= nv) return;
              for (size_t pi = 0; pi < hphis.size(); ++pi)
                if (v == hphis[pi].dst) {
                  v = phidst[pi];
                  return;
                }
            };
            rw(I.a);
            rw(I.b2);
            rw(I.c2);
            for (uint32_t &av : I.args) rw(av);
          }
        }
        std::vector<Instr> xphis{};
        for (size_t pi = 0; pi < hphis.size(); ++pi)
          xphis.push_back({.k = K::Phi, .dst = phidst[pi],
                           .a = hphis[pi].dst, .b2 = cfv[pi],
                           .t = h, .f = CF});
        uint32_t xpos = fn.blocks[X].begin;
        fn.code.insert(fn.code.begin() + (long)xpos, xphis.begin(),
                       xphis.end());
        const uint32_t k = (uint32_t)xphis.size();
        for (uint32_t b = 0; b < fn.nblocks(); ++b) {
          if (b == X) {
            fn.blocks[b].end += k;
            continue;
          }
          if (fn.blocks[b].begin > xpos) fn.blocks[b].begin += k;
          if (fn.blocks[b].end > xpos) fn.blocks[b].end += k;
        }
      }
      { // preheader now enters the guard instead of the header
        for (uint32_t i = fn.blocks[pre].begin; i < fn.blocks[pre].end; ++i) {
          Instr &I = fn.code[i];
          if (I.k == K::Br && I.t == h) I.t = G;
        }
        if (fn.succ[2 * pre] == h) fn.succ[2 * pre] = G;
        if (fn.succ[2 * pre + 1] == h) fn.succ[2 * pre + 1] = G;
      }
      fn.recompute_preds();
      os.lcf_rt++;
      return true; // one loop per call: the fixpoint driver re-analyzes
    }
    auto splice_into_preheader = [&](std::vector<Instr> &batch) {
      uint32_t pe = fn.blocks[pre].end - 1; // terminator (pre-insert)
      fn.code.insert(fn.code.begin() + (long)pe, batch.begin(), batch.end());
      const uint32_t k = (uint32_t)batch.size();
      for (uint32_t x = 0; x < nb; ++x) {
        if (x == pre) {
          fn.blocks[x].end += k;
          continue;
        }
        if (fn.blocks[x].begin >= pe + 1) fn.blocks[x].begin += k;
        if (fn.blocks[x].end >= pe + 1) fn.blocks[x].end += k;
      }
    };
    if (analytic && !loop_maytrap) {
      // closed form: x_N = K1*x0 + K0 with K1 = a^N and the series constants
      std::vector<Instr> batch{};
      const uint64_t Nm1 = N - 1;
      const uint64_t i0u = (uint64_t)kv[IP.pre_arg];
      const uint64_t du =
          step_sub ? (uint64_t)0 - (uint64_t)step_mag : (uint64_t)step_mag;
      for (size_t pi = 0; pi < hphis.size(); ++pi) {
        const Evo &e = evo[pi];
        uint64_t S = 0, T = 0, P = 0;
        lcf_geo(e.a, N, S, T, P);
        const uint64_t K1 = P; // a^N
        const uint64_t Cc = e.b * i0u + e.g;
        const uint64_t Bc = e.b * du;
        const uint64_t K0 = (Cc + Bc * Nm1) * S - Bc * T;
        const uint32_t x0 = hphis[pi].pre_arg;
        const uint32_t fv = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
        finalv[pi] = fv;
        if (K1 == 1 && K0 == 0) {
          batch.push_back({.k = K::Copy, .dst = fv, .a = x0});
        } else if (K1 == 1) {
          uint32_t c = new_anon_vreg(fn, 0);
          batch.push_back({.k = K::Const, .dst = c, .c = (val_t)K0});
          batch.push_back({.k = K::Bin, .b = B::Add, .dst = fv, .a = x0,
                           .b2 = c});
        } else if (K0 == 0) {
          uint32_t c = new_anon_vreg(fn, 0);
          batch.push_back({.k = K::Const, .dst = c, .c = (val_t)K1});
          batch.push_back({.k = K::Bin, .b = B::Mul, .dst = fv, .a = x0,
                           .b2 = c});
        } else {
          uint32_t c1 = new_anon_vreg(fn, 0);
          uint32_t t = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
          batch.push_back({.k = K::Const, .dst = c1, .c = (val_t)K1});
          batch.push_back({.k = K::Bin, .b = B::Mul, .dst = t, .a = x0,
                           .b2 = c1});
          uint32_t c2 = new_anon_vreg(fn, 0);
          batch.push_back({.k = K::Const, .dst = c2, .c = (val_t)K0});
          batch.push_back(
              {.k = K::Bin, .b = B::Add, .dst = fv, .a = t, .b2 = c2});
        }
      }
      splice_into_preheader(batch);
      os.lcf_loops++;
      folded = true;
    } else {
      // ---- fallback: exact compile-time simulation of all N iterations -----
      bool ok = true;
      for (const HPhi &p : hphis)
        if (!(p.pre_arg < nv && kc[p.pre_arg])) ok = false;
      uint64_t loop_instrs = 0;
      for (uint32_t b = 0; b < nb; ++b)
        if (inL[b]) loop_instrs += fn.blocks[b].end - fn.blocks[b].begin;
      if (ok && N * loop_instrs > LCF_SIM_BUDGET) ok = false;
      if (ok) { // every outside-defined operand must be a known constant
        for (uint32_t b = 0; b < nb && ok; ++b) {
          if (!inL[b]) continue;
          for (uint32_t i = fn.blocks[b].begin;
               i < fn.blocks[b].end && ok; ++i) {
            const Instr &I = fn.code[i];
            auto ck = [&](uint32_t v) {
              if (v != NONE && v < nv &&
                  !(defi[v] != NONE && inL[defb[v]]) && !kc[v])
                ok = false;
            };
            ck(I.a);
            ck(I.b2);
            ck(I.c2);
          }
        }
      }
      if (ok) { // the straight-line body chain, from `body` to the latch
        std::vector<uint32_t> chain{};
        uint32_t cur = body, guard = 0;
        while (cur != h && guard++ <= nb) {
          chain.push_back(cur);
          cur = fn.succ0(cur);
        }
        if (cur != h) ok = false;
        if (ok) {
          std::vector<val_t> val(nv, 0);
          for (const HPhi &p : hphis) val[p.dst] = kv[p.pre_arg];
          std::vector<val_t> nxt(hphis.size());
          bool trap = false;
          auto eval = [&](const Instr &I) -> bool {
            switch (I.k) {
            case K::Const:
              val[I.dst] = I.c;
              return true;
            case K::Copy:
              val[I.dst] = val[I.a];
              return true;
            case K::Bin: {
              Rv r = w_bin(I.b, val[I.a], val[I.b2]);
              if (r.trap) return false;
              val[I.dst] = r.v;
              return true;
            }
            default:
              return false;
            }
          };
          for (uint64_t it = 0; it < N && !trap; ++it) {
            for (uint32_t i = fn.blocks[h].begin;
                 i + 1 < fn.blocks[h].end; ++i) {
              if (fn.code[i].k == K::Phi) continue;
              if (!eval(fn.code[i])) trap = true;
            }
            if (trap) break;
            for (uint32_t cb : chain) {
              for (uint32_t i = fn.blocks[cb].begin; i < fn.blocks[cb].end;
                   ++i)
                if (!eval(fn.code[i])) trap = true;
              if (trap) break;
            }
            if (trap) break;
            for (size_t pi = 0; pi < hphis.size(); ++pi)
              nxt[pi] = val[hphis[pi].latch_arg];
            for (size_t pi = 0; pi < hphis.size(); ++pi)
              val[hphis[pi].dst] = nxt[pi];
          }
          if (!trap) {
            std::vector<Instr> batch{};
            for (size_t pi = 0; pi < hphis.size(); ++pi) {
              const uint32_t fv =
                  new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
              finalv[pi] = fv;
              batch.push_back(
                  {.k = K::Const, .dst = fv, .c = val[hphis[pi].dst]});
            }
            splice_into_preheader(batch);
            os.lcf_sims++;
            folded = true;
          }
        }
      }
    }
    if (!folded) continue;
    // ---- rewire: preheader jumps straight to the exit; loop dies -----------
    for (uint32_t i = fn.blocks[pre].begin; i < fn.blocks[pre].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Br && I.t == h) I.t = X;
      else if (I.k == K::Cbr) {
        if (I.t == h) I.t = X;
        if (I.f == h) I.f = X;
      }
    }
    if (fn.succ[2 * pre] == h) fn.succ[2 * pre] = X;
    if (fn.succ[2 * pre + 1] == h) fn.succ[2 * pre + 1] = X;
    for (uint32_t b = 0; b < nb; ++b) { // outside uses of phis -> final values
      if (inL[b]) continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        auto rw = [&](uint32_t &v) {
          if (v == NONE || v >= nv) return;
          for (size_t pi = 0; pi < hphis.size(); ++pi)
            if (v == hphis[pi].dst) {
              v = finalv[pi];
              return;
            }
        };
        rw(I.a);
        rw(I.b2);
        rw(I.c2);
        for (uint32_t &av : I.args) rw(av);
      }
    }
    for (uint32_t i = fn.blocks[X].begin; i < fn.blocks[X].end; ++i) {
      Instr &I = fn.code[i]; // exit phis: the h-edge now departs from pre
      if (I.k != K::Phi) break;
      if (I.t == h) I.t = pre;
      else if (I.f == h) I.f = pre;
    }
    fn.recompute_preds();
    return true; // one loop per call: the driver re-analyzes
  }
  return false;
}

// After passes convert phis to Copy/Const in place, the "all phis first"
// block invariant breaks — consumers scan until the first non-phi. Restore
// it by stably moving remaining phis to the front of each block.
constexpr void normalize_phis(Fn &fn) {
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    uint32_t begin = fn.blocks[b].begin, end = fn.blocks[b].end;
    if (end - begin < 2) continue;
    bool need = false;
    for (uint32_t i = begin; i < end; ++i) {
      if (fn.code[i].k == K::Phi) continue;
      for (uint32_t j = i + 1; j < end; ++j)
        if (fn.code[j].k == K::Phi) { need = true; break; }
      break;
    }
    if (!need) continue;
    std::vector<Instr> phis{}, rest{};
    for (uint32_t i = begin; i < end; ++i)
      (fn.code[i].k == K::Phi ? phis : rest).push_back(fn.code[i]);
    uint32_t w = begin;
    for (const Instr &I : phis) fn.code[w++] = I;
    for (const Instr &I : rest) fn.code[w++] = I;
  }
}

// ============================================================================
// 9e0. PASS MANAGER
// ============================================================================
// PassCfg is the single configuration threaded from the CLI through
// compile() -> optimize_prog() -> optimize(). Passes live in three cost
// tiers and the scheduler runs them with a strict discipline:
//   tier 0 "cheap"    : constprop, copyprop, canon, cse, unreachable,
//                       splice, dce -- always run to a FIXPOINT first
//   tier 1 "middle"   : mem2reg, trapelim(+range analysis), licm, divmagic,
//                       reassoc -- one round each, followed by a tier-0
//                       fixpoint, repeated until global fixpoint / --fp-limit
//   tier 2 "expensive": unroll, lcf -- CFG restructuring, run ONLY after
//                       tiers 0/1 reached their fixpoint; each round is
//                       followed by full tier-0 (+1) fixpoints again
// -O levels gate the tiers:
//   -O0: nothing (SSA + destruction only)
//   -O1: tier 0
//   -O2: tiers 0+1
//   -O3: everything incl. interprocedural inlining (default)
// Per-pass overrides (--enable/--disable) are applied on top of the preset,
// so `-O1 --enable=lcf` is legal. --debug=N controls the dump volume:
//   0 silent; 1 one line per pass invocation; 2 + the IR after each pass
//   that changed something; 3 reserved for analysis dumps.
struct PassCfg {
  uint32_t level{3};   // -O0..-O3 preset
  uint32_t unroll{16}; // full-unroll constant-trip loops with N <= this
  uint32_t fp_limit{24}; // fixpoint round cap for every tier loop
  uint8_t debug{0};    // dump level (0..3)
  bool regalloc{true}; // C/bytecode register allocation (coloring)
  // tier 0
  bool p_fold{true}, p_copyprop{true}, p_canon{true}, p_cse{true},
      p_unreach{true}, p_splice{true}, p_dce{true};
  // tier 1
  bool p_mem2reg{true}, p_trapelim{true}, p_licm{true}, p_divmagic{true},
      p_reassoc{true};
  // tier 2
  bool p_unroll{true}, p_lcf{true};
  // interprocedural
  bool do_inline{true};
  bool do_opt{true}; // false: skip the optimizer entirely
  // program-wide gates (filled in by compile(); see prog_handle_gates)
  bool lift_ok{true};   // mem2reg array promotion allowed
  bool stkarr_ok{true}; // C-backend stack arrays allowed
  // runtime-only debugging context (constexpr path leaves it empty)
  std::string_view dbg_name{};

  constexpr void set_level(uint32_t lv) {
    level = lv;
    bool t0 = lv >= 1, t1 = lv >= 2, t2 = lv >= 3;
    p_fold = p_copyprop = p_canon = p_cse = p_unreach = p_splice = p_dce = t0;
    p_mem2reg = p_trapelim = p_licm = p_divmagic = p_reassoc = t1;
    p_unroll = p_lcf = t2;
    do_inline = t2;
    do_opt = lv != 0;
  }
};

constexpr bool optimize(Fn &fn, OptStats &os, An &final_an,
                        PassCfg cfg = {}); // (defined below)

// --- 9e2. interprocedural: call graph, SCC (mjz_lib), inlining ---------------
// Nodes are functions (index 0 = the main body); edges are call sites. SCCs
// come from mjz::graph_ns::calculate_strongly_connected_components (Tarjan):
//   * a function in a multi-member SCC, or with a self-edge, is RECURSIVE
//     and is never inlined (inlining it would duplicate unbounded work and
//     break the SSA shape)
//   * Tarjan finalizes an SCC only after every SCC it can reach, so walking
//     SCCs in emission order processes callees before callers: inlined
//     bodies propagate through the rounds
struct CallGraph {
  std::vector<std::vector<ulen>> adj{}; // caller -> callees (with dups)
  std::vector<uint32_t> scc_of{};       // fn -> scc index
  std::vector<uint32_t> scc_begin{};    // scc -> start in members
  std::vector<uint32_t> members{};      // concatenated, scc by scc
  uint32_t nscc{};
  std::vector<char> recursive{};
};

constexpr CallGraph build_callgraph(const std::vector<Fn> &fns) {
  CallGraph G{};
  const uint32_t n = (uint32_t)fns.size();
  G.adj.assign(n, {});
  for (uint32_t f = 0; f < n; ++f)
    for (const Instr &I : fns[f].code)
      if (I.k == K::Call) {
        uint32_t c = (uint32_t)I.c;
        if (c < n) G.adj[f].push_back(c);
      }
  auto fs = gns::make_basic_forest<V>(G.adj);
  auto res = gns::calculate_strongly_connected_components<V>(fs);
  G.nscc = (uint32_t)res.nodes_index.size();
  G.members.assign(res.edges.begin(), res.edges.end());
  G.scc_begin.assign(G.nscc, 0);
  for (uint32_t s = 0; s < G.nscc; ++s)
    G.scc_begin[s] = (uint32_t)res.nodes_index[s];
  G.scc_of.assign(n, NONE);
  for (uint32_t s = 0; s < G.nscc; ++s) {
    uint32_t e = (s + 1 < G.nscc) ? G.scc_begin[s + 1]
                                  : (uint32_t)G.members.size();
    for (uint32_t j = G.scc_begin[s]; j < e; ++j)
      G.scc_of[G.members[j]] = s;
  }
  G.recursive.assign(n, 0);
  for (uint32_t f = 0; f < n; ++f) {
    if (G.scc_of[f] == NONE) continue;
    uint32_t s = G.scc_of[f];
    uint32_t e = (s + 1 < G.nscc) ? G.scc_begin[s + 1]
                                  : (uint32_t)G.members.size();
    if (e - G.scc_begin[s] > 1) G.recursive[f] = 1; // mutual recursion
    for (ulen c : G.adj[f])
      if ((uint32_t)c == f) G.recursive[f] = 1;     // self recursion
  }
  return G;
}

// Inline ONE call site `dst = G(args)` in F at instr index ci. Both sides are
// in SSA. The callee's blocks are copied with fresh vreg/block ids; params
// are SUBSTITUTED by the caller's argument vregs (they dominate the call, so
// they dominate the whole inlined region). The caller's block splits at the
// call: everything after it moves to a fresh continuation block M. Several
// returns merge through a chain of 2-pred phi blocks (this IR's phis hold
// exactly two args); the chain end jumps to M. Uses of the call result are
// rewritten to the merged value.
constexpr bool opt_inline_one(Fn &F, const Fn &G, uint32_t ci) {
  if (ci >= F.code.size()) return false;
  const Instr call = F.code[ci]; // value copy: the vector is rebuilt below
  if (call.k != K::Call || call.dst == NONE) return false;
  if (call.args.size() != G.nparams) return false;
  const uint32_t nbF = F.nblocks(), nbG = G.nblocks();
  const uint32_t nvG = G.nvars + (uint32_t)G.version_base.size();
  // G's entry must be phi-free (entry blocks never have phis; paranoia)
  for (uint32_t i = G.blocks[0].begin; i < G.blocks[0].end; ++i)
    if (G.code[i].k == K::Phi) return false;
  // locate the caller block holding the call
  uint32_t B = NONE;
  for (uint32_t b = 0; b < nbF; ++b)
    if (ci >= F.blocks[b].begin && ci < F.blocks[b].end) {
      B = b;
      break;
    }
  if (B == NONE) return false;
  const uint32_t Bs0 = F.succ[2 * B], Bs1 = F.succ[2 * B + 1];
  // vreg map: params -> caller args; all other callee vregs -> fresh ids
  std::vector<uint32_t> vmap(nvG, NONE);
  for (uint32_t v = 0; v < nvG; ++v) {
    if (v < G.nparams) vmap[v] = call.args[v];
    else vmap[v] = new_anon_vreg(F, G.base_of(v));
  }
  auto mapv = [&](uint32_t v) -> uint32_t {
    if (v == NONE || v >= nvG) return v;
    return vmap[v] != NONE ? vmap[v] : v;
  };
  // block map: fresh ids appended to F
  std::vector<uint32_t> bmap(nbG, NONE);
  for (uint32_t g = 0; g < nbG; ++g) bmap[g] = nbF + g;
  // return sites: (block, value)
  std::vector<std::pair<uint32_t, uint32_t>> rets;
  for (uint32_t g = 0; g < nbG; ++g)
    for (uint32_t i = G.blocks[g].begin; i < G.blocks[g].end; ++i)
      if (G.code[i].k == K::Ret && i + 1 == G.blocks[g].end)
        rets.push_back({g, G.code[i].a});
  if (rets.empty()) return false;
  const uint32_t R = (uint32_t)rets.size();
  const uint32_t M = nbF + nbG + (R > 1 ? R - 1 : 0);
  // ---- rebuild the code vector (append-only ids; ranges recomputed) ----
  std::vector<Instr> nc;
  nc.reserve(F.code.size() + G.code.size() + 32);
  std::vector<Block> nblocks(M + 1);
  std::vector<uint32_t> nsucc(2 * (M + 1), NONE);
  // 1) F's blocks; B is truncated at the call and ends with Br G-entry
  for (uint32_t b = 0; b < nbF; ++b) {
    uint32_t base = (uint32_t)nc.size();
    if (b == B) {
      for (uint32_t i = F.blocks[b].begin; i < ci; ++i) nc.push_back(F.code[i]);
      nc.push_back({.k = K::Br, .t = bmap[0]});
      nblocks[b] = {base, (uint32_t)nc.size()};
      nsucc[2 * b] = bmap[0];
      nsucc[2 * b + 1] = NONE;
    } else {
      for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i)
        nc.push_back(F.code[i]);
      nblocks[b] = {base, (uint32_t)nc.size()};
      nsucc[2 * b] = F.succ[2 * b];
      nsucc[2 * b + 1] = F.succ[2 * b + 1];
    }
  }
  // 2) G's blocks, copied; Ret -> Br into the merge chain
  auto ret_target = [&](uint32_t j) -> uint32_t {
    if (R == 1) return M;
    return (j == 0) ? (nbF + nbG) : (nbF + nbG + j - 1);
  };
  for (uint32_t g = 0; g < nbG; ++g) {
    uint32_t base = (uint32_t)nc.size();
    uint32_t ridx = NONE;
    for (uint32_t j = 0; j < R; ++j)
      if (rets[j].first == g) ridx = j;
    for (uint32_t i = G.blocks[g].begin; i < G.blocks[g].end; ++i) {
      const Instr &I = G.code[i];
      if (i + 1 == G.blocks[g].end && I.k == K::Ret) {
        nc.push_back({.k = K::Br, .t = ret_target(ridx)});
        continue;
      }
      Instr c = I;
      if (c.dst != NONE) c.dst = mapv(c.dst);
      c.a = mapv(c.a);
      c.b2 = mapv(c.b2);
      if (c.c2 != NONE) c.c2 = mapv(c.c2);
      for (uint32_t &av : c.args) av = mapv(av);
      if (c.k == K::Phi) {
        if (c.t != NONE && c.t < nbG) c.t = bmap[c.t];
        if (c.f != NONE && c.f < nbG) c.f = bmap[c.f];
      } else if (c.k == K::Br) {
        if (c.t < nbG) c.t = bmap[c.t];
      } else if (c.k == K::Cbr) {
        if (c.t < nbG) c.t = bmap[c.t];
        if (c.f < nbG) c.f = bmap[c.f];
      }
      nc.push_back(c);
    }
    nblocks[bmap[g]] = {base, (uint32_t)nc.size()};
    if (nc.size() > base) {
      const Instr &term = nc.back();
      if (term.k == K::Br) {
        nsucc[2 * bmap[g]] = term.t;
      } else if (term.k == K::Cbr) {
        nsucc[2 * bmap[g]] = term.t;
        nsucc[2 * bmap[g] + 1] = term.f;
      }
    }
  }
  // 3) merge chain (R > 1): m_j = [phi(prev, ret_{j+1})] + Br next-or-M
  uint32_t result_val = NONE;
  if (R > 1) {
    uint32_t prev_res = mapv(rets[0].second);
    for (uint32_t j = 0; j + 1 < R; ++j) {
      uint32_t mb = nbF + nbG + j;
      uint32_t base = (uint32_t)nc.size();
      uint32_t rv = new_anon_vreg(F, 0);
      uint32_t a_src = prev_res;
      uint32_t b_src = mapv(rets[j + 1].second);
      uint32_t pa = (j == 0) ? bmap[rets[0].first] : (nbF + nbG + j - 1);
      uint32_t pb = bmap[rets[j + 1].first];
      Instr phi{.k = K::Phi, .dst = rv};
      if (pa < pb) { // slot order follows ascending pred ids
        phi.a = a_src;
        phi.t = pa;
        phi.b2 = b_src;
        phi.f = pb;
      } else {
        phi.a = b_src;
        phi.t = pb;
        phi.b2 = a_src;
        phi.f = pa;
      }
      nc.push_back(phi);
      uint32_t tgt = (j + 2 < R) ? (nbF + nbG + j + 1) : M;
      nc.push_back({.k = K::Br, .t = tgt});
      nblocks[mb] = {base, (uint32_t)nc.size()};
      nsucc[2 * mb] = tgt;
      prev_res = rv;
    }
    result_val = prev_res;
  } else {
    result_val = mapv(rets[0].second);
  }
  // 4) M: the caller's continuation (instrs after the call + terminator)
  {
    uint32_t base = (uint32_t)nc.size();
    for (uint32_t i = ci + 1; i < F.blocks[B].end; ++i)
      nc.push_back(F.code[i]);
    nblocks[M] = {base, (uint32_t)nc.size()};
    nsucc[2 * M] = Bs0;
    nsucc[2 * M + 1] = Bs1;
  }
  // 5) phis in B's old successors: their edge now departs from M
  for (uint32_t s : {Bs0, Bs1}) {
    if (s == NONE || s >= nbF) continue;
    for (uint32_t i = nblocks[s].begin; i < nblocks[s].end; ++i) {
      Instr &I = nc[i];
      if (I.k != K::Phi) break;
      if (I.t == B) I.t = M;
      if (I.f == B) I.f = M;
    }
  }
  // 6) commit
  F.code = std::move(nc);
  F.blocks = std::move(nblocks);
  F.succ = std::move(nsucc);
  // 7) uses of the call result -> merged value
  for (Instr &I : F.code) {
    if (I.a == call.dst) I.a = result_val;
    if (I.b2 == call.dst) I.b2 = result_val;
    if (I.c2 == call.dst) I.c2 = result_val;
    for (uint32_t &av : I.args)
      if (av == call.dst) av = result_val;
  }
  F.recompute_preds();
  return true;
}

// drop functions unreachable from main; renumber Call targets
constexpr void eliminate_dead_fns(std::vector<Fn> &fns,
                                  std::vector<std::string_view> &names,
                                  std::vector<OptStats> &ostats,
                                  std::vector<uint32_t> &src_index) {
  for (;;) {
    const uint32_t n = (uint32_t)fns.size();
    std::vector<char> reach(n, 0);
    std::vector<uint32_t> wl{0};
    reach[0] = 1;
    while (!wl.empty()) {
      uint32_t f = wl.back();
      wl.pop_back();
      for (const Instr &I : fns[f].code)
        if (I.k == K::Call) {
          uint32_t c = (uint32_t)I.c;
          if (c < n && !reach[c]) {
            reach[c] = 1;
            wl.push_back(c);
          }
        }
    }
    bool any_dead = false;
    for (uint32_t f = 1; f < n; ++f) any_dead |= !reach[f];
    if (!any_dead) return;
    std::vector<uint32_t> remap(n, NONE);
    std::vector<Fn> nf;
    std::vector<std::string_view> nn;
    std::vector<OptStats> no;
    std::vector<uint32_t> nsrc;
    for (uint32_t f = 0; f < n; ++f) {
      if (!reach[f]) continue;
      remap[f] = (uint32_t)nf.size();
      nf.push_back(std::move(fns[f]));
      nn.push_back(names[f]);
      no.push_back(ostats[f]);
      nsrc.push_back(src_index[f]);
    }
    for (Fn &F : nf)
      for (Instr &I : F.code)
        if (I.k == K::Call) I.c = remap[(uint32_t)I.c];
    fns = std::move(nf);
    names = std::move(nn);
    ostats = std::move(no);
    src_index = std::move(nsrc);
  }
}

// Whole-program optimization: per-function fixpoint (incl. unrolling), then
// SCC-guided inlining rounds (each inlined caller is re-optimized so constant
// arguments fold through the inlined body), dead-function elimination, and a
// final pass (inlining can expose constant trip counts worth unrolling).
constexpr void optimize_prog(std::vector<Fn> &fns,
                             std::vector<std::string_view> &names,
                             std::vector<OptStats> &ostats, PassCfg cfg,
                             std::vector<uint32_t> &src_index) {
  for (uint32_t f = 0; f < fns.size(); ++f) {
    An A{};
    PassCfg c1 = cfg;
    if (f < names.size()) c1.dbg_name = names[f];
    optimize(fns[f], ostats[f], A, c1);
  }
  if (!cfg.do_inline || fns.size() < 2) return;
  for (int round = 0; round < 4; ++round) {
    CallGraph G = build_callgraph(fns);
    auto recount = [&]() {
      std::vector<uint32_t> cs(fns.size(), 0);
      for (uint32_t f = 0; f < fns.size(); ++f)
        for (const Instr &I : fns[f].code)
          if (I.k == K::Call) {
            uint32_t c = (uint32_t)I.c;
            if (c < cs.size()) cs[c]++;
          }
      return cs;
    };
    std::vector<uint32_t> callsites = recount();
    bool inlined_any = false;
    for (uint32_t s = 0; s < G.nscc; ++s) {
      uint32_t e = (s + 1 < G.nscc) ? G.scc_begin[s + 1]
                                    : (uint32_t)G.members.size();
      for (uint32_t j = G.scc_begin[s]; j < e; ++j) {
        uint32_t f = G.members[j];
        for (;;) { // rescan after each inline: the fn changed shape
          uint32_t site = NONE, callee = NONE;
          for (uint32_t b = 0; b < fns[f].nblocks() && site == NONE; ++b)
            for (uint32_t i = fns[f].blocks[b].begin;
                 i < fns[f].blocks[b].end; ++i) {
              const Instr &I = fns[f].code[i];
              if (I.k != K::Call) continue;
              uint32_t g = (uint32_t)I.c;
              if (g == 0 || g >= fns.size()) continue; // never inline main
              if (G.recursive[g]) continue;            // SCC: recursive
              uint32_t sz = (uint32_t)fns[g].code.size();
              bool small = sz <= 30;                   // always inline tiny
              bool single_use = callsites[g] == 1 && sz <= 300;
              if (!small && !single_use) continue;
              if (fns[g].nparams != I.args.size()) continue;
              if (fns[f].code.size() + sz > 8000) continue; // growth cap
              site = i;
              callee = g;
              break;
            }
          if (site == NONE) break;
          if (!opt_inline_one(fns[f], fns[callee], site)) break;
          ostats[f].inlines++;
          inlined_any = true;
          callsites = recount();
          { // re-fold this caller; unroll/lcf wait for the final pass
            An A2{};
            PassCfg c2 = cfg;
            c2.p_unroll = false;
            optimize(fns[f], ostats[f], A2, c2);
          }
        }
      }
    }
    if (!inlined_any) break;
  }
  eliminate_dead_fns(fns, names, ostats, src_index);
  for (uint32_t f = 0; f < fns.size(); ++f) {
    An A{};
    PassCfg c3 = cfg;
    if (f < names.size()) c3.dbg_name = names[f];
    optimize(fns[f], ostats[f], A, c3);
  }
}

// --- 9e. driver ----------------------------------------------------------------
// runtime-only pass tracing (never on the constexpr path: guarded by
// cfg.debug, which the compile-time pipeline always leaves at 0)
static void print_fn(const Fn &fn, const char *title); // (defined in 13)
static void dbg_pass(uint8_t debug, const Fn &fn, const char *pass, bool ch) {
  if (debug < 1) return;
  std::printf("  [pass %-9s] %s\n", pass, ch ? "changed" : "fixed");
  if (debug >= 2 && ch) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "IR after %s", pass);
    print_fn(fn, buf);
  }
}
struct Optimized {
  Fn fn{};         // SSA after optimization
  OptStats os{};
  An an{};         // final analysis (post-opt)
};
constexpr bool optimize(Fn &fn, OptStats &os, An &final_an, PassCfg cfg) {
  if (!cfg.do_opt || cfg.level == 0) { // -O0: untouched IR, fresh analysis
    final_an = analyze(fn);
    return false;
  }
  bool any = false;
  const uint32_t cap = cfg.fp_limit ? cfg.fp_limit : 24;
  if (cfg.debug >= 1) {
    char nb[64] = "<fn>";
    if (!cfg.dbg_name.empty())
      std::snprintf(nb, sizeof nb, "%.*s", (int)cfg.dbg_name.size(),
                    cfg.dbg_name.data());
    std::printf("==== optimize %s (level %u, fp-limit %u) ====\n", nb,
                (unsigned)cfg.level, (unsigned)cap);
  }
  // ---- tier 0: cheap simplification to its fixpoint -----------------------
  // (each round must make progress; dominance passes see a fresh An)
  auto tier0 = [&]() -> bool {
    bool ch_any = false;
    for (uint32_t it = 0; it < cap; ++it) {
      bool ch = false, c = false;
      c = opt_constprop(fn, os); // fold/identities/branch folds
      dbg_pass(cfg.debug, fn, "fold", c);
      ch |= c;
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_COPYPROP
      if (cfg.p_copyprop) {
        c = opt_copyprop(fn, os); // copies -> uses of the source
        dbg_pass(cfg.debug, fn, "copyprop", c);
        ch |= c;
      }
#endif
      normalize_phis(fn);
      if (cfg.p_canon) {
        c = opt_canon(fn, os); // canonical forms + NOT-of-compare
        dbg_pass(cfg.debug, fn, "canon", c);
        ch |= c;
      }
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_CSE
      if (cfg.p_cse) {
        An A = analyze(fn);
        c = opt_cse(fn, A, os); // dominating recomputation -> reuse
        dbg_pass(cfg.debug, fn, "cse", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (cfg.p_unreach) {
        c = opt_remove_unreachable(fn, os); // dead blocks + phi degeneration
        dbg_pass(cfg.debug, fn, "unreach", c);
        ch |= c;
      }
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_SPLICE
      if (cfg.p_splice) {
        c = opt_splice(fn, os); // collapse single-pred/succ jump chains
        dbg_pass(cfg.debug, fn, "splice", c);
        ch |= c;
      }
#endif
      normalize_phis(fn);
      if (cfg.p_dce) {
        c = opt_dce(fn, os);
        dbg_pass(cfg.debug, fn, "dce", c);
        ch |= c;
      }
      normalize_phis(fn);
      os.iters = it + 1;
      if (!ch) break;
      ch_any = true;
    }
    return ch_any;
  };
  any |= tier0();
  // ---- tier 1: middle passes; cheap fixpoint after each round -------------
  if (cfg.level >= 2) {
    for (uint32_t outer = 0; outer < cap; ++outer) {
      bool ch = false, c = false;
      if (cfg.p_mem2reg) {
        An A = analyze(fn);
        c = opt_mem2reg(fn, A, os, cfg.lift_ok);
        dbg_pass(cfg.debug, fn, "mem2reg", c);
        ch |= c;
        normalize_phis(fn);
      }
      if (cfg.p_trapelim) {
        An A = analyze(fn);
        c = opt_trapelim(fn, A, os); // ranges + aliasing -> nt flags
        dbg_pass(cfg.debug, fn, "trapelim", c);
        ch |= c;
        normalize_phis(fn);
      }
#ifndef MJZ_DBG_NO_LICM
      if (cfg.p_licm) {
        An A = analyze(fn);
        c = opt_licm(fn, A, os); // hoist invariants to the preheader
        dbg_pass(cfg.debug, fn, "licm", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_DIVMAGIC
      if (cfg.p_divmagic) {
        c = opt_divmagic(fn, os); // /constant -> mulhi+shift
        dbg_pass(cfg.debug, fn, "divmagic", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_REASSOC
      if (cfg.p_reassoc) {
        c = opt_reassoc(fn, os); // canonicalize +/-/*c chains (feeds LCF)
        dbg_pass(cfg.debug, fn, "reassoc", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (!ch) break;
      any = true;
      tier0();
    }
  }
  // ---- tier 2: expensive CFG restructuring, only after 0/1 fixpoint -------
  if (cfg.level >= 3) {
    for (uint32_t outer = 0; outer < cap; ++outer) {
      bool ch = false;
#ifndef MJZ_DBG_NO_UNROLL
      if (cfg.p_unroll && cfg.unroll) {
        An A = analyze(fn);
        bool c = opt_unroll(fn, A, os, cfg.unroll);
        dbg_pass(cfg.debug, fn, "unroll", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_LCF
      if (cfg.p_lcf) {
        An A2 = analyze(fn); // unrolling may have reshaped the CFG
        bool c = opt_lcf(fn, A2, os); // closed forms / simulators
        dbg_pass(cfg.debug, fn, "lcf", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (!ch) break;
      any = true;
      tier0();
      // one tier-1 round: restructuring exposes new opportunities
      if (cfg.level >= 2) {
        bool c2 = false;
        if (cfg.p_mem2reg) {
          An A = analyze(fn);
          c2 |= opt_mem2reg(fn, A, os, cfg.lift_ok);
          normalize_phis(fn);
        }
        if (cfg.p_trapelim) {
          An A = analyze(fn);
          c2 |= opt_trapelim(fn, A, os);
          normalize_phis(fn);
        }
        if (cfg.p_licm) {
          An A = analyze(fn);
          c2 |= opt_licm(fn, A, os);
          normalize_phis(fn);
        }
        if (cfg.p_divmagic) {
          c2 |= opt_divmagic(fn, os);
          normalize_phis(fn);
        }
        if (cfg.p_reassoc) {
          c2 |= opt_reassoc(fn, os);
          normalize_phis(fn);
        }
        (void)c2;
        tier0();
      }
    }
  }
  normalize_phis(fn);
  final_an = analyze(fn);
  return any;
}

// --- 9f2. stack-allocation analysis (for the C backend) ---------------------
// An `array(n)` with constant n whose handle only ever flows (through Copies)
// to Load/Store/Len as the HANDLE operand — never into a phi (an older
// iteration's array could otherwise be observed after the same instruction
// re-allocated its slot), never into arithmetic/comparisons/print/return/call
// arguments, never stored as a value (array-of-arrays) — is provably local:
// its handle value is unobservable, so the C backend can keep it in a stack
// array (zero-filled at the allocation point, exactly like the arena).
// Handles ARE ordinary values: any other use makes the numeric value
// observable and pins the array to the arena (indices must match every
// other runner). Budget: per array <= 16384 elements and <= 1 MiB total,
// to keep native frames far below the default 8 MiB stack.
inline constexpr val_t MJZ_STK_ARR_MAX_ELEMS = 16384;
inline constexpr val_t MJZ_STK_ARR_TOTAL_ELEMS = 131072;
constexpr void analyze_stack_allocs(Fn &fn) {
  fn.stk_arrays.clear();
  const uint32_t nb = fn.nblocks();
  if (!nb) return;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return;
  std::vector<uint32_t> ndefs(nv, 0);
  for (const Instr &I : fn.code)
    if (I.dst != NONE && I.dst < nv) ++ndefs[I.dst];
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv && ndefs[I.dst] == 1) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  val_t total = 0;
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k != K::Alloc) continue;
      if (I.a == NONE || I.a >= nv || !kc[I.a]) continue; // runtime length
      val_t len = kv[I.a];
      if (len < 0 || len == 0 || len > MJZ_STK_ARR_MAX_ELEMS) continue;
      if (total + len > MJZ_STK_ARR_TOTAL_ELEMS) continue;
      // alias closure: the alloc dst + transitive Copy destinations
      std::vector<char> ali(nv, 0);
      ali[I.dst] = 1;
      for (bool grew = true; grew;) {
        grew = false;
        for (const Instr &C : fn.code)
          if (C.k == K::Copy && C.a != NONE && C.a < nv && C.dst < nv &&
              ali[C.a] && !ali[C.dst]) {
            ali[C.dst] = 1;
            grew = true;
          }
      }
      // escape scan over the FINAL alias set
      bool esc = false;
      for (uint32_t b2 = 0; b2 < nb && !esc; ++b2)
        for (uint32_t j = fn.blocks[b2].begin; j < fn.blocks[b2].end && !esc;
             ++j) {
          const Instr &U = fn.code[j];
          auto bad = [&](uint32_t v) { return v != NONE && v < nv && ali[v]; };
          switch (U.k) {
          case K::Copy: // source: alias extension (ok); dst cannot re-define
            break;
          case K::Load: // handle position ok; the INDEX is a value use
            if (bad(U.b2)) esc = true;
            break;
          case K::Store: // handle ok; index and stored value are value uses
            if (bad(U.b2) || bad(U.c2)) esc = true;
            break;
          case K::Len: // handle position ok
            break;
          default: // Bin/Cbr/Print/Call/Ret/Phi/Exit/Input: any alias use
            if (bad(U.a) || bad(U.b2) || bad(U.c2)) esc = true;
            for (uint32_t v : U.args)
              if (bad(v)) esc = true;
            break;
          }
        }
      if (esc) continue;
      total += len;
      Fn::StkArr sa{};
      sa.root = I.dst;
      sa.len = len;
      for (uint32_t v = 0; v < nv; ++v)
        if (ali[v]) sa.vregs.push_back(v);
      fn.stk_arrays.push_back(std::move(sa));
    }
}
// ============================================================================
// 9g. SSA DESTRUCTION — phis -> edge copies
//     1. split critical edges (pred with 2 succs -> block with phis): the
//        edge copies need a block of their own on that edge
//     2. for every pred edge into a phi block, lower the phis to a PARALLEL
//        copy set, then sequentialize it: greedily emit copies whose dst is
//        not a source of any pending copy; a leftover set is a swap cycle,
//        broken by saving one source into a fresh temp (lost-copy fix)
//     3. strip the phis; the result is plain (non-SSA) 3-address code where
//        every vreg is just a slot
// ============================================================================
struct DestrStats {
  uint32_t edges_split{};  // critical edges split
  uint32_t copies{};       // sequentialized edge copies emitted
  uint32_t zeros{};        // undef phi args materialized as Const 0
  uint32_t self_copies{};  // d<-d no-ops skipped
  uint32_t cycles_broken{};// swap cycles broken with a temp
  uint32_t fused{};        // post-destruction copies folded into their source
};

// sequentialize one parallel copy set; appends real instrs to `out`
constexpr void sequentialize(
    Fn &fn, std::vector<std::pair<uint32_t, uint32_t>> &pcs,
    std::vector<Instr> &out, DestrStats &ds) {
  // self-copies are no-ops (the slot already holds the right value on that
  // edge) and would otherwise look like unbreakable 1-cycles
  for (size_t i = 0; i < pcs.size();) {
    if (pcs[i].first == pcs[i].second && pcs[i].second != NONE) {
      pcs.erase(pcs.begin() + (long)i);
      ds.self_copies++;
    } else {
      ++i;
    }
  }
  while (!pcs.empty()) {
    bool progress = false;
    for (size_t i = 0; i < pcs.size(); ++i) {
      uint32_t d = pcs[i].first;
      bool d_needed = false; // is d still a source of a pending copy?
      for (const auto &p : pcs)
        if (p.second == d) { d_needed = true; break; }
      if (d_needed) continue; // emitting now would clobber a needed value
      if (pcs[i].second == NONE) { // undef arg: materialize 0
        out.push_back({.k = K::Const, .dst = d, .c = 0});
        ds.zeros++;
      } else {
        out.push_back({.k = K::Copy, .dst = d, .a = pcs[i].second});
        ds.copies++;
      }
      pcs.erase(pcs.begin() + (long)i);
      progress = true;
      break;
    }
    if (!progress && !pcs.empty()) {
      // swap cycle: save the source into a fresh temp, redirect this copy
      uint32_t d = pcs[0].first, s = pcs[0].second;
      uint32_t t = new_anon_vreg(fn, fn.base_of(d));
      out.push_back({.k = K::Copy, .dst = t, .a = s});
      pcs[0].second = t;
      ds.copies++;
      ds.cycles_broken++;
    }
  }
}

constexpr void split_critical_edges(Fn &fn, DestrStats &ds) {
  uint32_t n0 = fn.nblocks();
  for (uint32_t p = 0; p < n0; ++p) {
    if (fn.succ1(p) == NONE || fn.succ0(p) == fn.succ1(p)) continue;
    for (uint32_t si = 0; si < 2; ++si) {
      uint32_t s = si ? fn.succ1(p) : fn.succ0(p);
      if (s == NONE) continue;
      bool s_has_phi = false;
      for (uint32_t i = fn.blocks[s].begin; i < fn.blocks[s].end; ++i)
        if (fn.code[i].k == K::Phi) { s_has_phi = true; break; }
      if (!s_has_phi) continue; // no copies to place: no split needed
      // fresh block m = [br s] on the edge p -> s
      uint32_t m = fn.nblocks();
      fn.blocks.push_back({});
      fn.succ.push_back(s);
      fn.succ.push_back(NONE);
      fn.blocks[m].begin = (uint32_t)fn.code.size();
      fn.code.push_back({.k = K::Br, .t = s});
      fn.blocks[m].end = (uint32_t)fn.code.size();
      // retarget p's edge s -> m (succ table + terminator instr)
      if (si == 0) fn.succ[2 * p] = m;
      else fn.succ[2 * p + 1] = m;
      for (uint32_t i = fn.blocks[p].begin; i < fn.blocks[p].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k == K::Cbr) {
          if (si == 0 && I.t == s) I.t = m;
          if (si == 1 && I.f == s) I.f = m;
        } else if (I.k == K::Br && I.t == s) {
          I.t = m;
        }
      }
      // rekey phis in s: the arg formerly keyed by pred p now arrives via m
      for (uint32_t i = fn.blocks[s].begin; i < fn.blocks[s].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi) break;
        if (I.t == p) I.t = m;
        if (I.f == p) I.f = m;
      }
      ds.edges_split++;
    }
  }
  fn.recompute_preds();
}

constexpr void destroy_ssa(Fn &fn, DestrStats &ds) {
  split_critical_edges(fn, ds);
  // lower phis to edge copies + strip them, rebuilding the code vector.
  // after the split, every pred of a phi block is single-succ, so its edge
  // copies can sit directly before its terminator.
  // NOTE: ranges are read from a snapshot — the rebuild overwrites
  // fn.blocks[p] while later preds still read phi ranges by OLD coordinates.
  std::vector<Block> old_blocks = fn.blocks;
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size() + 16);
  for (uint32_t p = 0; p < fn.nblocks(); ++p) {
    uint32_t ob = old_blocks[p].begin, oe = old_blocks[p].end;
    uint32_t nbegin = (uint32_t)nc.size();
    if (oe > ob) {
      for (uint32_t i = ob; i + 1 < oe; ++i) // all but the terminator
        if (fn.code[i].k != K::Phi) nc.push_back(fn.code[i]);
      const Instr term = fn.code[oe - 1];
      uint32_t s = fn.succ0(p);
      if (fn.succ1(p) == NONE && s != NONE) {
        std::vector<std::pair<uint32_t, uint32_t>> pcs{};
        for (uint32_t i = old_blocks[s].begin; i < old_blocks[s].end; ++i) {
          const Instr &I = fn.code[i];
          if (I.k != K::Phi) break;
          uint32_t arg = NONE;
          if (I.t == p && I.a != NONE) arg = I.a;
          else if (I.f == p && I.b2 != NONE) arg = I.b2;
          pcs.push_back({I.dst, arg}); // arg NONE -> undef -> zero
        }
        if (!pcs.empty()) sequentialize(fn, pcs, nc, ds);
      }
      nc.push_back(term);
    }
    fn.blocks[p] = {nbegin, (uint32_t)nc.size()};
  }
  fn.code = std::move(nc);
  fn.recompute_preds();
}

// --- 9g2. post-destruction copy fusion ----------------------------------------
// Destruction leaves `t = <expr>; d = t;` pairs (the phi-edge copy of a
// freshly computed temp). When t has exactly ONE reader (that copy), the
// defining instruction's destination is renamed to d and the copy dies.
// The defining op stays exactly where it was, so traps, arena reads and
// stdin consumption do not move; d's only other writers are the *other*
// phi-edge copies, which execute later on their own paths, and d's readers
// are all dominated by the join block — so the early write is never observed
// except on the path the copy served. Copy chains collapse the same way.
// This is the IR-level half of "make the backend input non-redundant"; the
// emission plan below does the rest (immediates, branch fusion).
constexpr void post_destruct_peephole(Fn &fn, DestrStats &ds) {
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  for (int round = 0; round < 256; ++round) {
    if (fn.code.empty()) return;
    std::vector<uint32_t> uses(nv, 0);
    std::vector<uint32_t> def_idx(nv, NONE);
    for (uint32_t i = 0; i < fn.code.size(); ++i) {
      const Instr &I = fn.code[i];
      if (I.a != NONE && I.a < nv) uses[I.a]++;
      if (I.b2 != NONE && I.b2 < nv) uses[I.b2]++;
      if (I.c2 != NONE && I.c2 < nv) uses[I.c2]++;
      for (uint32_t v : I.args)
        if (v < nv) uses[v]++;
      if (I.dst != NONE && I.dst < nv) def_idx[I.dst] = i;
    }
    bool changed = false;
    std::vector<char> drop(fn.code.size(), 0);
    for (uint32_t b = 0; b < fn.nblocks() && !changed; ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end && !changed;
           ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Copy || I.dst == NONE || I.a == NONE || I.a == I.dst)
          continue;
        if (I.a >= nv || uses[I.a] != 1) continue; // src read only here
        uint32_t d = def_idx[I.a];
        if (d == NONE || d == i || drop[d]) continue;
        Instr &D = fn.code[d];
        if (D.dst != I.a) continue;
        bool same_block =
            (d >= fn.blocks[b].begin && d < fn.blocks[b].end);
        if (D.k == K::Copy && D.a != NONE && D.a != I.dst && same_block) {
          // copy chain: d2 <- s <- t : this copy reads t directly, s dies.
          // t is single-def when it is a temp; when it is a phi slot its
          // other edge copies live in other blocks, so within THIS block a
          // write to t between D and I is the parallel-copy clobber that
          // cycle breaking exists to prevent (t<-a; a<-b; b<-t) — reject it.
          bool between_writes_src = false;
          for (uint32_t j = d + 1; j < i; ++j)
            if (fn.code[j].dst == D.a) {
              between_writes_src = true;
              break;
            }
          if (between_writes_src) continue;
          I.a = D.a;
          drop[d] = 1;
        } else if (same_block && D.k != K::Br && D.k != K::Cbr &&
                   D.k != K::Print && D.k != K::Exit && D.k != K::Store) {
          // rename the def's destination to the copy's dst and drop the copy.
          // SOUND ONLY within one block with no reader of dst in between:
          // the write moves earlier (from the copy up to the def), and phi
          // slots are read anywhere the header dominates — e.g. a loop body
          // between `t = i+1` and the back-edge copy `i = t` still reads the
          // OLD i. Same-block + no intermediate read of dst rules that out.
          bool between_reads_dst = false;
          for (uint32_t j = d + 1; j < i; ++j) {
            const Instr &M = fn.code[j];
            bool reads_dst =
                M.a == I.dst || M.b2 == I.dst || M.c2 == I.dst;
            for (uint32_t av : M.args) reads_dst |= (av == I.dst);
            if (reads_dst) {
              between_reads_dst = true;
              break;
            }
          }
          if (between_reads_dst) continue;
          D.dst = I.dst;
          drop[i] = 1;
        } else
          continue;
        ds.fused++;
        changed = true;
      }
    if (!changed) return;
    // sweep the one dropped instr, preserving block ranges
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size());
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      fn.blocks[b].begin = (uint32_t)nc.size();
      for (uint32_t i = ob; i < oe; ++i)
        if (!drop[i]) nc.push_back(fn.code[i]);
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
  }
}
struct RunOut {
  std::vector<val_t> out{};
  bool trapped{};
  bool timeout{};
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
    if ((size_t)h >= a_lenv.size()) a_lenv.resize((size_t)h + 1, 0);
    a_lenv[(size_t)h] = n;
    return {h, false};
  }
  constexpr val_t arena_len(val_t h) const {
    if (h < 0 || (size_t)h >= a_lenv.size()) return 0;
    return a_lenv[(size_t)h];
  }
  constexpr Rv arena_load(val_t h, val_t i) {
    val_t l = arena_len(h); // bogus handle -> len 0 -> always out of bounds
    if (i < 0 || i >= l) return {0, true};
    return {a_data[(size_t)(h + i)], false};
  }
  constexpr bool arena_store(val_t h, val_t i, val_t v) {
    val_t l = arena_len(h);
    if (i < 0 || i >= l) return false;
    a_data[(size_t)(h + i)] = v;
    return true;
  }
  constexpr val_t next_input() {
    if (in_pos < ins.size()) return ins[in_pos++];
    return 0; // EOF (matches scanf-failure -> 0 in the native backends)
  }
};
// call depth cap shared by EVERY runner (interpreters, VM, emitted C,
// and emitted asm all enforce the same limit: runaway recursion TRAPs
// everywhere)
inline constexpr uint32_t MJZ_MAX_CALL_DEPTH = 512;

// ============================================================================
// 9h. BYTECODE backend — a tiny stack VM.
//     [u32 slot_count][code...] ; ops: Push i64, Load/Store u32 slot,
//     Add Sub Mul Div Mod Lt Le Gt Ge Eq Ne, Jmp u32, Jz u32, Print, Halt,
//     ArrNew/ArrLd/ArrSt/ArrLen (arena, operands via the stack), In (stdin).
//     Div/Mod trap exactly like the interpreters. Runs the destroyed IR.
// ============================================================================
enum class Bc : uint8_t {
  Halt = 0, Push, Load, Store, Add, Sub, Mul, Div, Mod,
  Lt, Le, Gt, Ge, Eq, Ne, Jmp, Jz, Print,
  ArrNew, ArrLd, ArrSt, ArrLen, In,
  Call, Ret, // Call u32 fnidx u32 nargs; Ret (value via the operand stack)
  And, Or, Xor, Shl, Lshr, Asr, MulHi // optimizer-lowered bit ops
};
constexpr const char *bc_name(Bc op) {
  switch (op) {
  case Bc::Halt: return "halt";
  case Bc::Push: return "push";
  case Bc::Load: return "load";
  case Bc::Store: return "store";
  case Bc::Add: return "add";
  case Bc::Sub: return "sub";
  case Bc::Mul: return "mul";
  case Bc::Div: return "div";
  case Bc::Mod: return "mod";
  case Bc::Lt: return "lt";
  case Bc::Le: return "le";
  case Bc::Gt: return "gt";
  case Bc::Ge: return "ge";
  case Bc::Eq: return "eq";
  case Bc::Ne: return "ne";
  case Bc::Jmp: return "jmp";
  case Bc::Jz: return "jz";
  case Bc::Print: return "print";
  case Bc::ArrNew: return "arrnew";
  case Bc::ArrLd: return "arrld";
  case Bc::ArrSt: return "arrst";
  case Bc::ArrLen: return "arrlen";
  case Bc::In: return "in";
  case Bc::Call: return "call";
  case Bc::Ret: return "ret";
  case Bc::And: return "and";
  case Bc::Or: return "or";
  case Bc::Xor: return "xor";
  case Bc::Shl: return "shl";
  case Bc::Lshr: return "lshr";
  case Bc::Asr: return "asr";
  case Bc::MulHi: return "mulhi";
  }
  return "?";
}
constexpr void bc_put_u32(std::vector<uint8_t> &c, uint32_t v) {
  for (int i = 0; i < 4; ++i) { c.push_back((uint8_t)(v & 0xff)); v >>= 8; }
}
constexpr void bc_put_i64(std::vector<uint8_t> &c, int64_t v) {
  uint64_t u = (uint64_t)v;
  for (int i = 0; i < 8; ++i) { c.push_back((uint8_t)(u & 0xff)); u >>= 8; }
}
constexpr uint32_t bc_rd_u32(const std::vector<uint8_t> &c, size_t p) {
  return (uint32_t)c[p] | ((uint32_t)c[p + 1] << 8) |
         ((uint32_t)c[p + 2] << 16) | ((uint32_t)c[p + 3] << 24);
}
constexpr int64_t bc_rd_i64(const std::vector<uint8_t> &c, size_t p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i) u |= (uint64_t)c[p + (size_t)i] << (8 * i);
  return (int64_t)u;
}
constexpr Bc bc_of_bin(B b) {
  switch (b) {
  case B::Add: return Bc::Add;
  case B::Sub: return Bc::Sub;
  case B::Mul: return Bc::Mul;
  case B::Div: return Bc::Div;
  case B::Mod: return Bc::Mod;
  case B::Lt: return Bc::Lt;
  case B::Le: return Bc::Le;
  case B::Gt: return Bc::Gt;
  case B::Ge: return Bc::Ge;
  case B::Eq: return Bc::Eq;
  case B::Ne: return Bc::Ne;
  case B::And: return Bc::And;
  case B::Or: return Bc::Or;
  case B::Xor: return Bc::Xor;
  case B::Shl: return Bc::Shl;
  case B::LShr: return Bc::Lshr;
  case B::AShr: return Bc::Asr;
  case B::MulHi: return Bc::MulHi;
  }
  return Bc::Halt;
}
constexpr uint32_t bc_instr_size(const Instr &I) {
  switch (I.k) {
  case K::Const: return 9 + 5;              // Push + Store
  case K::Copy: return 5 + 5;               // Load + Store
  case K::Bin: return 5 + 5 + 1 + 5;        // Load,Load,op,Store
  case K::Br: return 5;                     // Jmp
  case K::Cbr: return 5 + 5 + 5;            // Load,Jz,Jmp
  case K::Print: return 5 + 1;              // Load,Print
  case K::Exit: return 1;                   // Halt
  case K::Phi: return 0;                    // gone (destroyed)
  case K::Alloc: return 5 + 1 + 5;          // Load,ArrNew,Store
  case K::Load: return 5 + 5 + 1 + 5;       // Load,Load,ArrLd,Store
  case K::Store: return 5 + 5 + 5 + 1;      // Load,Load,Load,ArrSt
  case K::Len: return 5 + 1 + 5;            // Load,ArrLen,Store
  case K::Input: return 1 + 5;              // In,Store
  case K::Call:                             // Load*argc,Call,Store
    return 1 + 4 + 4 + 5 * (uint32_t)I.args.size() + 5;
  case K::Ret: return (I.a == NONE ? 9 : 5) + 1; // (Push|Load),Ret
  }
  return 0;
}
// layout: [u32 nfns][nfns x (u32 code_off, u32 nslots)][code...]
// fns[0] is the main body; block jumps are absolute byte offsets.
constexpr std::vector<uint8_t> emit_bytecode(const std::vector<Fn> &fns) {
  const uint32_t nf = (uint32_t)fns.size();
  std::vector<std::vector<uint32_t>> off(nf);
  std::vector<uint32_t> fn_off(nf, 0);
  uint32_t cur = 4 + 8 * nf;
  for (uint32_t f = 0; f < nf; ++f) {
    const Fn &fn = fns[f];
    off[f].assign(fn.nblocks(), 0);
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      off[f][b] = cur;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
        cur += bc_instr_size(fn.code[i]);
    }
    fn_off[f] = fn.nblocks() ? off[f][0] : cur;
  }
  std::vector<uint8_t> c{};
  bc_put_u32(c, nf);
  for (uint32_t f = 0; f < nf; ++f) {
    bc_put_u32(c, fn_off[f]);
    bc_put_u32(c, slot_count(fns[f])); // VM slots (== vregs w/o regalloc)
  }
  for (uint32_t f = 0; f < nf; ++f) {
    const Fn &fn = fns[f];
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        switch (I.k) {
        case K::Phi:
          break;
        case K::Const:
          c.push_back((uint8_t)Bc::Push); bc_put_i64(c, I.c);
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Copy:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Bin:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.b2));
          c.push_back((uint8_t)bc_of_bin(I.b));
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Br:
          c.push_back((uint8_t)Bc::Jmp); bc_put_u32(c, off[f][I.t]);
          break;
        case K::Cbr:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Jz); bc_put_u32(c, off[f][I.f]);
          c.push_back((uint8_t)Bc::Jmp); bc_put_u32(c, off[f][I.t]);
          break;
        case K::Print:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Print);
          break;
        case K::Alloc:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::ArrNew);
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Load:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.b2));
          c.push_back((uint8_t)Bc::ArrLd);
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Store:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.b2));
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.c2));
          c.push_back((uint8_t)Bc::ArrSt);
          break;
        case K::Len:
          c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          c.push_back((uint8_t)Bc::ArrLen);
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Input:
          c.push_back((uint8_t)Bc::In);
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Call:
          for (uint32_t av : I.args) { // args pushed in parameter order
            c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], av));
          }
          c.push_back((uint8_t)Bc::Call);
          bc_put_u32(c, (uint32_t)I.c);
          bc_put_u32(c, (uint32_t)I.args.size());
          c.push_back((uint8_t)Bc::Store); bc_put_u32(c, slot_id(fns[f], I.dst));
          break;
        case K::Ret:
          if (I.a == NONE) {
            c.push_back((uint8_t)Bc::Push); bc_put_i64(c, 0);
          } else {
            c.push_back((uint8_t)Bc::Load); bc_put_u32(c, slot_id(fns[f], I.a));
          }
          c.push_back((uint8_t)Bc::Ret);
          break;
        case K::Exit:
          c.push_back((uint8_t)Bc::Halt);
          break;
        }
      }
    }
  }
  return c;
}

constexpr RunOut vm_run(const std::vector<uint8_t> &c,
                        uint64_t step_limit = 200000,
                        const std::vector<val_t> &ins = {}) {
  RunOut R{};
  R.ins = ins;
  if (c.size() < 4) { R.trapped = true; return R; }
  const uint32_t nfns = bc_rd_u32(c, 0);
  if (!nfns || c.size() < 4 + (size_t)8 * nfns) {
    R.trapped = true;
    return R;
  }
  auto fn_off = [&](uint32_t f) -> uint32_t {
    return bc_rd_u32(c, 4 + (size_t)8 * f);
  };
  auto fn_slots = [&](uint32_t f) -> uint32_t {
    return bc_rd_u32(c, 4 + (size_t)8 * f + 4);
  };
  std::vector<std::vector<val_t>> frames; // one slot vector per activation
  frames.push_back(std::vector<val_t>(fn_slots(0), 0));
  std::vector<uint32_t> rets;             // return pcs of suspended callers
  std::vector<val_t> st{};
  size_t pc = fn_off(0);
  // "steps" count IR-equivalent instructions (Store/Jmp/Jz/Print/Halt), not
  // sub-ops, so one step limit means the same thing for interpreter and VM.
  uint64_t steps = 0;
  for (;;) {
    if (pc >= c.size()) { R.trapped = true; return R; }
    Bc op = (Bc)c[pc++];
    switch (op) {
    case Bc::Halt:
      if (++steps > step_limit) { R.timeout = true; return R; }
      return R;
    case Bc::Push:
      st.push_back(bc_rd_i64(c, pc)); pc += 8; break;
    case Bc::Load:
      st.push_back(frames.back()[bc_rd_u32(c, pc)]); pc += 4; break;
    case Bc::Store: {
      if (++steps > step_limit) { R.timeout = true; return R; }
      val_t v = st.back(); st.pop_back();
      frames.back()[bc_rd_u32(c, pc)] = v; pc += 4; break;
    }
    case Bc::Call: {
      if (++steps > step_limit) { R.timeout = true; return R; }
      uint32_t g = bc_rd_u32(c, pc); pc += 4;
      uint32_t na = bc_rd_u32(c, pc); pc += 4;
      if (g >= nfns || frames.size() >= MJZ_MAX_CALL_DEPTH) {
        R.trapped = true; // bad callee / recursion depth cap
        return R;
      }
      std::vector<val_t> args(na, 0); // pop na args (a0 pushed first)
      for (uint32_t k = na; k-- > 0;) {
        if (st.empty()) { R.trapped = true; return R; }
        args[k] = st.back();
        st.pop_back();
      }
      rets.push_back((uint32_t)pc);
      std::vector<val_t> fr(fn_slots(g), 0);
      for (uint32_t k = 0; k < na && k < fr.size(); ++k) fr[k] = args[k];
      frames.push_back(std::move(fr));
      pc = fn_off(g);
      break;
    }
    case Bc::Ret: {
      if (++steps > step_limit) { R.timeout = true; return R; }
      if (rets.empty() || st.empty() || frames.size() < 2) {
        R.trapped = true;
        return R;
      }
      val_t v = st.back();
      st.pop_back();
      frames.pop_back();
      pc = rets.back();
      rets.pop_back();
      st.push_back(v); // the caller's Store pops it into the result slot
      break;
    }
    case Bc::Add: case Bc::Sub: case Bc::Mul: case Bc::Div: case Bc::Mod:
    case Bc::Lt: case Bc::Le: case Bc::Gt: case Bc::Ge: case Bc::Eq:
    case Bc::Ne:
    case Bc::And: case Bc::Or: case Bc::Xor: case Bc::Shl: case Bc::Lshr:
    case Bc::Asr: case Bc::MulHi: {
      if (st.size() < 2) { R.trapped = true; return R; }
      val_t y = st.back(); st.pop_back();
      val_t x = st.back(); st.pop_back();
      Rv v{};
      switch (op) {
      case Bc::Add: v = w_add(x, y); break;
      case Bc::Sub: v = w_sub(x, y); break;
      case Bc::Mul: v = w_mul(x, y); break;
      case Bc::Div: v = w_div(x, y); break;
      case Bc::Mod: v = w_mod(x, y); break;
      case Bc::Lt: v = {x < y, false}; break;
      case Bc::Le: v = {x <= y, false}; break;
      case Bc::Gt: v = {x > y, false}; break;
      case Bc::Ge: v = {x >= y, false}; break;
      case Bc::Eq: v = {x == y, false}; break;
      case Bc::Ne: v = {x != y, false}; break;
      case Bc::And: v = w_and(x, y); break;
      case Bc::Or: v = w_or(x, y); break;
      case Bc::Xor: v = w_xor(x, y); break;
      case Bc::Shl: v = w_shl(x, y); break;
      case Bc::Lshr: v = w_lshr(x, y); break;
      case Bc::Asr: v = w_ashr(x, y); break;
      case Bc::MulHi: v = w_mulhi(x, y); break;
      default: break;
      }
      if (v.trap) { R.trapped = true; return R; }
      st.push_back(v.v);
      break;
    }
    case Bc::Jmp:
      if (++steps > step_limit) { R.timeout = true; return R; }
      pc = bc_rd_u32(c, pc); break;
    case Bc::Jz: {
      if (++steps > step_limit) { R.timeout = true; return R; }
      val_t v = st.back(); st.pop_back();
      uint32_t t = bc_rd_u32(c, pc); pc += 4;
      if (v == 0) pc = t;
      break;
    }
    case Bc::Print: {
      if (++steps > step_limit) { R.timeout = true; return R; }
      val_t v = st.back(); st.pop_back();
      R.out.push_back(v);
      break;
    }
    case Bc::ArrNew: { // pop n -> push fresh handle
      val_t n = st.back(); st.pop_back();
      Rv r = R.arena_alloc(n);
      if (r.trap) { R.trapped = true; return R; }
      st.push_back(r.v);
      break;
    }
    case Bc::ArrLd: { // pop i, pop h -> push h[i]
      if (st.size() < 2) { R.trapped = true; return R; }
      val_t i = st.back(); st.pop_back();
      val_t h = st.back(); st.pop_back();
      Rv r = R.arena_load(h, i);
      if (r.trap) { R.trapped = true; return R; }
      st.push_back(r.v);
      break;
    }
    case Bc::ArrSt: { // pop v, pop i, pop h -> h[i] = v   (1 IR step)
      if (++steps > step_limit) { R.timeout = true; return R; }
      if (st.size() < 3) { R.trapped = true; return R; }
      val_t v = st.back(); st.pop_back();
      val_t i = st.back(); st.pop_back();
      val_t h = st.back(); st.pop_back();
      if (!R.arena_store(h, i, v)) { R.trapped = true; return R; }
      break;
    }
    case Bc::ArrLen: { // pop h -> push len(h)
      val_t h = st.back(); st.pop_back();
      st.push_back(R.arena_len(h));
      break;
    }
    case Bc::In: // push next stdin value (0 at EOF)
      st.push_back(R.next_input());
      break;
    }
  }
}
// ============================================================================
// 10. SSA interpreter (phi resolution by pred identity via t/f)
// ============================================================================
// program-level SSA interpreter with an explicit frame stack: Call pushes a
// frame (params bound from the operand-side argument vregs), Ret pops back
// to the caller's resume point and stores the value into the result slot.
constexpr RunOut run_prog(const std::vector<Fn> &fns,
                          uint64_t step_limit = 200000,
                          const std::vector<val_t> &ins = {}) {
  RunOut R{};
  R.ins = ins;
  if (fns.empty() || fns[0].nblocks() == 0) { R.trapped = true; return R; }
  struct Fr {
    uint32_t fn;
    std::vector<val_t> val;
    std::vector<char> has;
  };
  struct Resume {
    uint32_t fn, blk, i, dst;
  };
  std::vector<Fr> stack;
  std::vector<Resume> resumes;
  {
    uint32_t nv0 = fns[0].nvars + (uint32_t)fns[0].version_base.size();
    stack.push_back({0, std::vector<val_t>(nv0, 0),
                     std::vector<char>(nv0, 0)});
  }
  uint64_t steps = 0;
  uint32_t cur_fn = 0, cur_blk = 0;
  int64_t i = (int64_t)fns[0].blocks[0].begin;
  for (;;) {
    if (++steps > step_limit) { R.timeout = true; return R; }
    if (cur_fn >= fns.size()) { R.trapped = true; return R; }
    const Fn &F = fns[cur_fn];
    if (cur_blk >= F.nblocks() ||
        i < (int64_t)F.blocks[cur_blk].begin ||
        i >= (int64_t)F.blocks[cur_blk].end) {
      R.trapped = true;
      return R;
    }
    const Instr &I = F.code[i];
    switch (I.k) {
    case K::Phi: // resolved on the edge (staged before jumping)
      ++i;
      continue;
    case K::Br:
    case K::Cbr: {
      uint32_t next = (I.k == K::Br)
                          ? I.t
                          : ((stack.back().val[I.a] != 0) ? I.t : I.f);
      if (next >= F.nblocks()) { R.trapped = true; return R; }
      // stage phi values of `next` for this edge: read all, then write all
      std::vector<std::pair<uint32_t, val_t>> staged{};
      for (uint32_t j = F.blocks[next].begin; j < F.blocks[next].end; ++j) {
        const Instr &P = F.code[j];
        if (P.k != K::Phi) break;
        uint32_t arg = NONE;
        if (P.t == cur_blk && P.a != NONE) arg = P.a;
        else if (P.f == cur_blk && P.b2 != NONE) arg = P.b2;
        if (arg != NONE)
          staged.push_back({P.dst, stack.back().val[arg]});
      }
      for (auto [d, v] : staged) {
        stack.back().val[d] = v;
        stack.back().has[d] = 1;
      }
      cur_blk = next;
      i = (int64_t)F.blocks[cur_blk].begin;
      continue;
    }
    case K::Exit:
      return R;
    case K::Ret: {
      val_t rv = (I.a == NONE) ? 0 : stack.back().val[I.a];
      if (resumes.empty() || stack.size() < 2) {
        R.trapped = true; // return with no caller (main has no Ret)
        return R;
      }
      Resume rs = resumes.back();
      resumes.pop_back();
      stack.pop_back();
      cur_fn = rs.fn;
      cur_blk = rs.blk;
      i = (int64_t)rs.i;
      if (rs.dst != NONE) {
        stack.back().val[rs.dst] = rv;
        stack.back().has[rs.dst] = 1;
      }
      continue;
    }
    case K::Call: {
      if (stack.size() >= MJZ_MAX_CALL_DEPTH) {
        R.trapped = true; // recursion depth cap (matches native guards)
        return R;
      }
      uint32_t g = (uint32_t)I.c;
      if (g >= fns.size() || I.args.size() != fns[g].nparams) {
        R.trapped = true;
        return R;
      }
      resumes.push_back({cur_fn, cur_blk, (uint32_t)(i + 1), I.dst});
      uint32_t nvg = fns[g].nvars + (uint32_t)fns[g].version_base.size();
      std::vector<val_t> nv(nvg, 0);
      std::vector<char> nh(nvg, 0);
      for (uint32_t p = 0; p < I.args.size(); ++p) {
        nv[p] = stack.back().val[I.args[p]];
        nh[p] = 1;
      }
      stack.push_back({g, std::move(nv), std::move(nh)});
      cur_fn = g;
      cur_blk = 0;
      i = (int64_t)fns[g].blocks[0].begin;
      continue;
    }
    case K::Const:
      if (I.dst >= stack.back().val.size())
        std::fprintf(stderr, "OOB const: fn=%u blk=%u dst=%u valsize=%zu nfns=%zu\n",
                     cur_fn, cur_blk, I.dst, stack.back().val.size(), fns.size());
      stack.back().val[I.dst] = I.c;
      stack.back().has[I.dst] = 1;
      break;
    case K::Copy:
      stack.back().val[I.dst] = stack.back().val[I.a];
      stack.back().has[I.dst] = 1;
      break;
    case K::Bin: {
      if (I.a >= stack.back().val.size() || I.b2 >= stack.back().val.size())
        std::fprintf(stderr,
                     "OOB bin: fn=%u blk=%u a=%u b2=%u valsize=%zu nvars=%u\n",
                     cur_fn, cur_blk, I.a, I.b2, stack.back().val.size(),
                     fns[cur_fn].nvars);
      val_t x = stack.back().val[I.a], y = stack.back().val[I.b2];
      Rv r{};
      switch (I.b) {
      case B::Add: r = w_add(x, y); break;
      case B::Sub: r = w_sub(x, y); break;
      case B::Mul: r = w_mul(x, y); break;
      case B::Div: r = w_div(x, y); break;
      case B::Mod: r = w_mod(x, y); break;
      case B::Lt: r = {x < y, false}; break;
      case B::Le: r = {x <= y, false}; break;
      case B::Gt: r = {x > y, false}; break;
      case B::Ge: r = {x >= y, false}; break;
      case B::Eq: r = {x == y, false}; break;
      case B::Ne: r = {x != y, false}; break;
      case B::And: r = w_and(x, y); break;
      case B::Or: r = w_or(x, y); break;
      case B::Xor: r = w_xor(x, y); break;
      case B::Shl: r = w_shl(x, y); break;
      case B::LShr: r = w_lshr(x, y); break;
      case B::AShr: r = w_ashr(x, y); break;
      case B::MulHi: r = w_mulhi(x, y); break;
      }
      if (r.trap) { R.trapped = true; return R; }
      stack.back().val[I.dst] = r.v;
      stack.back().has[I.dst] = 1;
      break;
    }
    case K::Print:
      R.out.push_back(stack.back().val[I.a]);
      break;
    case K::Alloc: {
      Rv r = R.arena_alloc(stack.back().val[I.a]);
      if (r.trap) { R.trapped = true; return R; }
      stack.back().val[I.dst] = r.v;
      stack.back().has[I.dst] = 1;
      break;
    }
    case K::Load: {
      Rv r = R.arena_load(stack.back().val[I.a], stack.back().val[I.b2]);
      if (r.trap) { R.trapped = true; return R; }
      stack.back().val[I.dst] = r.v;
      stack.back().has[I.dst] = 1;
      break;
    }
    case K::Store:
      if (!R.arena_store(stack.back().val[I.a], stack.back().val[I.b2],
                         stack.back().val[I.c2])) {
        R.trapped = true;
        return R;
      }
      break;
    case K::Len:
      stack.back().val[I.dst] = R.arena_len(stack.back().val[I.a]);
      stack.back().has[I.dst] = 1;
      break;
    case K::Input:
      stack.back().val[I.dst] = R.next_input();
      stack.back().has[I.dst] = 1;
      break;
    }
    ++i;
  }
}

// ============================================================================
// 11. AST interpreter (the oracle)
// ============================================================================
struct AstRun {
  const Ast &a;
  RunOut R{};
  uint64_t steps{};
  uint64_t limit{};
  bool dead{};      // set on trap/timeout to short-circuit
  bool returned{};  // a 'return' completed: unwind this frame
  bool broke{};     // a 'break' completed: unwind to the enclosing loop
  bool continued{}; // a 'continue' completed: jump to the loop's re-test
  val_t retval{};   // ... with this value
  uint32_t depth{}; // call depth (capped: matches every other runner)

  constexpr AstRun(const Ast &a_, uint64_t lim = 200000)
      : a(a_), limit(lim) {}

  constexpr bool eval(uint32_t ei, val_t &out) {
    if (dead || ++steps > limit) { R.timeout = true; dead = true; out = 0; return false; }
    const Expr &e = a.es[ei];
    switch (e.k) {
    case Ek::Num: out = e.num; return true;
    case Ek::Var: out = env[e.var]; return true;
    case Ek::Neg: {
      val_t x{};
      if (!eval(e.l, x)) return false;
      out = w_sub(0, x).v;
      return true;
    }
    case Ek::Not: {
      val_t x{};
      if (!eval(e.l, x)) return false;
      out = (x == 0) ? 1 : 0;
      return true;
    }
    case Ek::Bin: {
      val_t x{}, y{};
      if (!eval(e.l, x)) return false;
      if (!eval(e.r, y)) return false;
      Rv r{};
      switch (e.b) {
      case Bk::Add: r = w_add(x, y); break;
      case Bk::Sub: r = w_sub(x, y); break;
      case Bk::Mul: r = w_mul(x, y); break;
      case Bk::Div: r = w_div(x, y); break;
      case Bk::Mod: r = w_mod(x, y); break;
      case Bk::Lt: r = {x < y, false}; break;
      case Bk::Le: r = {x <= y, false}; break;
      case Bk::Gt: r = {x > y, false}; break;
      case Bk::Ge: r = {x >= y, false}; break;
      case Bk::Eq: r = {x == y, false}; break;
      case Bk::Ne: r = {x != y, false}; break;
      }
      if (r.trap) { R.trapped = true; dead = true; out = 0; return false; }
      out = r.v;
      return true;
    }
    case Ek::And: {
      val_t x{};
      if (!eval(e.l, x)) return false;
      if (x == 0) { out = 0; return true; }   // short-circuit
      val_t y{};
      if (!eval(e.r, y)) return false;
      out = (y != 0) ? 1 : 0;
      return true;
    }
    case Ek::Or: {
      val_t x{};
      if (!eval(e.l, x)) return false;
      if (x != 0) { out = 1; return true; }   // short-circuit
      val_t y{};
      if (!eval(e.r, y)) return false;
      out = (y != 0) ? 1 : 0;
      return true;
    }
    case Ek::Alloc: {
      val_t n{};
      if (!eval(e.l, n)) return false;
      Rv r = R.arena_alloc(n);
      if (r.trap) { R.trapped = true; dead = true; out = 0; return false; }
      out = r.v;
      return true;
    }
    case Ek::Load: {
      val_t h{}, i{};
      if (!eval(e.l, h)) return false;
      if (!eval(e.r, i)) return false;
      Rv r = R.arena_load(h, i);
      if (r.trap) { R.trapped = true; dead = true; out = 0; return false; }
      out = r.v;
      return true;
    }
    case Ek::Len: {
      val_t h{};
      if (!eval(e.l, h)) return false;
      out = R.arena_len(h);
      return true;
    }
    case Ek::Input:
      out = R.next_input();
      return true;
    case Ek::Call: { // args left to right, then a fresh frame for the body
      const FnDef &fd = a.fns[e.var];
      std::vector<val_t> argv{};
      for (uint32_t ai : a.seq_kids[e.l]) {
        val_t v{};
        if (!eval(ai, v)) return false;
        argv.push_back(v);
      }
      if (depth >= MJZ_MAX_CALL_DEPTH) {
        R.trapped = true;
        dead = true;
        out = 0;
        return false;
      }
      std::vector<val_t> saved = std::move(env);
      env.assign(a.names.size(), 0);
      for (size_t p = 0; p < fd.params.size() && p < argv.size(); ++p)
        env[fd.params[p]] = argv[p];
      ++depth;
      bool was = returned;
      returned = false;
      exec(fd.body);
      --depth;
      val_t rv = retval;
      env = std::move(saved);
      returned = was;
      if (dead) {
        out = 0;
        return false;
      }
      out = rv;
      return true;
    }
    }
    out = 0;
    return false;
  }
  std::vector<val_t> env{};
  constexpr bool exec(uint32_t si) {
    if (dead) return false;
    if (++steps > limit) { R.timeout = true; dead = true; return false; }
    const Stmt &s = a.ss[si];
    switch (s.k) {
    case Sk::Let:
    case Sk::Assign: {
      val_t v{};
      if (!eval(s.e, v)) return false;
      env[s.var] = v;
      return true;
    }
    case Sk::Print: {
      val_t v{};
      if (!eval(s.e, v)) return false;
      R.out.push_back(v);
      return true;
    }
    case Sk::Ret: {
      val_t v{};
      if (!eval(s.e, v)) return false;
      retval = v;
      returned = true;
      return true; // frame finished; callers see `returned` and unwind
    }
    case Sk::Seq:
      for (uint32_t x : a.seq_kids[s.a]) {
        if (!exec(x)) return false;
        if (returned || broke || continued) return true;
      }
      return true;
    case Sk::StoreA: { // a[i1]...[ik] = value (indices, then loads, then value)
      const auto &idx = a.seq_kids[s.a];
      val_t h = env[s.var];
      for (size_t j = 0; j + 1 < idx.size(); ++j) {
        val_t i{};
        if (!eval(idx[j], i)) return false;
        Rv r = R.arena_load(h, i);
        if (r.trap) { R.trapped = true; dead = true; return false; }
        h = r.v;
      }
      val_t last{};
      if (!eval(idx.back(), last)) return false;
      val_t v{};
      if (!eval(s.b, v)) return false;
      if (!R.arena_store(h, last, v)) {
        R.trapped = true;
        dead = true;
        return false;
      }
      return true;
    }
    case Sk::If: {
      val_t c{};
      if (!eval(s.e, c)) return false;
      return exec(c != 0 ? s.a : s.b);
    }
    case Sk::Brk:
      broke = true;
      return true; // the enclosing While consumes the flag
    case Sk::Cont:
      continued = true;
      return true;
    case Sk::While: {
      for (;;) {
        val_t c{};
        if (!eval(s.e, c)) return false;
        if (c == 0) return true;
        if (!exec(s.a)) return false;
        if (returned) return true;
        if (broke) { broke = false; return true; }
        if (continued) { continued = false; /* re-test */ }
      }
    }
    }
    return false;
  }
};
constexpr RunOut run_ast(const Ast &a, uint32_t root, uint64_t limit = 200000,
                         const std::vector<val_t> &ins = {}) {
  AstRun ar(a, limit);
  ar.R.ins = ins;
  ar.env.assign(a.names.size(), 0);
  ar.exec(root);
  return ar.R;
}

// ============================================================================
// 11z. SLOT ALLOCATION — "register allocation" for the C + bytecode backends
// ============================================================================
// After SSA destruction every vreg behaves like a stack slot. This pass
// colors the interference graph so vregs with DISJOINT live ranges share one
// home: the C backend declares fewer locals (r0..r{K-1} instead of one
// variable per vreg) and the bytecode VM frames shrink (fewer slots per
// frame). Unlike the x86-64 backend there is NO spill machinery and no fixed
// register set: colors are unbounded (C has unlimited locals, the VM has
// unlimited slots), so greedy coloring is always feasible.
//   * liveness: per-block backward dataflow to a fixpoint (same discipline
//     as the x86 ra_plan, but on plain instructions — deliberately ignorant
//     of the emission plan's const-inlining / branch fusion, which only
//     makes the interference estimate conservative).
//   * interference: at each def, edges to everything live-after it; params
//     are "defined at entry", so they conflict with each other and with
//     everything live into the entry block.
//   * coloring: parameters are PINNED to slots 0..nparams-1 (the VM's Call
//     convention writes caller args into exactly these slots); every other
//     vreg takes the smallest color unused by its already-colored neighbors,
//     highest degree first (Chaitin-style heuristic -> few slots).
// With --no-regalloc (PassCfg::regalloc == false) this never runs and the
// backends emit the identity mapping — one named local per vreg ("SSA-like
// C", one VM slot per vreg).
constexpr void assign_slots(Fn &fn) {
  fn.slot_of.clear();
  fn.nslots = 0;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nb || !nv) return;
  auto each_use = [&](const Instr &I, auto &&f) {
    auto one = [&](uint32_t v) {
      if (v != NONE && v < nv) f(v);
    };
    one(I.a);
    one(I.b2);
    one(I.c2);
    for (uint32_t v : I.args) one(v);
  };
  auto is_def = [&](const Instr &I) -> uint32_t {
    if (I.dst == NONE || I.dst >= nv) return NONE;
    return I.dst;
  };
  // ---- liveness: block-granular backward dataflow to a fixpoint ----------
  std::vector<std::vector<char>> lin(nb, std::vector<char>(nv, 0)),
      lout(nb, std::vector<char>(nv, 0));
  for (uint32_t round = 0; round < 64; ++round) {
    bool changed = false;
    for (uint32_t bb = nb; bb-- > 0;) {
      std::vector<char> &lo = lout[bb];
      std::fill(lo.begin(), lo.end(), 0);
      for (uint32_t s : {fn.succ0(bb), fn.succ1(bb)})
        if (s != NONE && s < nb)
          for (uint32_t v = 0; v < nv; ++v) lo[v] |= lin[s][v];
      std::vector<char> live = lo;
      for (uint32_t i = fn.blocks[bb].end; i-- > fn.blocks[bb].begin;) {
        const Instr &I = fn.code[i];
        if (I.k == K::Phi) continue; // destroyed input: none (defensive)
        uint32_t d = is_def(I);
        if (d != NONE) live[d] = 0;
        each_use(I, [&](uint32_t v) { live[v] = 1; });
      }
      for (uint32_t v = 0; v < nv; ++v)
        if (live[v] != lin[bb][v]) {
          lin[bb][v] = live[v];
          changed = true;
        }
    }
    if (!changed) break;
  }
  // ---- interference edges -------------------------------------------------
  std::vector<std::vector<uint32_t>> adj(nv);
  auto edge = [&](uint32_t a, uint32_t b) {
    if (a == b || a >= nv || b >= nv) return;
    for (uint32_t x : adj[a])
      if (x == b) return;
    adj[a].push_back(b);
    adj[b].push_back(a);
  };
  { // parameters are defined at entry: conflict with each other and with
    // everything live into the entry block
    std::vector<char> live = lin[0];
    for (uint32_t p = 0; p < fn.nparams && p < nv; ++p) {
      for (uint32_t u = 0; u < nv; ++u)
        if (live[u] && u != p) edge(p, u);
      live[p] = 0;
    }
  }
  for (uint32_t b = 0; b < nb; ++b) {
    std::vector<char> live = lout[b];
    for (uint32_t i = fn.blocks[b].end; i-- > fn.blocks[b].begin;) {
      const Instr &I = fn.code[i];
      if (I.k == K::Phi) continue;
      uint32_t d = is_def(I);
      if (d != NONE) {
        for (uint32_t u = 0; u < nv; ++u)
          if (live[u]) edge(d, u); // the write clobbers: no sharing here
        live[d] = 0;
      }
      each_use(I, [&](uint32_t v) { live[v] = 1; });
    }
  }
  // ---- coloring: params pinned, then greedy, highest degree first --------
  std::vector<int32_t> color(nv, -1);
  for (uint32_t p = 0; p < fn.nparams && p < nv; ++p) color[p] = (int32_t)p;
  std::vector<uint32_t> order{};
  order.reserve(nv);
  for (uint32_t v = 0; v < nv; ++v) order.push_back(v);
  // insertion sort by descending degree (stable, constexpr, nv is modest)
  for (size_t i = 1; i < order.size(); ++i)
    for (size_t j = i; j > 0 && adj[order[j - 1]].size() < adj[order[j]].size();
         --j)
      std::swap(order[j - 1], order[j]);
  std::vector<char> taken(nv, 0);
  for (uint32_t v : order) {
    if (color[v] >= 0) continue; // pinned param
    std::fill(taken.begin(), taken.end(), 0);
    for (uint32_t u : adj[v])
      if (color[u] >= 0 && (uint32_t)color[u] < nv) taken[color[u]] = 1;
    int32_t c = 0;
    while (c < (int32_t)nv && taken[c]) ++c;
    color[v] = c;
  }
  uint32_t maxc = 0;
  for (uint32_t v = 0; v < nv; ++v)
    if (color[v] > (int32_t)maxc) maxc = (uint32_t)color[v];
  fn.slot_of.assign(nv, 0);
  for (uint32_t v = 0; v < nv; ++v) fn.slot_of[v] = (uint32_t)color[v];
  fn.nslots = maxc + 1;
  if (fn.nslots < fn.nparams) fn.nslots = fn.nparams; // params are pinned
}

// ============================================================================
// 12. full pipeline driver
// ============================================================================
struct FnUnit { // one function's journey through the pipeline
  Fn ssa{};        // SSA before optimization
  Fn opt{};        // SSA after optimization
  Fn destroyed{};  // after SSA destruction (no phis; slots only)
  SsaStats sstats{};
  OptStats ostats{};
  DestrStats dstats{};
};
struct Program {
  Parsed P{};
  std::vector<Fn> ssa_all{}; // pre-opt SSA of EVERY fn (original indices;
                             // the unoptimized runners need the full set even
                             // after the optimizer dropped dead functions)
  std::vector<FnUnit> fns{};                // [0] = main body, then fns
  std::vector<std::string_view> fn_names{}; // parallel source names
  An an_pre_main{};                         // main's unoptimized SSA CFG
  bool uses_input{}; // the program contains input()
  bool ok{};
  const char *error{};
  uint32_t errpos{};
};

constexpr Program compile(std::string_view src, PassCfg cfg = {}) {
  Program P{};
  P.P = parse(src);
  if (!P.P.ok) { P.error = P.P.err.what; P.errpos = P.P.err.pos; return P; }
  const Ast &a = P.P.ast;
  // call arity must match the definitions (checked post-parse: definitions
  // may appear anywhere in the file)
  for (const Expr &e : a.es)
    if (e.k == Ek::Call) {
      const FnDef &fd = a.fns[e.var];
      if (fd.params.size() != a.seq_kids[e.l].size()) {
        P.error = "wrong number of arguments in call";
        P.errpos = 0;
        return P;
      }
    }
  const uint32_t nf = 1 + (uint32_t)a.fns.size();
  std::vector<Fn> ssa0(nf);
  std::vector<SsaStats> sst(nf);
  {
    Builder B(ssa0[0]);
    B.build_main(a, P.P.root);
    if (ssa0[0].nblocks() == 0) { P.error = "empty program"; return P; }
    An A = analyze(ssa0[0]);
    if (!ssa_construct(ssa0[0], A, sst[0])) {
      P.error = "SSA construction failed (use before def?)";
      return P;
    }
    P.an_pre_main = A;
  }
  for (uint32_t k = 0; k < a.fns.size(); ++k) {
    Builder B(ssa0[k + 1]);
    B.build_fn(a, a.fns[k]);
    An A = analyze(ssa0[k + 1]);
    if (!ssa_construct(ssa0[k + 1], A, sst[k + 1])) {
      P.error = "SSA construction failed (use before def?)";
      return P;
    }
  }
  // whole-program optimization: per-fn fixpoints, callgraph SCC inlining,
  // dead-function elimination, final pass (unrolling included).
  // The program-wide handle gates (array promotion / C stack arrays) are
  // computed on the PRE-optimization IR of every function.
  std::vector<Fn> work = ssa0;
  std::vector<std::string_view> names(nf);
  names[0] = "<main>";
  for (uint32_t k = 0; k < a.fns.size(); ++k) names[k + 1] = a.fns[k].name;
  std::vector<OptStats> os(nf);
  std::vector<uint32_t> src_index(nf);
  for (uint32_t k = 0; k < nf; ++k) src_index[k] = k;
  prog_handle_gates(ssa0, cfg.lift_ok, cfg.stkarr_ok);
  if (cfg.do_opt)
    optimize_prog(work, names, os, cfg, src_index);
  // assemble the per-function units (dropped fns realigned via src_index)
  P.ssa_all = ssa0;
  P.fns.resize(work.size());
  P.fn_names = names;
  for (uint32_t j = 0; j < work.size(); ++j) {
    FnUnit &U = P.fns[j];
    uint32_t src = src_index[j];
    U.ssa = ssa0[src];
    U.sstats = sst[src];
    if (cfg.stkarr_ok)
      analyze_stack_allocs(work[j]); // stash local arrays before both copies
    else
      work[j].stk_arrays.clear();
    U.opt = work[j];
    U.ostats = os[j];
    U.destroyed = work[j]; // inherits .stk_arrays (vreg ids are stable)
    destroy_ssa(U.destroyed, U.dstats);
    post_destruct_peephole(U.destroyed, U.dstats);
    if (cfg.regalloc) // C + bytecode: minimize the variable/slot count
      assign_slots(U.destroyed);
  }
  P.uses_input = P.P.uses_input;
  P.ok = true;
  return P;
}

// assemble one IR view (0 = ssa, 1 = opt, 2 = destroyed) of a program
constexpr std::vector<Fn> fns_of(const Program &P, int which) {
  if (which == 0) return P.ssa_all; // unoptimized: ALL functions, orig indices
  std::vector<Fn> out{};
  out.reserve(P.fns.size());
  for (const FnUnit &U : P.fns)
    out.push_back(which == 1 ? U.opt : U.destroyed);
  return out;
}
// summed optimization stats across all functions (iters: max)
static OptStats sum_ostats(const Program &P) {
  OptStats t{};
  for (const FnUnit &U : P.fns) {
    t.folds += U.ostats.folds;
    t.copies += U.ostats.copies;
    t.brs_folded += U.ostats.brs_folded;
    t.blocks_gone += U.ostats.blocks_gone;
    t.dce += U.ostats.dce;
    t.hoisted += U.ostats.hoisted;
    t.copyprops += U.ostats.copyprops;
    t.cses += U.ostats.cses;
    t.splices += U.ostats.splices;
    t.cbr_same += U.ostats.cbr_same;
    t.phi_degens += U.ostats.phi_degens;
    t.canon_swaps += U.ostats.canon_swaps;
    t.canon_cmps += U.ostats.canon_cmps;
    t.inlines += U.ostats.inlines;
    t.unrolls += U.ostats.unrolls;
    t.reassocs += U.ostats.reassocs;
    t.reassoc_gone += U.ostats.reassoc_gone;
    t.lcf_loops += U.ostats.lcf_loops;
    t.lcf_sims += U.ostats.lcf_sims;
    t.lcf_gensims += U.ostats.lcf_gensims;
    t.lcf_rt += U.ostats.lcf_rt;
    t.divmagics += U.ostats.divmagics;
    t.notcombs += U.ostats.notcombs;
    t.traps_killed += U.ostats.traps_killed;
    t.mem2reg_lifts += U.ostats.mem2reg_lifts;
    t.mem2reg_dses += U.ostats.mem2reg_dses;
    if (U.ostats.iters > t.iters) t.iters = U.ostats.iters;
  }
  return t;
}

// ============================================================================
// 13. pretty-printers (runtime only)
// ============================================================================
static std::string vname(const Fn &fn, uint32_t v) {
  if (v == NONE) return "_";
  char buf[64];
  uint32_t base = fn.base_of(v), ver = fn.vern_of(v);
  std::string n;
  if (base < fn.var_names.size() && !fn.var_names[base].empty()) {
    // named source variable: versions are per-base numbered -> unique
    n.assign(fn.var_names[base]);
    std::snprintf(buf, sizeof buf, "%s.%u", n.c_str(), (unsigned)ver);
  } else {
    // anonymous temp: raw vreg id (many temp bases share version numbers)
    std::snprintf(buf, sizeof buf, "t%u", (unsigned)v);
  }
  return std::string(buf);
}

static void print_fn(const Fn &fn, const char *title) {
  std::printf("  ---- %s ----\n", title);
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    if (fn.blocks[b].begin == fn.blocks[b].end &&
        fn.succ0(b) == NONE && fn.succ1(b) == NONE && b != 0)
      continue; // isolated dead block
    std::printf("  b%u:", b);
    if (!fn.preds.empty() && !fn.preds[b].empty()) {
      std::printf("              ; preds:");
      for (uint32_t p : fn.preds[b]) std::printf(" b%u", p);
    }
    std::printf("\n");
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      std::printf("    ");
      switch (I.k) {
      case K::Const:
        std::printf("%s = %lld", vname(fn, I.dst).c_str(),
                    (long long)I.c);
        break;
      case K::Copy:
        std::printf("%s = %s", vname(fn, I.dst).c_str(),
                    vname(fn, I.a).c_str());
        break;
      case K::Bin:
        std::printf("%s = %s %s %s", vname(fn, I.dst).c_str(),
                    vname(fn, I.a).c_str(), bname(I.b),
                    vname(fn, I.b2).c_str());
        break;
      case K::Br:
        std::printf("br b%u", I.t);
        break;
      case K::Cbr:
        std::printf("cbr %s ? b%u : b%u", vname(fn, I.a).c_str(), I.t, I.f);
        break;
      case K::Print:
        std::printf("print %s", vname(fn, I.a).c_str());
        break;
      case K::Phi: {
        std::printf("%s = phi ", vname(fn, I.dst).c_str());
        if (I.a != NONE)
          std::printf("[%s from b%u]", vname(fn, I.a).c_str(), I.t);
        if (I.b2 != NONE)
          std::printf("%s[%s from b%u]", I.a != NONE ? " " : "",
                      vname(fn, I.b2).c_str(), I.f);
        break;
      }
      case K::Exit:
        std::printf("exit");
        break;
      case K::Alloc:
        std::printf("%s = alloc %s", vname(fn, I.dst).c_str(),
                    vname(fn, I.a).c_str());
        break;
      case K::Load:
        std::printf("%s = %s[%s]", vname(fn, I.dst).c_str(),
                    vname(fn, I.a).c_str(), vname(fn, I.b2).c_str());
        break;
      case K::Store:
        std::printf("%s[%s] = %s", vname(fn, I.a).c_str(),
                    vname(fn, I.b2).c_str(), vname(fn, I.c2).c_str());
        break;
      case K::Len:
        std::printf("%s = len %s", vname(fn, I.dst).c_str(),
                    vname(fn, I.a).c_str());
        break;
      case K::Input:
        std::printf("%s = input()", vname(fn, I.dst).c_str());
        break;
      case K::Call: {
        std::printf("%s = f%u(", vname(fn, I.dst).c_str(), (unsigned)I.c);
        for (size_t k = 0; k < I.args.size(); ++k)
          std::printf("%s%s", k ? ", " : "", vname(fn, I.args[k]).c_str());
        std::printf(")");
        break;
      }
      case K::Ret:
        if (I.a == NONE) std::printf("ret 0");
        else std::printf("ret %s", vname(fn, I.a).c_str());
        break;
      }
      std::printf("\n");
    }
  }
}

static void print_analysis(const An &A, const Fn &fn, const char *tag) {
  uint32_t nloops = 0;
  std::printf("  [%s] blocks=%u", tag, (unsigned)A.n);
  std::printf(" | loops:");
  bool first = true;
  for (uint32_t h = 0; h < A.n; ++h) {
    if (A.inner[h] != h) continue;
    auto row = A.loops.range()[h];
    ++nloops;
    std::printf("%s{hdr=b%u,depth=%u,size=%u}", first ? "" : ",",
                (unsigned)h, (unsigned)(A.nest[h] + 1),
                (unsigned)(row.size() ? (uint32_t)row.size() : 1));
    first = false;
  }
  if (first) std::printf(" none");
  std::printf("\n");
  (void)fn;
  (void)nloops;
}

static void print_run(const RunOut &R) {
  if (R.trapped) { std::printf("TRAP\n"); return; }
  if (R.timeout) { std::printf("TIMEOUT\n"); return; }
  for (size_t i = 0; i < R.out.size(); ++i)
    std::printf("%s%lld", i ? " " : "", (long long)R.out[i]);
  if (R.out.empty()) std::printf("(no output)");
  std::printf("\n");
}

constexpr bool same_run(const RunOut &a, const RunOut &b) {
  return a.trapped == b.trapped && a.timeout == b.timeout && a.out == b.out;
}

// ============================================================================
// 13.5 BACKENDS (runtime only): C, x86-64 asm (GAS), bytecode listing,
//      and the native build&run harness used by --run.
//      All backends consume the DESTROYED IR (no phis).
//      Output text contract shared by every runner (incl. interpreters):
//        one "%lld\n" line per print; "TRAP\n" appended if a div/mod traps;
//        "TIMEOUT\n" if a step-limited runner hit its limit.
// ============================================================================

// canonical text of an in-process RunOut — comparable with native stdout
static std::string text_of(const RunOut &R) {
  std::string s{};
  char buf[32];
  for (val_t v : R.out) {
    std::snprintf(buf, sizeof buf, "%lld\n", (long long)v);
    s += buf;
  }
  if (R.trapped) s += "TRAP\n";
  else if (R.timeout) s += "TIMEOUT\n";
  return s;
}

// ---- 13.5z. EMISSION PLAN -----------------------------------------------------
// Everything the native backends need to emit non-redundant code, computed
// once from the destroyed IR:
//   * block layout: reachable blocks in RPO order, extended by "traces" that
//     chain a block with its preferred unplaced successor (fewer preds wins;
//     ties prefer the else/false arm). With a successor emitted right after,
//     `goto`/`jmp` to it is dropped (fallthrough) in both backends.
//   * constants: every use of a Const-defined vreg is inlined as a literal;
//     the Const def and its C declaration disappear.
//   * branch fusion: a cbr whose condition is a single-use comparison
//     becomes `if (a < b) goto L;` / `cmp+jcc` — no setcc/movzbq/roundtrip.
//   * dead flags: pure instrs whose dst has no remaining textual use.
struct EmitPlan {
  std::vector<uint32_t> order{};  // reachable blocks, emission order
  std::vector<uint32_t> next{};   // per block: block emitted right after
  std::vector<char> kc{};         // per vreg: defined by Const
  std::vector<val_t> kv{};        // per vreg: that constant
  std::vector<uint32_t> uses{};   // per vreg: textual uses after inlining
  std::vector<uint32_t> fuse{};   // per instr: Cbr -> fused compare def idx
  std::vector<char> dead{};       // per instr: skip (pure & unused / inlined)
};

static EmitPlan make_emit_plan(const Fn &fn) {
  EmitPlan P{};
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  // reachability from entry
  std::vector<char> reach(nb, 0);
  {
    std::vector<uint32_t> st{0};
    reach[0] = 1;
    while (!st.empty()) {
      uint32_t b = st.back();
      st.pop_back();
      for (uint32_t s : {fn.succ0(b), fn.succ1(b)})
        if (s != NONE && !reach[s]) {
          reach[s] = 1;
          st.push_back(s);
        }
    }
  }
  // reverse postorder (succ0 first => loop headers precede their bodies)
  std::vector<uint32_t> rpo{};
  {
    std::vector<uint32_t> post{};
    post.reserve(nb);
    std::vector<char> seen(nb, 0);
    std::vector<std::pair<uint32_t, uint32_t>> st{};
    if (reach[0]) {
      seen[0] = 1;
      st.push_back({0, 0});
    }
    while (!st.empty()) {
      uint32_t b = st.back().first, ci = st.back().second;
      if (ci < 2) {
        uint32_t s = (ci == 0) ? fn.succ0(b) : fn.succ1(b);
        ++st.back().second;
        if (s != NONE && reach[s] && !seen[s]) {
          seen[s] = 1;
          st.push_back({s, 0});
        }
      } else {
        post.push_back(b);
        st.pop_back();
      }
    }
    rpo.assign(post.rbegin(), post.rend());
  }
  // trace layout: chain each block with its preferred unplaced successor
  std::vector<char> placed(nb, 0);
  P.next.assign(nb, NONE);
  for (uint32_t b : rpo) {
    if (!reach[b] || placed[b]) continue;
    uint32_t cur = b;
    for (;;) {
      placed[cur] = 1;
      P.order.push_back(cur);
      uint32_t c0 = fn.succ0(cur), c1 = fn.succ1(cur);
      auto cand = [&](uint32_t s) -> bool {
        return s != NONE && s < nb && reach[s] && !placed[s];
      };
      uint32_t pick = NONE;
      if (cand(c0) && cand(c1))
        pick = (fn.preds[c1].size() < fn.preds[c0].size()) ? c1 : c0;
      else if (cand(c1))
        pick = c1; // else/false arm: `if (c) goto T; <else falls through>`
      else if (cand(c0))
        pick = c0;
      if (pick == NONE) break;
      cur = pick;
    }
  }
  for (size_t k = 0; k + 1 < P.order.size(); ++k)
    P.next[P.order[k]] = P.order[k + 1];
  // constants: a vreg is literal-inlinable ONLY when it has exactly ONE def
  // and that def is a Const. Post-destruction phi slots can hold several
  // defs, one of which post_destruct_peephole may have folded into a Const
  // (e.g. `c3 = 5` on the entry edge, `c3 = t` on the back edge): inlining
  // the one Const def at every use would freeze the slot's other-path
  // values to that constant and miscompile the loop test.
  P.kc.assign(nv, 0);
  P.kv.assign(nv, 0);
  {
    std::vector<uint32_t> ndefs(nv, 0);
    for (const Instr &I : fn.code)
      if (I.dst != NONE && I.dst < nv) ++ndefs[I.dst];
    for (const Instr &I : fn.code)
      if (I.k == K::Const && I.dst != NONE && I.dst < nv &&
          ndefs[I.dst] == 1) {
        P.kc[I.dst] = 1;
        P.kv[I.dst] = I.c;
      }
  }
  // raw use counts + def index (for fusion decisions); ndefs also gates
  // fusion (see the comment at the fuse loop below)
  std::vector<uint32_t> raw(nv, 0);
  std::vector<uint32_t> def_idx(nv, NONE);
  std::vector<uint32_t> ndefs(nv, 0);
  for (uint32_t i = 0; i < fn.code.size(); ++i)
    if (fn.code[i].dst != NONE && fn.code[i].dst < nv) ++ndefs[fn.code[i].dst];
  for (uint32_t i = 0; i < fn.code.size(); ++i) {
    const Instr &I = fn.code[i];
    if (I.a != NONE && I.a < nv) raw[I.a]++;
    if (I.b2 != NONE && I.b2 < nv) raw[I.b2]++;
    if (I.c2 != NONE && I.c2 < nv) raw[I.c2]++;
    for (uint32_t v : I.args)
      if (v < nv) raw[v]++;
    if (I.dst != NONE && I.dst < nv) def_idx[I.dst] = i;
  }
  // branch fusion: cbr on a single-use comparison
  P.fuse.assign(fn.code.size(), NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k != K::Cbr || I.a == NONE || I.a >= nv) continue;
      uint32_t d = def_idx[I.a];
      // single USE is not enough post-destruction: a phi slot has one use
      // (this branch) but one def per incoming edge; fusing "the" def would
      // pick an arbitrary edge's comparison. Require a single DEF too.
      if (d == NONE || raw[I.a] != 1 || ndefs[I.a] != 1) continue;
      const Instr &D = fn.code[d];
      bool is_cmp = D.k == K::Bin &&
                    (D.b == B::Lt || D.b == B::Le || D.b == B::Gt ||
                     D.b == B::Ge || D.b == B::Eq || D.b == B::Ne);
      if (is_cmp) P.fuse[i] = d;
    }
  // textual uses (consts inlined, fused cbrs read the comparison operands)
  P.uses.assign(nv, 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const) continue; // inlined at every use
      if (P.fuse[i] != NONE) {
        const Instr &D = fn.code[P.fuse[i]];
        if (D.a != NONE && D.a < nv && !P.kc[D.a]) P.uses[D.a]++;
        if (D.b2 != NONE && D.b2 < nv && !P.kc[D.b2]) P.uses[D.b2]++;
        continue;
      }
      for (uint32_t v : {I.a, I.b2, I.c2})
        if (v != NONE && v < nv && !P.kc[v]) P.uses[v]++;
      for (uint32_t v : I.args)
        if (v != NONE && v < nv && !P.kc[v]) P.uses[v]++;
    }
  // dead: pure instrs whose dst is never textually read
  P.dead.assign(fn.code.size(), 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const) {
        // dead (inlined at every use) only when the slot is const-inlinable,
        // i.e. single-def; a multi-def slot's Const edge write is real code
        P.dead[i] =
            (I.dst != NONE && I.dst < nv && P.kc[I.dst]) ? 1 : 0;
        continue;
      }
      if (I.dst == NONE || I.dst >= nv) continue;
      if (P.uses[I.dst] != 0) continue;
      bool pure = I.k == K::Copy || I.k == K::Len ||
                  (I.k == K::Bin && I.b != B::Div && I.b != B::Mod);
      if (pure) P.dead[i] = 1;
    }
  for (uint32_t d : P.fuse)
    if (d != NONE) P.dead[d] = 1; // consumed by the branch text
  return P;
}

// ---- 13.5a. C backend --------------------------------------------------------
// Emits C valid as both C and C++. One function per FnUnit: helpers get
// `static int64_t mjz_fN(...)` signatures (params are vregs v0..vP-1), a
// depth guard (runaway recursion TRAPs exactly like the interpreters/VM),
// and MJZ_RET epilogues; main is `int main(void)`. Arithmetic goes through
// uint64_t casts: the wraps the interpreters produce are UB-free here;
// div/mod are guarded so the trap fires BEFORE the UB division would.
static std::string emit_c_fn(const Fn &fn, uint32_t fidx, bool is_main,
                            bool multi_fn) {
  EmitPlan P = make_emit_plan(fn);
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  std::string s{};
  char buf[192];
  // home naming: r%u (allocated slot) when register allocation ran,
  // v%u (one local per vreg, "SSA-like") with --no-regalloc / -O0
  const bool ra = !fn.slot_of.empty();
  auto D = [&](uint32_t v) -> std::string { // home of a DEF/write (vreg id)
    if (ra) std::snprintf(buf, sizeof buf, "r%u", (unsigned)slot_id(fn, v));
    else std::snprintf(buf, sizeof buf, "v%u", (unsigned)v);
    return std::string(buf);
  };
  auto SN = [&](uint32_t slot) -> std::string { // name of a SLOT index
    std::snprintf(buf, sizeof buf, ra ? "r%u" : "v%u", (unsigned)slot);
    return std::string(buf);
  };
  // stack-local arrays (analysis ran on the SSA form): vreg -> entry index
  std::vector<int32_t> arr_of(nv, -1);
  for (size_t k = 0; k < fn.stk_arrays.size(); ++k)
    for (uint32_t v : fn.stk_arrays[k].vregs)
      if (v < nv) arr_of[v] = (int32_t)k;
  auto arr_name = [&](size_t k) -> std::string {
    return "mjz_s" + std::to_string(fn.stk_arrays[k].root);
  };
  // operand text: inlined literal or variable (consts are always inlined)
  auto V = [&](uint32_t v) -> std::string {
    if (v == NONE) return "0";
    if (v < nv && P.kc[v]) {
      val_t c = P.kv[v];
      if (c >= -2147483647 && c <= 2147483647)
        std::snprintf(buf, sizeof buf, "%lld", (long long)c);
      else
        std::snprintf(buf, sizeof buf, "UINT64_C(0x%016llx)",
                      (unsigned long long)(uint64_t)c);
      return std::string(buf);
    }
    return D(v);
  };
  auto cmp_txt = [](B b) -> const char * {
    switch (b) {
    case B::Lt: return "<";
    case B::Le: return "<=";
    case B::Gt: return ">";
    case B::Ge: return ">=";
    case B::Eq: return "==";
    default: return "!=";
    }
  };
  // negation of a comparison (for branches that fall through on the true
  // arm): `if (!(a < b)) goto L;` becomes `if (a >= b) goto L;` — the NOT of
  // a compare is trivially combinable into the compare itself
  auto neg_txt = [](B b) -> const char * {
    switch (b) {
    case B::Lt: return ">=";
    case B::Le: return ">";
    case B::Gt: return "<=";
    case B::Ge: return "<";
    case B::Eq: return "!=";
    default: return "==";
    }
  };
  // signature (non-main: parameters are the first vregs, v0..vP-1)
  if (is_main) {
    s += "int main(void) {\n";
  } else {
    s += "static int64_t mjz_f" + std::to_string(fidx) + "(";
    for (uint32_t p = 0; p < fn.nparams; ++p)
      s += p ? ", int64_t " + D(p) : "int64_t " + D(p);
    s += ") {\n";
    if (multi_fn)
      s += "  if (++mjz_depth > 512) { printf(\"TRAP\\n\"); exit(1); }\n";
  }
  // declarations: every slot WRITTEN by a non-dead instruction needs one
  // (written-but-unread slots still exist: e.g. a kept-for-trap Alloc whose
  // handle is never used). Read-only slots cannot exist post-SSA.
  // No initializers: SSA dominance (+ the explicit `Const 0` materialization
  // of undef phi args during destruction) guarantees every slot is written
  // before any read on every path — the same property the asm backend has
  // relied on since it skipped slot zero-init. C compilers at -O2 can now
  // keep these in registers.
  {
    const uint32_t ns = slot_count(fn);
    std::vector<char> written(ns, 0);
    for (uint32_t b : P.order)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        if (P.dead[i]) continue;
        uint32_t dst = fn.code[i].dst;
        if (dst != NONE && dst < nv) written[slot_id(fn, dst)] = 1;
      }
    for (uint32_t sl = fn.nparams; sl < ns; ++sl) {
      if (!written[sl]) continue;
      std::snprintf(buf, sizeof buf, "  int64_t %s;\n", SN(sl).c_str());
      s += buf;
    }
  }
  for (size_t k = 0; k < fn.stk_arrays.size(); ++k) { // local arrays
    std::snprintf(buf, sizeof buf, "  int64_t mjz_s%u[%lld];\n",
                  (unsigned)fn.stk_arrays[k].root,
                  (long long)fn.stk_arrays[k].len);
    s += buf;
  }
  // referenced labels (so fallthrough-only blocks need no label at all)
  std::vector<char> refd(fn.nblocks(), 0);
  for (uint32_t b : P.order)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (P.dead[i]) continue;
      if (I.k == K::Br && I.t != P.next[b]) refd[I.t] = 1;
      if (I.k == K::Cbr) {
        if (P.fuse[i] == NONE && I.a < nv && P.kc[I.a]) {
          uint32_t tgt = (P.kv[I.a] != 0) ? I.t : I.f; // constant condition
          if (tgt != P.next[b]) refd[tgt] = 1;
        } else if (I.f == P.next[b]) {
          refd[I.t] = 1; // `if (cond) goto L_t;` false arm falls through
        } else if (I.t == P.next[b]) {
          refd[I.f] = 1; // `if (!(cond)) goto L_f;` true arm falls through
        } else {
          refd[I.t] = 1;
          refd[I.f] = 1;
        }
      }
    }
  bool saw_exit = false;
  // body, in layout order: one successor falls through, goto elided
  for (uint32_t b : P.order) {
    if (refd[b]) {
      std::snprintf(buf, sizeof buf, "L%u:;\n", (unsigned)b);
      s += buf;
    }
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (P.dead[i]) continue;
      switch (I.k) {
      case K::Phi:
        break; // destroyed input has none
      case K::Const:
        if (!P.dead[i]) { // multi-def slot: a real write of THIS constant
          val_t c = I.c;
          if (c >= -2147483647 && c <= 2147483647)
            std::snprintf(buf, sizeof buf, "  %s = %lld;\n", D(I.dst).c_str(),
                          (long long)c);
          else
            std::snprintf(buf, sizeof buf, "  %s = UINT64_C(0x%016llx);\n",
                          D(I.dst).c_str(),
                          (unsigned long long)(uint64_t)c);
          s += buf;
        }
        break; // dead ones are inlined at their uses
      case K::Copy:
        if (D(I.dst) == V(I.a))
          break; // same slot after coalescing: a no-op self-assignment
        std::snprintf(buf, sizeof buf, "  %s = %s;\n", D(I.dst).c_str(),
                      V(I.a).c_str());
        s += buf;
        break;
      case K::Bin: {
        if (I.b == B::Add || I.b == B::Sub || I.b == B::Mul) {
          const char *op = I.b == B::Add ? "+" : (I.b == B::Sub ? "-" : "*");
          std::snprintf(buf, sizeof buf,
                        "  %s = (int64_t)((uint64_t)%s %s (uint64_t)%s);\n",
                        D(I.dst).c_str(), V(I.a).c_str(), op, V(I.b2).c_str());
          s += buf;
        } else if (I.b == B::Div || I.b == B::Mod) {
          const char *op = I.b == B::Div ? "/" : "%";
          bool bc = I.b2 < nv && P.kc[I.b2];
          val_t bv = bc ? P.kv[I.b2] : 0;
          bool ac = I.a < nv && P.kc[I.a];
          val_t av = ac ? P.kv[I.a] : 0;
          bool always =
              bc && (bv == 0 || (bv == -1 && ac && av == INT64_MIN));
          if (always) {
            s += "  printf(\"TRAP\\n\"); return 1;\n";
            break;
          }
          if (bc && bv == -1)
            s += "  if (" + V(I.a) +
                 " == INT64_MIN) { printf(\"TRAP\\n\"); return 1; }\n";
          else if (!bc)
            s += "  if (" + V(I.b2) + " == 0 || (" + V(I.a) +
                 " == INT64_MIN && " + V(I.b2) +
                 " == -1)) { printf(\"TRAP\\n\"); return 1; }\n";
          std::snprintf(buf, sizeof buf, "  %s = %s %s %s;\n",
                        D(I.dst).c_str(), V(I.a).c_str(), op, V(I.b2).c_str());
          s += buf;
        } else if (I.b == B::And || I.b == B::Or || I.b == B::Xor) {
          const char *op = I.b == B::And ? "&" : (I.b == B::Or ? "|" : "^");
          std::snprintf(buf, sizeof buf,
                        "  %s = (int64_t)((uint64_t)%s %s (uint64_t)%s);\n",
                        D(I.dst).c_str(), V(I.a).c_str(), op, V(I.b2).c_str());
          s += buf;
        } else if (I.b == B::MulHi) {
          std::snprintf(buf, sizeof buf,
                        "  %s = (int64_t)(((unsigned __int128)(uint64_t)%s * "
                        "(uint64_t)%s) >> 64);\n",
                        D(I.dst).c_str(), V(I.a).c_str(), V(I.b2).c_str());
          s += buf;
        } else if (I.b == B::Shl || I.b == B::LShr || I.b == B::AShr) {
          // amounts masked to 6 bits, matching the interpreters/VM exactly
          if (I.b == B::Shl)
            std::snprintf(buf, sizeof buf,
                          "  %s = (int64_t)((uint64_t)%s << ((uint64_t)%s & 63));\n",
                          D(I.dst).c_str(), V(I.a).c_str(), V(I.b2).c_str());
          else if (I.b == B::LShr)
            std::snprintf(buf, sizeof buf,
                          "  %s = (int64_t)((uint64_t)%s >> ((uint64_t)%s & 63));\n",
                          D(I.dst).c_str(), V(I.a).c_str(), V(I.b2).c_str());
          else
            std::snprintf(buf, sizeof buf,
                          "  %s = %s >> (int64_t)((uint64_t)%s & 63);\n",
                          D(I.dst).c_str(), V(I.a).c_str(), V(I.b2).c_str());
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  %s = (%s %s %s);\n",
                        D(I.dst).c_str(), V(I.a).c_str(), cmp_txt(I.b),
                        V(I.b2).c_str());
          s += buf;
        }
        break;
      }
      case K::Br:
        if (I.t != P.next[b]) { // else: fall through
          std::snprintf(buf, sizeof buf, "  goto L%u;\n", (unsigned)I.t);
          s += buf;
        }
        break;
      case K::Cbr: {
        uint32_t tgt = NONE;
        if (P.fuse[i] != NONE) { // fused: branch on the comparison itself;
          // when the TRUE arm is the fallthrough, the NOT of the compare is
          // folded INTO the compare: `if (!(a<b)) goto L;` -> `if (a>=b) goto L;`
          const Instr &D = fn.code[P.fuse[i]];
          std::string pos =
              "(" + V(D.a) + " " + cmp_txt(D.b) + " " + V(D.b2) + ")";
          std::string neg =
              "(" + V(D.a) + " " + neg_txt(D.b) + " " + V(D.b2) + ")";
          if (I.f == P.next[b]) { // false arm falls through
            std::snprintf(buf, sizeof buf, "  if %s goto L%u;\n", pos.c_str(),
                          (unsigned)I.t);
            s += buf;
          } else if (I.t == P.next[b]) { // true arm falls through: negated
            std::snprintf(buf, sizeof buf, "  if %s goto L%u;\n", neg.c_str(),
                          (unsigned)I.f);
            s += buf;
          } else {
            std::snprintf(buf, sizeof buf, "  if %s goto L%u;\n", pos.c_str(),
                          (unsigned)I.t);
            s += buf;
            std::snprintf(buf, sizeof buf, "  goto L%u;\n", (unsigned)I.f);
            s += buf;
          }
          break;
        }
        if (I.a < nv && P.kc[I.a]) { // constant condition
          tgt = (P.kv[I.a] != 0) ? I.t : I.f;
          if (tgt != P.next[b]) {
            std::snprintf(buf, sizeof buf, "  goto L%u;\n", (unsigned)tgt);
            s += buf;
          }
          break;
        }
        std::string cond = "(" + V(I.a) + " != 0)";
        if (I.f == P.next[b]) { // false arm falls through
          std::snprintf(buf, sizeof buf, "  if %s goto L%u;\n", cond.c_str(),
                        (unsigned)I.t);
          s += buf;
        } else if (I.t == P.next[b]) { // true arm falls through: invert
          std::snprintf(buf, sizeof buf, "  if (!(%s)) goto L%u;\n",
                        cond.c_str(), (unsigned)I.f);
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  if %s goto L%u;\n", cond.c_str(),
                        (unsigned)I.t);
          s += buf;
          std::snprintf(buf, sizeof buf, "  goto L%u;\n", (unsigned)I.f);
          s += buf;
        }
        break;
      }
      case K::Print:
        std::snprintf(buf, sizeof buf,
                      "  printf(\"%%lld\\n\", (long long)%s);\n",
                      V(I.a).c_str());
        s += buf;
        break;
      case K::Exit:
        saw_exit = true;
        s += "  return 0;\n";
        break;
      case K::Alloc:
        if (I.dst < nv && arr_of[I.dst] >= 0) { // local: zero-fill a stack array
          std::snprintf(buf, sizeof buf, "  memset(%s, 0, sizeof %s);\n",
                        arr_name(arr_of[I.dst]).c_str(),
                        arr_name(arr_of[I.dst]).c_str());
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  %s = mjz_alloc(%s);\n",
                        D(I.dst).c_str(), V(I.a).c_str());
          s += buf;
        }
        break;
      case K::Load:
        if (I.a < nv && arr_of[I.a] >= 0) { // local array: direct + bounds check
          std::snprintf(buf, sizeof buf,
                        "  if (%s < 0 || %s >= %lld) { printf(\"TRAP\\n\"); "
                        "exit(1); }\n",
                        V(I.b2).c_str(), V(I.b2).c_str(),
                        (long long)fn.stk_arrays[arr_of[I.a]].len);
          s += buf;
          std::snprintf(buf, sizeof buf, "  %s = %s[%s];\n", D(I.dst).c_str(),
                        arr_name(arr_of[I.a]).c_str(), V(I.b2).c_str());
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  %s = mjz_load(%s, %s);\n",
                        D(I.dst).c_str(), V(I.a).c_str(), V(I.b2).c_str());
          s += buf;
        }
        break;
      case K::Store:
        if (I.a < nv && arr_of[I.a] >= 0) { // local array: direct + bounds check
          std::snprintf(buf, sizeof buf,
                        "  if (%s < 0 || %s >= %lld) { printf(\"TRAP\\n\"); "
                        "exit(1); }\n",
                        V(I.b2).c_str(), V(I.b2).c_str(),
                        (long long)fn.stk_arrays[arr_of[I.a]].len);
          s += buf;
          std::snprintf(buf, sizeof buf, "  %s[%s] = %s;\n",
                        arr_name(arr_of[I.a]).c_str(), V(I.b2).c_str(),
                        V(I.c2).c_str());
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  mjz_store(%s, %s, %s);\n",
                        V(I.a).c_str(), V(I.b2).c_str(), V(I.c2).c_str());
          s += buf;
        }
        break;
      case K::Len:
        if (I.a < nv && arr_of[I.a] >= 0) { // local: length is a constant
          std::snprintf(buf, sizeof buf, "  %s = %lld;\n", D(I.dst).c_str(),
                        (long long)fn.stk_arrays[arr_of[I.a]].len);
          s += buf;
        } else {
          std::snprintf(buf, sizeof buf, "  %s = mjz_len(%s);\n",
                        D(I.dst).c_str(), V(I.a).c_str());
          s += buf;
        }
        break;
      case K::Input:
        std::snprintf(buf, sizeof buf, "  %s = mjz_input();\n",
                      D(I.dst).c_str());
        s += buf;
        break;
      case K::Call: {
        std::string as{};
        for (size_t k = 0; k < I.args.size(); ++k)
          as += k ? ", " + V(I.args[k]) : V(I.args[k]);
        std::snprintf(buf, sizeof buf, "  %s = mjz_f%u(%s);\n",
                      D(I.dst).c_str(), (unsigned)I.c, as.c_str());
        s += buf;
        break;
      }
      case K::Ret:
        if (multi_fn)
          s += "  MJZ_RET(" + V(I.a) + ");\n";
        else // main has no Ret; this is defensive
          s += "  return " + V(I.a) + ";\n";
        break;
      }
    }
  }
  if (is_main)
    s += saw_exit ? "}\n" : "  return 0;\n}\n";
  else // all paths returned already; this is unreachable-fallthrough safety
    s += "  --mjz_depth; return 0;\n}\n";
  return s;
}

// whole-program C: shared helpers, prototypes (mutual recursion is legal),
// callee bodies first, then main.
static std::string emit_c(const std::vector<Fn> &fns,
                          const std::vector<std::string_view> &names) {
  (void)names;
  std::string s{};
  bool use_arr = false, use_in = false, use_stk = false;
  auto stk_arr_of = [](const Fn &fn, uint32_t v) -> int {
    for (size_t k = 0; k < fn.stk_arrays.size(); ++k)
      for (uint32_t u : fn.stk_arrays[k].vregs)
        if (u == v) return (int)k;
    return -1;
  };
  for (const Fn &fn : fns) {
    if (!fn.stk_arrays.empty()) use_stk = true;
    for (const Instr &I : fn.code) {
      const uint32_t nv =
          fn.nvars + (uint32_t)fn.version_base.size();
      if (I.k == K::Alloc) {
        if (I.dst < nv && stk_arr_of(fn, I.dst) >= 0) continue; // stack
        use_arr = true;
      } else if (I.k == K::Load || I.k == K::Store || I.k == K::Len) {
        if (I.a < nv && stk_arr_of(fn, I.a) >= 0) continue; // stack
        use_arr = true;
      } else if (I.k == K::Input)
        use_in = true;
    }
  }
  const bool multi = fns.size() > 1;
  s += "/* generated by minijz */\n";
  s += "#include <stdio.h>\n#include <stdint.h>\n";
  if (use_arr || multi || use_stk) s += "#include <stdlib.h>\n"; // exit()
  if (use_stk) s += "#include <string.h>\n";                    // memset()
  if (use_arr) {
    // bump arena, bit-identical to the interpreters/VM/asm backends:
    // handle = index of first element; len[] is 0 for non-handles
    s += "#define MJZ_ARENA_CAP 2097152\n";
    s += "static int64_t mjz_arena[MJZ_ARENA_CAP + 1];\n";
    s += "static int64_t mjz_alen[MJZ_ARENA_CAP + 1];\n";
    s += "static int64_t mjz_atop = 0;\n";
    s += "static int64_t mjz_alloc(int64_t n) {\n";
    s += "  if (n < 0 || n > MJZ_ARENA_CAP ||\n";
    s += "      mjz_atop > MJZ_ARENA_CAP - n) { printf(\"TRAP\\n\"); exit(1); }\n";
    s += "  int64_t h = mjz_atop;\n";
    s += "  mjz_atop += n;\n";
    s += "  mjz_alen[h] = n;\n";
    s += "  return h;\n";
    s += "}\n";
    s += "static int64_t mjz_len(int64_t h) {\n";
    s += "  if (h < 0 || h > MJZ_ARENA_CAP) return 0;\n";
    s += "  return mjz_alen[h];\n";
    s += "}\n";
    s += "static int64_t mjz_load(int64_t h, int64_t i) {\n";
    s += "  if (i < 0 || i >= mjz_len(h)) { printf(\"TRAP\\n\"); exit(1); }\n";
    s += "  return mjz_arena[h + i];\n";
    s += "}\n";
    s += "static void mjz_store(int64_t h, int64_t i, int64_t v) {\n";
    s += "  if (i < 0 || i >= mjz_len(h)) { printf(\"TRAP\\n\"); exit(1); }\n";
    s += "  mjz_arena[h + i] = v;\n";
    s += "}\n";
  }
  if (use_in) {
    s += "static int64_t mjz_input(void) {\n";
    s += "  long long v = 0;\n";
    s += "  if (scanf(\"%lld\", &v) != 1) return 0;\n"; // EOF/parse fail -> 0
    s += "  return (int64_t)v;\n";
    s += "}\n";
  }
  s += "\n";

  if (multi) {
    s += "static int64_t mjz_depth = 0;\n";
    s += "#define MJZ_RET(x) do { --mjz_depth; "
         "return (int64_t)(x); } while (0)\n";
  }
  s += "\n";
  for (uint32_t f = 1; f < fns.size(); ++f) { // prototypes first
    const Fn &fn = fns[f];
    const bool ra = !fn.slot_of.empty();
    s += "static int64_t mjz_f" + std::to_string(f) + "(";
    for (uint32_t p = 0; p < fn.nparams; ++p) {
      char pb[32];
      if (ra) std::snprintf(pb, sizeof pb, "r%u", (unsigned)slot_id(fn, p));
      else std::snprintf(pb, sizeof pb, "v%u", (unsigned)p);
      s += p ? ", int64_t " + std::string(pb) : "int64_t " + std::string(pb);
    }
    s += ");\n";
  }
  if (fns.size() > 1) s += "\n";
  for (uint32_t f = 1; f < fns.size(); ++f) // callees first, main last
    s += emit_c_fn(fns[f], f, false, multi);
  s += emit_c_fn(fns[0], 0, true, multi);
  return s;
}

// ---- 13.5b.0 register allocation (chordal graph coloring, via mjz_lib) -----
// Live ranges are computed on the destroyed code (backward dataflow to a
// fixpoint); at every definition the def interferes with everything live
// after it. That interference graph is handed to mjz::graph_ns::
// color_chordal_graph — greedy coloring in maximum-cardinality-search order,
// which is OPTIMAL for chordal graphs, and SSA-shaped interference graphs are
// chordal. The pool is the five callee-saved registers rbx/r12-r15, so any
// value live across a call is safe with no extra saving; callers only need
// the (counted) push/pop of the subset actually used. Anything the coloring
// cannot house (color >= 5) spills to the classic -N(%rbp) slot.
struct RaPlan {
  std::vector<int32_t> color{};  // -1: no location; 0..4: register; 5: spill
  std::vector<uint32_t> spill{}; // per vreg: spill slot ordinal
  uint32_t nspill{};
  uint32_t usedmask{};
};
static RaPlan ra_plan(const Fn &fn, const EmitPlan &P) {
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  RaPlan R{};
  R.color.assign(nv, -1);
  R.spill.assign(nv, 0);
  if (!nb || !nv) return R;
  // textual-use reader honoring inlined consts and fused branches
  auto each_use = [&](uint32_t i, auto &&f) {
    const Instr &I = fn.code[i];
    if (P.fuse[i] != NONE) { // the branch reads the comparison's operands
      const Instr &D = fn.code[P.fuse[i]];
      auto one = [&](uint32_t v) {
        if (v != NONE && v < nv && !P.kc[v]) f(v);
      };
      one(D.a);
      one(D.b2);
      return;
    }
    auto one = [&](uint32_t v) {
      if (v != NONE && v < nv && !P.kc[v]) f(v);
    };
    one(I.a);
    one(I.b2);
    one(I.c2);
    for (uint32_t v : I.args) one(v);
  };
  auto is_def = [&](uint32_t i) -> uint32_t { // live def of instr i (or NONE)
    const Instr &I = fn.code[i];
    if (P.dead[i]) return NONE;
    if (I.dst == NONE || I.dst >= nv || P.kc[I.dst]) return NONE;
    return I.dst;
  };
  // ---- block liveness (backward scan, fixpoint over successors) ----------
  std::vector<std::vector<char>> lin(nb, std::vector<char>(nv, 0)),
      lout(nb, std::vector<char>(nv, 0));
  for (int round = 0; round < 64; ++round) {
    bool changed = false;
    for (int32_t b = (int32_t)nb - 1; b >= 0; --b) {
      std::vector<char> &lo = lout[(uint32_t)b];
      std::fill(lo.begin(), lo.end(), 0);
      for (uint32_t s : {fn.succ0((uint32_t)b), fn.succ1((uint32_t)b)})
        if (s != NONE && s < nb)
          for (uint32_t v = 0; v < nv; ++v) lo[v] |= lin[s][v];
      std::vector<char> live = lo;
      for (uint32_t i = fn.blocks[(uint32_t)b].end;
           i-- > fn.blocks[(uint32_t)b].begin;) {
        if (P.dead[i]) continue;
        uint32_t d = is_def(i);
        if (d != NONE) live[d] = 0;
        each_use(i, [&](uint32_t v) { live[v] = 1; });
      }
      for (uint32_t v = 0; v < nv; ++v)
        if (live[v] != lin[(uint32_t)b][v]) {
          lin[(uint32_t)b][v] = live[v];
          changed = true;
        }
    }
    if (!changed) break;
  }
  // ---- interference edges ------------------------------------------------
  std::vector<std::vector<ulen>> adj(nv);
  auto edge = [&](uint32_t a, uint32_t b) {
    if (a == b || a >= nv || b >= nv) return;
    for (uint32_t x : adj[a])
      if (x == b) return;
    adj[a].push_back(b);
    adj[b].push_back(a);
  };
  { // parameters are "defined at entry": they conflict with each other and
    // with anything live into the entry block
    std::vector<char> live = lin[0];
    for (uint32_t p = 0; p < fn.nparams && p < nv; ++p) {
      for (uint32_t u = 0; u < nv; ++u)
        if (live[u] && u != p) edge(p, u);
      live[p] = 0;
    }
  }
  for (uint32_t b = 0; b < nb; ++b) {
    std::vector<char> live = lout[b];
    for (uint32_t i = fn.blocks[b].end; i-- > fn.blocks[b].begin;) {
      if (P.dead[i]) continue;
      uint32_t d = is_def(i);
      if (d != NONE) {
        for (uint32_t u = 0; u < nv; ++u)
          if (live[u]) edge(d, u);
        live[d] = 0;
      }
      each_use(i, [&](uint32_t v) { live[v] = 1; });
    }
  }
  // ---- the lib does the coloring ------------------------------------------
  auto forest = gns::make_basic_forest<V>(adj);
  auto colored = gns::color_chordal_graph<V>(forest);
  // ---- assignment: colors 0..4 -> rbx,r12..r15; the rest spill ------------
  std::vector<char> hasloc(nv, 0);
  for (uint32_t v = 0; v < fn.nparams && v < nv; ++v) hasloc[v] = 1;
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (is_def(i) != NONE) hasloc[fn.code[i].dst] = 1;
  for (uint32_t b = 0; b < nb; ++b) // defensive: used-but-undefined
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      each_use(i, [&](uint32_t v) { hasloc[v] = 1; });
  uint32_t spills = 0;
  for (uint32_t v = 0; v < nv; ++v) {
    if (!hasloc[v]) continue;
    uint64_t c = (v < colored.first.size()) ? (uint64_t)colored.first[v] : 0;
    if (c < 5) {
      R.color[v] = (int32_t)c;
      R.usedmask |= (uint32_t)1 << c;
    } else {
      R.color[v] = 5;
      R.spill[v] = spills++;
    }
  }
  R.nspill = spills;
  return R;
}

static std::string emit_x64_fn(const Fn &fn, uint32_t fidx, bool is_main,
                              bool multi) {
  EmitPlan P = make_emit_plan(fn);
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  char buf[192];
  std::string s{};
  auto fits32 = [](val_t c) {
    return c >= INT64_C(-2147483648) && c <= INT64_C(2147483647);
  };
  RaPlan RP = ra_plan(fn, P);
  static const char *const RREGS[5] = {"rbx", "r12", "r13", "r14", "r15"};
  auto hascolor = [&](uint32_t v) -> bool {
    return v != NONE && v < nv && RP.color[v] >= 0 && RP.color[v] < 5;
  };
  auto rname = [&](uint32_t v) -> const char * {
    return RREGS[RP.color[v]];
  };
  auto slot = [&](uint32_t v) -> std::string { // spill home of v
    std::snprintf(buf, sizeof buf, "-%u(%%rbp)",
                  (unsigned)(8 * (RP.spill[v] + 1)));
    return std::string(buf);
  };
  auto loc = [&](uint32_t v) -> std::string { // operand text: %reg or slot
    if (hascolor(v)) return std::string("%") + rname(v);
    return slot(v);
  };
  auto is_c = [&](uint32_t v) -> bool {
    return v != NONE && v < nv && P.kc[v];
  };
  auto cv = [&](uint32_t v) -> val_t { return P.kv[v]; };
  // load an operand into a SCRATCH register (rax/rcx/rdx/rsi/rdi/r8/r9)
  auto ld = [&](const char *reg, uint32_t v) {
    if (is_c(v)) {
      if (fits32(cv(v)))
        std::snprintf(buf, sizeof buf, "  movq $%lld, %%%s\n",
                      (long long)cv(v), reg);
      else
        std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%%s\n",
                      (long long)cv(v), reg);
    } else if (hascolor(v)) {
      std::snprintf(buf, sizeof buf, "  movq %%%s, %%%s\n", rname(v), reg);
    } else {
      std::snprintf(buf, sizeof buf, "  movq %s, %%%s\n", slot(v).c_str(),
                    reg);
    }
    s += buf;
  };
  // store %rax into a destination (register in place, or spill slot)
  auto st_rax = [&](uint32_t v) {
    if (v == NONE || v >= nv) return;
    if (hascolor(v)) {
      std::snprintf(buf, sizeof buf, "  movq %%rax, %%%s\n", rname(v));
      s += buf;
    } else {
      s += "  movq %rax, " + slot(v) + "\n";
    }
  };
  // move operand x into dst's register (no-op when already there)
  auto src_into = [&](const char *dr, uint32_t x) {
    if (is_c(x)) {
      if (fits32(cv(x))) {
        std::snprintf(buf, sizeof buf, "  movq $%lld, %%%s\n",
                      (long long)cv(x), dr);
        s += buf;
      } else {
        std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%rax\n",
                      (long long)cv(x));
        s += buf;
        std::snprintf(buf, sizeof buf, "  movq %%rax, %%%s\n", dr);
        s += buf;
      }
    } else if (hascolor(x) && !std::strcmp(rname(x), dr)) {
      // already in place: no code
    } else {
      std::snprintf(buf, sizeof buf, "  movq %s, %%%s\n", loc(x).c_str(),
                    dr);
      s += buf;
    }
  };
  auto jcc_of = [](B b) -> const char * {
    switch (b) {
    case B::Lt: return "jl";
    case B::Le: return "jle";
    case B::Gt: return "jg";
    case B::Ge: return "jge";
    case B::Eq: return "je";
    default: return "jne";
    }
  };
  auto jcc_inv = [](const char *j) -> const char * {
    if (!std::strcmp(j, "jl")) return "jge";
    if (!std::strcmp(j, "jle")) return "jg";
    if (!std::strcmp(j, "jg")) return "jle";
    if (!std::strcmp(j, "jge")) return "jl";
    if (!std::strcmp(j, "je")) return "jne";
    return "je";
  };
  // frame: spill bytes, 16-aligned, padded so calls stay aligned given the
  // (possibly odd) number of pushed callee-saved registers
  const uint32_t npush = (uint32_t)__builtin_popcount(RP.usedmask);
  uint64_t frame = ((uint64_t)RP.nspill * 8 + 15) / 16 * 16;
  if (npush & 1) frame += 8;
  // block labels are file-scope in asm: prefix per function
  auto lbl = [&](uint32_t b) -> std::string {
    char l[48];
    if (is_main) std::snprintf(l, sizeof l, ".Lb%u", (unsigned)b);
    else std::snprintf(l, sizeof l, ".LF%u_%u", (unsigned)fidx, (unsigned)b);
    return std::string(l);
  };
  if (is_main) {
    s += "  .globl main\n  .type main, @function\nmain:\n";
  } else {
    s += "mjz_f" + std::to_string(fidx) + ":\n";
  }
  s += "  pushq %rbp\n  movq %rsp, %rbp\n";
  for (uint32_t c = 0; c < 5; ++c)
    if (RP.usedmask & ((uint32_t)1 << c))
      s += std::string("  pushq %") + RREGS[c] + "\n";
  if (frame) {
    std::snprintf(buf, sizeof buf, "  subq $%llu, %%rsp\n",
                  (unsigned long long)frame);
    s += buf;
  }
  if (!is_main) { // recursion depth guard (matches the VM/interpreters)
    s += "  incq mjz_depth(%rip)\n";
    s += "  cmpq $512, mjz_depth(%rip)\n  jg mjz_trap\n";
    static const char *argregs[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    for (uint32_t p = 0; p < fn.nparams && p < 6; ++p)
      s += std::string("  movq %") + argregs[p] + ", " + loc(p) + "\n";
  }
  // NOTE: no slot zero-init — SSA dominance guarantees every read of a slot
  // is preceded (on its path) by the write of its def; undef phi args were
  // materialized as explicit `Const 0` copies during SSA destruction.
  // referenced labels (fallthrough-only blocks need none)
  std::vector<char> refd(fn.nblocks(), 0);
  for (uint32_t b : P.order)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (P.dead[i]) continue;
      if (I.k == K::Br && I.t != P.next[b]) refd[I.t] = 1;
      if (I.k == K::Cbr) {
        if (P.fuse[i] == NONE && I.a < nv && P.kc[I.a]) {
          uint32_t tgt = (P.kv[I.a] != 0) ? I.t : I.f; // constant condition
          if (tgt != P.next[b]) refd[tgt] = 1;
        } else if (I.f == P.next[b]) {
          refd[I.t] = 1; // `jcc .Lb_t` false arm falls through
        } else if (I.t == P.next[b]) {
          refd[I.f] = 1; // inverted `jcc .Lb_f` true arm falls through
        } else {
          refd[I.t] = 1;
          refd[I.f] = 1;
        }
      }
    }
  bool saw_exit = false;
  for (uint32_t b : P.order) {
    if (refd[b]) s += lbl(b) + ":\n";
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (P.dead[i]) continue;
      switch (I.k) {
      case K::Phi:
        break; // destroyed input has none
      case K::Const: { // multi-def slot: a real write of THIS constant
        if (hascolor(I.dst)) {
          if (fits32(I.c)) {
            std::snprintf(buf, sizeof buf, "  movq $%lld, %%%s\n",
                          (long long)I.c, rname(I.dst));
            s += buf;
          } else {
            std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%rax\n",
                          (long long)I.c);
            s += buf;
            std::snprintf(buf, sizeof buf, "  movq %%rax, %%%s\n",
                          rname(I.dst));
            s += buf;
          }
        } else {
          if (fits32(I.c))
            s += "  movq $" + std::to_string((long long)I.c) + ", " +
                 slot(I.dst) + "\n";
          else {
            std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%rax\n",
                          (long long)I.c);
            s += buf;
            s += "  movq %rax, " + slot(I.dst) + "\n";
          }
        }
        break;
      }
      case K::Copy:
        if (hascolor(I.dst)) {
          src_into(rname(I.dst), I.a);
        } else if (is_c(I.a)) {
          if (fits32(cv(I.a)))
            s += "  movq $" + std::to_string((long long)cv(I.a)) + ", " +
                 slot(I.dst) + "\n";
          else {
            std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%rax\n",
                          (long long)cv(I.a));
            s += buf;
            s += "  movq %rax, " + slot(I.dst) + "\n";
          }
        } else {
          s += "  movq " + loc(I.a) + ", %rax\n";
          s += "  movq %rax, " + slot(I.dst) + "\n";
        }
        break;
      case K::Bin: {
        // normalize: commutative ops put the immediate (if any) on the right
        uint32_t x = I.a, y = I.b2;
        if (is_c(x) && !is_c(y) &&
            (I.b == B::Add || I.b == B::Mul || I.b == B::Eq || I.b == B::Ne))
          std::swap(x, y);
        bool yimm = is_c(y);
        const bool inreg_dst = hascolor(I.dst);
        const char *dr = inreg_dst ? rname(I.dst) : "rax";
        switch (I.b) {
        case B::Add:
        case B::Sub:
        case B::Mul:
        case B::And:
        case B::Or:
        case B::Xor: {
          const char *op = I.b == B::Add    ? "addq"
                           : I.b == B::Sub  ? "subq"
                           : I.b == B::Mul  ? "imulq"
                           : I.b == B::And  ? "andq"
                           : I.b == B::Or   ? "orq"
                                            : "xorq";
          const bool comm = (I.b != B::Sub);
          if (!yimm && hascolor(y) && !std::strcmp(rname(y), dr)) {
            // TWO-ADDRESS HAZARD: y is coalesced into the destination
            // register (legal: y dies here). Moving x into dr first would
            // destroy y before the op reads it.
            if (comm) { // dst = x OP y == y OP x: accumulate onto y in place
              std::snprintf(buf, sizeof buf, "  %s %s, %%%s\n", op,
                            loc(x).c_str(), dr);
              s += buf;
            } else { // Sub: stash y, move x in, subtract the stash
              std::snprintf(buf, sizeof buf, "  movq %%%s, %%rcx\n", dr);
              s += buf;
              src_into(dr, x);
              s += "  subq %rcx, %" + std::string(dr) + "\n";
            }
            break;
          }
          src_into(dr, x); // dst register holds x (mov elided when equal)
          if (yimm && fits32(cv(y))) {
            std::snprintf(buf, sizeof buf, "  %s $%lld, %%%s\n", op,
                          (long long)cv(y), dr);
            s += buf;
          } else if (yimm) {
            ld("rcx", y);
            std::snprintf(buf, sizeof buf, "  %s %%rcx, %%%s\n", op, dr);
            s += buf;
          } else {
            std::snprintf(buf, sizeof buf, "  %s %s, %%%s\n", op,
                          loc(y).c_str(), dr);
            s += buf;
          }
          break;
        }
        case B::Shl:
        case B::LShr:
        case B::AShr: {
          // x86 masks shift counts to 6 bits itself -- same semantics as the
          // &63 the other runners apply, so const and variable counts agree
          const char *op = I.b == B::Shl    ? "salq"
                           : I.b == B::LShr ? "shrq"
                                            : "sarq";
          src_into(dr, x);
          if (yimm) {
            std::snprintf(buf, sizeof buf, "  %s $%llu, %%%s\n", op,
                          (unsigned long long)((uint64_t)cv(y) & 63), dr);
            s += buf;
          } else {
            ld("rcx", y);
            std::snprintf(buf, sizeof buf, "  %s %%cl, %%%s\n", op, dr);
            s += buf;
          }
          break;
        }
        case B::MulHi: { // unsigned high half: rdx:rax = rax * rcx
          ld("rax", x);
          ld("rcx", y);
          s += "  mulq %rcx\n";
          if (inreg_dst) {
            std::snprintf(buf, sizeof buf, "  movq %%rdx, %%%s\n", dr);
            s += buf;
          } else {
            s += "  movq %rdx, %rax\n";
          }
          break;
        }
        case B::Div:
        case B::Mod: {
          bool bc = yimm;            // divisor known?
          val_t bv = bc ? cv(y) : 0;
          bool ac = is_c(x);         // dividend known?
          val_t av = ac ? cv(x) : 0;
          bool always = bc && (bv == 0 || (bv == -1 && ac && av == INT64_MIN));
          if (always) {
            s += "  jmp mjz_trap\n";
            break;
          }
          if (bc && bv == -1) { // x / -1 == -x (trap only on INT64_MIN)
            src_into("rax", x);
            s += "  movabsq $0x8000000000000000, %rdx\n";
            s += "  cmpq %rdx, %rax\n  je mjz_trap\n";
            s += I.b == B::Div ? "  negq %rax\n" : "  xorl %eax, %eax\n";
            break;
          }
          src_into("rax", x);
          if (bc) { // known safe divisor: no guard at all
            ld("rcx", y);
          } else { // unknown divisor: full guard, then divide
            ld("rcx", y);
            s += "  testq %rcx, %rcx\n  jz mjz_trap\n";
            s += "  movabsq $0x8000000000000000, %rdx\n";
            s += "  cmpq %rdx, %rax\n  jne 1f\n";
            s += "  cmpq $-1, %rcx\n  je mjz_trap\n";
            s += "1:\n";
          }
          s += "  cqto\n  idivq %rcx\n";
          if (I.b == B::Mod) s += "  movq %rdx, %rax\n";
          st_rax(I.dst); // register in place, spill slot otherwise
          break;
        }
        default: { // comparisons
          src_into("rax", x);
          if (yimm && fits32(cv(y))) {
            std::snprintf(buf, sizeof buf, "  cmpq $%lld, %%rax\n",
                          (long long)cv(y));
            s += buf;
          } else {
            ld("rcx", y);
            s += "  cmpq %rcx, %rax\n";
          }
          const char *set = I.b == B::Lt   ? "setl"
                            : I.b == B::Le ? "setle"
                            : I.b == B::Gt ? "setg"
                            : I.b == B::Ge ? "setge"
                            : I.b == B::Eq ? "sete"
                                           : "setne";
          if (inreg_dst) {
            std::snprintf(buf, sizeof buf, "  %s %%dl\n  movzbq %%dl, %%%s\n",
                          set, dr);
            s += buf;
          } else {
            std::snprintf(buf, sizeof buf,
                          "  %s %%dl\n  movzbq %%dl, %%rax\n", set);
            s += buf;
          }
          break;
        }
        }
        if (!inreg_dst && I.b != B::Div && I.b != B::Mod)
          s += "  movq %rax, " + slot(I.dst) + "\n";
        break;
      }
      case K::Br:
        if (I.t != P.next[b]) // else: fall through
          s += "  jmp " + lbl(I.t) + "\n";
        break;
      case K::Cbr: {
        // emit a compare + conditional jump (fused when possible)
        auto emit_cmp = [&]() -> B {
          uint32_t x = I.a, y = I.b2;
          B op = B::Ne; // plain truth test: jne = jump when cond != 0
          if (P.fuse[i] != NONE) {
            const Instr &D = fn.code[P.fuse[i]];
            x = D.a;
            y = D.b2;
            op = D.b;
            if (is_c(x) && !is_c(y) && (op == B::Eq || op == B::Ne))
              std::swap(x, y); // commutative: immediate on the right
          }
          if (P.fuse[i] == NONE) { // truth test on the condition value
            if (hascolor(x)) {
              std::snprintf(buf, sizeof buf, "  testq %%%s, %%%s\n",
                            rname(x), rname(x));
              s += buf;
            } else {
              ld("rax", x);
              s += "  testq %rax, %rax\n";
            }
            return op;
          }
          if (hascolor(x)) { // compare in the operand's own register
            if (is_c(y) && fits32(cv(y))) {
              std::snprintf(buf, sizeof buf, "  cmpq $%lld, %%%s\n",
                            (long long)cv(y), rname(x));
              s += buf;
            } else if (is_c(y)) {
              ld("rcx", y);
              std::snprintf(buf, sizeof buf, "  cmpq %%rcx, %%%s\n",
                            rname(x));
              s += buf;
            } else {
              std::snprintf(buf, sizeof buf, "  cmpq %s, %%%s\n",
                            loc(y).c_str(), rname(x));
              s += buf;
            }
            return op;
          }
          ld("rax", x);
          if (is_c(y) && fits32(cv(y))) {
            std::snprintf(buf, sizeof buf, "  cmpq $%lld, %%rax\n",
                          (long long)cv(y));
            s += buf;
          } else {
            ld("rcx", y);
            s += "  cmpq %rcx, %rax\n";
          }
          return op;
        };
        if (P.fuse[i] == NONE && is_c(I.a)) { // constant condition
          uint32_t tgt = (cv(I.a) != 0) ? I.t : I.f;
          if (tgt != P.next[b]) s += "  jmp " + lbl(tgt) + "\n";
          break;
        }
        B op = emit_cmp();
        const char *jcc = jcc_of(op);
        if (I.f == P.next[b]) // false arm falls through
          s += std::string("  ") + jcc + " " + lbl(I.t) + "\n";
        else if (I.t == P.next[b]) // true arm falls through: invert
          s += std::string("  ") + jcc_inv(jcc) + " " + lbl(I.f) + "\n";
        else {
          s += std::string("  ") + jcc + " " + lbl(I.t) + "\n";
          s += "  jmp " + lbl(I.f) + "\n";
        }
        break;
      }
      case K::Print:
        ld("rsi", I.a);
        s += "  leaq .Lfmt(%rip), %rdi\n";
        s += "  xorl %eax, %eax\n  call printf\n";
        break;
      case K::Alloc:
        ld("rdi", I.a);
        s += "  call mjz_alloc\n";
        st_rax(I.dst);
        break;
      case K::Load:
        ld("rdi", I.a);
        ld("rsi", I.b2);
        s += "  call mjz_load\n";
        st_rax(I.dst);
        break;
      case K::Store:
        ld("rdi", I.a);
        ld("rsi", I.b2);
        ld("rdx", I.c2);
        s += "  call mjz_store\n";
        break;
      case K::Len:
        ld("rdi", I.a);
        s += "  call mjz_len\n";
        st_rax(I.dst);
        break;
      case K::Input:
        s += "  call mjz_input\n";
        st_rax(I.dst);
        break;
      case K::Call: { // SysV: up to six integer registers
        static const char *argregs[6] = {"rdi", "rsi", "rdx",
                                         "rcx", "r8", "r9"};
        for (size_t k = 0; k < I.args.size() && k < 6; ++k)
          ld(argregs[k], I.args[k]);
        s += "  call mjz_f" + std::to_string((unsigned)I.c) + "\n";
        st_rax(I.dst);
        break;
      }
      case K::Ret:
        if (I.a != NONE) ld("rax", I.a);
        else s += "  xorl %eax, %eax\n";
        if (multi) s += "  decq mjz_depth(%rip)\n";
        if (frame) {
          std::snprintf(buf, sizeof buf, "  addq $%llu, %%rsp\n",
                        (unsigned long long)frame);
          s += buf;
        }
        for (int32_t c = 4; c >= 0; --c)
          if (RP.usedmask & ((uint32_t)1 << c))
            s += std::string("  popq %") + RREGS[c] + "\n";
        s += "  popq %rbp\n  ret\n";
        break;
      case K::Exit:
        saw_exit = true;
        s += "  xorl %edi, %edi\n  call exit\n";
        break;
      }
    }
  }
  if (!saw_exit) s += "  xorl %edi, %edi\n  call exit\n";
  return s;
}

// whole-program asm: callee bodies, main, shared trap path + helpers + data.
static std::string emit_x64(const std::vector<Fn> &fns,
                            const std::vector<std::string_view> &names) {
  (void)names;
  std::string s{};
  char buf[160];
  (void)buf;
  // helpers/arena are emitted only when the program uses the feature
  bool use_arr = false, use_in = false;
  for (const Fn &fn : fns)
    for (const Instr &I : fn.code) {
      if (I.k == K::Alloc || I.k == K::Load || I.k == K::Store ||
          I.k == K::Len)
        use_arr = true;
      else if (I.k == K::Input)
        use_in = true;
    }
  const bool multi = fns.size() > 1;
  s += "# generated by minijz\n  .text\n";
  for (uint32_t f = 1; f < fns.size(); ++f) // callees first, main last
    s += emit_x64_fn(fns[f], f, false, multi);
  s += emit_x64_fn(fns[0], 0, true, multi);
  // shared trap path — self-aligning, so it is correct whether reached via
  // `call` (helpers) or via `jz`/`jmp` (main's div/mod and OOB checks)
  s += "mjz_trap:\n  pushq %rbp\n  movq %rsp, %rbp\n  andq $-16, %rsp\n";
  s += "  leaq .Ltraps(%rip), %rdi\n  call puts\n";
  s += "  movl $1, %edi\n  call exit\n";
  if (use_arr) {
    // arena helpers; the bump state must mirror the C/interp backends
    // exactly (CAP = 2^21 slots; handles are element indices)
    s += "# int64 mjz_alloc(int64 n): bump-allocate n slots, return handle\n";
    s += "mjz_alloc:\n";
    s += "  testq %rdi, %rdi\n  js mjz_trap\n";        // n < 0
    s += "  movabsq $2097152, %rdx\n  cmpq %rdx, %rdi\n  jg mjz_trap\n";
    s += "  subq %rdi, %rdx\n";                        // CAP - n (no ovf)
    s += "  movq mjz_atop(%rip), %rax\n";              // h = top
    s += "  cmpq %rdx, %rax\n  jg mjz_trap\n";        // top > CAP - n
    s += "  leaq mjz_alen(%rip), %rdx\n";
    s += "  movq %rdi, (%rdx,%rax,8)\n";               // len[h] = n
    s += "  addq %rax, %rdi\n  movq %rdi, mjz_atop(%rip)\n"; // top = h+n
    s += "  ret\n";
    s += "# int64 mjz_len(int64 h): 0 for any non-handle\n";
    s += "mjz_len:\n";
    s += "  testq %rdi, %rdi\n  js .Llen0\n";
    s += "  movabsq $2097152, %rdx\n  cmpq %rdx, %rdi\n  jg .Llen0\n";
    s += "  leaq mjz_alen(%rip), %rdx\n  movq (%rdx,%rdi,8), %rax\n  ret\n";
    s += ".Llen0:\n  xorl %eax, %eax\n  ret\n";
    s += "# int64 mjz_load(int64 h, int64 i): TRAPs unless 0 <= i < len[h]\n";
    s += "mjz_load:\n";
    s += "  testq %rdi, %rdi\n  js mjz_trap\n";
    s += "  movabsq $2097152, %rdx\n  cmpq %rdx, %rdi\n  jg mjz_trap\n";
    s += "  testq %rsi, %rsi\n  js mjz_trap\n";
    s += "  leaq mjz_alen(%rip), %rdx\n  movq (%rdx,%rdi,8), %rcx\n";
    s += "  cmpq %rcx, %rsi\n  jge mjz_trap\n";
    s += "  leaq mjz_arena(%rip), %rdx\n  addq %rdi, %rsi\n";
    s += "  movq (%rdx,%rsi,8), %rax\n  ret\n";
    s += "# void mjz_store(int64 h, int64 i, int64 v)\n";
    s += "mjz_store:\n";
    s += "  testq %rdi, %rdi\n  js mjz_trap\n";
    s += "  movabsq $2097152, %r8\n  cmpq %r8, %rdi\n  jg mjz_trap\n";
    s += "  testq %rsi, %rsi\n  js mjz_trap\n";
    s += "  leaq mjz_alen(%rip), %r9\n  movq (%r9,%rdi,8), %rcx\n";
    s += "  cmpq %rcx, %rsi\n  jge mjz_trap\n";
    s += "  leaq mjz_arena(%rip), %r9\n  addq %rdi, %rsi\n";
    s += "  movq %rdx, (%r9,%rsi,8)\n  ret\n";
  }
  if (use_in) {
    s += "# int64 mjz_input(void): scanf into stack scratch; 0 on EOF/failure\n";
    s += "mjz_input:\n";
    s += "  pushq %rbx\n  subq $16, %rsp\n";
    s += "  leaq .Lscan(%rip), %rdi\n";
    s += "  movq $0, (%rsp)\n  movq %rsp, %rsi\n";
    s += "  xorl %eax, %eax\n  call scanf\n";
    s += "  movq (%rsp), %rax\n";
    s += "  addq $16, %rsp\n  popq %rbx\n  ret\n";
  }
  s += "  .section .rodata\n";
  s += ".Lfmt:\n  .string \"%lld\\n\"\n";
  s += ".Ltraps:\n  .string \"TRAP\"\n";
  if (use_in) s += ".Lscan:\n  .string \"%lld\"\n";
  if (use_arr) { // (CAP + 1) * 8 bytes each; handles may equal CAP
    s += "  .section .bss\n  .balign 32\n";
    s += "mjz_arena:\n  .zero 16777224\n";
    s += "mjz_alen:\n  .zero 16777224\n";
    s += "mjz_atop:\n  .zero 8\n";
  }
  if (multi) { // recursion depth counter (shared by every non-main fn)
    s += "  .section .bss\n  .balign 8\nmjz_depth:\n  .zero 8\n";
  }
  return s;
}

// ---- 13.5c. bytecode listing (disassembly) -----------------------------------
static std::string emit_bc_listing(const std::vector<uint8_t> &c) {
  std::string s{};
  char buf[160];
  if (c.size() < 4) return s;
  const uint32_t nfns = bc_rd_u32(c, 0);
  std::snprintf(buf, sizeof buf, "; %zu bytes, %u fn(s)\n",
                (size_t)c.size(), (unsigned)nfns);
  s += buf;
  for (uint32_t f = 0; f < nfns && c.size() >= 4 + (size_t)8 * (f + 1); ++f) {
    uint32_t fo = bc_rd_u32(c, 4 + (size_t)8 * f);
    uint32_t sl = bc_rd_u32(c, 4 + (size_t)8 * f + 4);
    uint32_t eo = (f + 1 < nfns) ? bc_rd_u32(c, 4 + (size_t)8 * (f + 1))
                                 : (uint32_t)c.size();
    std::snprintf(buf, sizeof buf, "; fn %u: off=%u slots=%u\n", f, fo, sl);
    s += buf;
    size_t pc = fo;
    while (pc < eo && pc < c.size()) {
      Bc op = (Bc)c[pc];
      std::snprintf(buf, sizeof buf, "%6zu  %-6s", pc, bc_name(op));
      s += buf;
      ++pc;
      switch (op) {
      case Bc::Push:
        std::snprintf(buf, sizeof buf, " %lld\n",
                      (long long)bc_rd_i64(c, pc));
        s += buf;
        pc += 8;
        break;
      case Bc::Load:
      case Bc::Store:
        std::snprintf(buf, sizeof buf, " v%u\n",
                      (unsigned)bc_rd_u32(c, pc));
        s += buf;
        pc += 4;
        break;
      case Bc::Jmp:
      case Bc::Jz:
        std::snprintf(buf, sizeof buf, " %u\n",
                      (unsigned)bc_rd_u32(c, pc));
        s += buf;
        pc += 4;
        break;
      case Bc::Call:
        std::snprintf(buf, sizeof buf, " f%u (%u args)\n",
                      (unsigned)bc_rd_u32(c, pc),
                      (unsigned)bc_rd_u32(c, pc + 4));
        s += buf;
        pc += 8;
        break;
      default: s += "\n"; break;
      }
    }
  }
  return s;
}

// ---- 13.5d. native build & run harness ----------------------------------------
static bool write_file(const std::string &path, const std::string &data) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << data;
  return bool(f);
}
static bool write_file_bytes(const std::string &path,
                             const std::vector<uint8_t> &data) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write((const char *)data.data(), (std::streamsize)data.size());
  return bool(f);
}
static std::string slurp_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}
static std::string find_cc() {
  auto has = [](const char *cc) {
    std::string cmd = std::string("command -v ") + cc + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
  };
  if (has("clang++")) return "clang++"; // preferred: user-requested driver
  if (has("g++")) return "g++";
  if (has("cc")) return "cc";
  return "";
}
// parse whitespace-separated integers; stop at the first non-number (the
// same place scanf in the native builds gives up, so the tape stays in sync)
static std::vector<val_t> parse_inputs(const std::string &text) {
  std::vector<val_t> out{};
  const char *p = text.c_str();
  char *end{};
  for (;;) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (!*p) break;
    long long v = std::strtoll(p, &end, 10);
    if (end == p) break;
    out.push_back((val_t)v);
    p = end;
  }
  return out;
}
// run `cmd`, capturing stdout into `out`; stdin redirected from `in_path`
// (empty = inherit); returns the raw system() status
static int run_capture(const std::string &cmd, std::string &out,
                       const std::string &cap_path,
                       const std::string &in_path = {}) {
  std::string full = cmd;
  if (!in_path.empty()) full += " < " + in_path;
  full += " > " + cap_path + " 2>/dev/null";
  int rc = std::system(full.c_str());
  out = slurp_file(cap_path);
  return rc;
}
static double now_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ============================================================================
// 14. curated demos
// ============================================================================
struct Demo {
  const char *name;
  const char *src;
  bool show_ir;
  const val_t *ins{}; // stdin tape for this demo (nullptr: empty tape)
  size_t nins{};
};

static const val_t DEMO_INS_ECHO[] = {19, 23};
static const val_t DEMO_INS_CANON[] = {13, 7};
static const val_t DEMO_INS_STATS[] = {5, 3, 8, 1, 9, 9, 2, 0, 7, 6};
static const val_t DEMO_INS_DIVMAGIC[] = {1234567890123456789, -987654321098765432};
static const val_t DEMO_INS_LCFRT[] = {1000000};

static const Demo demos[] = {
  {"sum_1_to_10",
   "let s = 0;\nlet i = 1;\nwhile (i <= 10) { s = s + i; i = i + 1; }\nprint s;\n",
   true},
  {"fibonacci",
   "let a = 0;\nlet b = 1;\nlet n = 10;\nwhile (n > 0) {\n"
   "  let t = a + b;\n  a = b;\n  b = t;\n  n = n - 1;\n}\nprint a;\n",
   false},
  {"gcd",
   "let x = 252;\nlet y = 105;\nwhile (y != 0) {\n  let r = x % y;\n"
   "  x = y;\n  y = r;\n}\nprint x;\n",
   false},
  {"short_circuit_div_guard",
   "let a = 100;\nlet b = 0;\nif (b != 0 && a / b > 2) { print 1; } else { print 0; }\n",
   false},
  {"trap_is_preserved",
   "let a = 1;\nlet b = 0;\nprint a / b;\n",
   false},
  {"licm_demo",
   // x is loop-derived (not const-foldable), so x*x in the second loop is a
   // genuine loop-invariant computation for LICM to hoist into the preheader
   "let i = 0;\nlet x = 0;\nwhile (i < 5) { x = x + 3; i = i + 1; }\n"
   "let s = 0;\nlet j = 0;\nwhile (j < 10) { s = s + x * x; j = j + 1; }\n"
   "print s;\nprint x;\n",
   true},
  {"nested_loops",
   "let i = 0;\nlet total = 0;\nwhile (i < 3) {\n  let j = 0;\n"
   "  while (j < 4) { total = total + i * j; j = j + 1; }\n  i = i + 1;\n}\n"
   "print total;\n",
   false},
  {"const_fold",
   "print (2 + 3) * 4;\nif (1 == 1) { print 5; }\nif (0) { print 99; }\n",
   true},
  {"cse_demo",
   // i*7 is computed for the print AND for the update: the dominator-scoped
   // CSE must collapse them; the copy-prop cleans the phi chains after it
   "let x = 0;\nlet i = 0;\nwhile (i < 3) { x = x + i * 7; print i * 7; "
   "i = i + 1; }\nprint x;\n",
   true},
  {"canon_cse",
   // canonicalization demo: y*x is rewritten to x*y and x>y to y<x, so
   // both pairs unify and the CSE pass collapses them (see the report)
   "let x = input();\nlet y = input();\n"
   "let u = x * y;\nlet v = y * x;\n"
   "let p = x > y;\nlet q = y < x;\n"
   "print u == v;\nprint p == q;\nprint u + v;\n",
   true, DEMO_INS_CANON, 2},
  {"array_sum",
   "let a = array(5);\nlet i = 0;\nwhile (i < 5) { a[i] = i * i; i = i + 1; }\n"
   "let s = 0;\ni = 0;\nwhile (i < len(a)) { s = s + a[i]; i = i + 1; }\n"
   "print s;\nprint len(a);\n",
   true},
  {"array_2d",
   // handles are values: an array of handles IS a 2-D array
   "let m = array(3);\nlet i = 0;\nwhile (i < 3) { m[i] = array(3); i = i + 1; }\n"
   "i = 0;\nwhile (i < 3) {\n  let j = 0;\n"
   "  while (j < 3) { m[i][j] = i * 3 + j; j = j + 1; }\n  i = i + 1;\n}\n"
   "print m[2][1];\nprint m[0][0] + m[1][1] + m[2][2];\n",
   true},
  {"array_alias",
   // let b = a copies the HANDLE: both names see the same memory
   "let a = array(3);\na[0] = 7;\nlet b = a;\nb[0] = b[0] + 1;\n"
   "print a[0];\nprint a == b;\n",
   false},
  {"array_oob_trap",
   "let a = array(3);\nprint a[2];\nprint a[3];\n", // second read: TRAP
   false},
  {"array_licm",
   // len(a) never changes, but it is arena state: must NOT be hoisted or
   // CSE'd with the later read either — both stay put and agree
   "let a = array(4);\nlet s = 0;\nlet i = 0;\n"
   "while (i < len(a)) { s = s + len(a); i = i + 1; }\nprint s;\n",
   true},
  {"echo_input",
   "let x = input();\nlet y = input();\nprint x + y;\nprint x * y;\n",
   false, DEMO_INS_ECHO, 2},
  {"input_stats",
   // read 10 numbers, print sum / max / count of zeroes
   "let s = 0;\nlet mx = 0;\nlet z = 0;\nlet i = 0;\n"
   "while (i < 10) {\n  let v = input();\n  s = s + v;\n"
   "  if (i == 0 || v > mx) { mx = v; }\n  if (v == 0) { z = z + 1; }\n"
   "  i = i + 1;\n}\nprint s;\nprint mx;\nprint z;\n",
   true, DEMO_INS_STATS, 10},
  {"dead_branch",
   "let x = 1;\nif (0) { x = 2; print 111; }\nprint x;\n",
   false},
  {"if_else_chain",
   "let x = 42;\nif (x < 10) { print 1; } else { if (x < 50) { print 2; } else { print 3; } }\n",
   false},
  // ---- functions: calls, recursion, SCC mutual recursion, inlining ----
  {"fn_basics",
   "fn add(a, b) { return a + b; }\nfn mul(a, b) { return a * b; }\n"
   "print add(2, 3);\nprint mul(add(1, 1), add(2, 2));\n",
   true},
  {"fn_recursion",
   "fn fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); }\n"
   "fn fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }\n"
   "print fact(6);\nprint fib(12);\n",
   true},
  {"fn_mutual_recursion", // a 2-member call-graph SCC: never inlined
   "fn is_even(n) { if (n == 0) { return 1; } return is_odd(n - 1); }\n"
   "fn is_odd(n) { if (n == 0) { return 0; } return is_even(n - 1); }\n"
   "print is_even(10);\nprint is_odd(7);\nprint is_even(7);\n",
   true},
  {"fn_inline_const", // inlined, then folded at compile time
   "fn double(x) { return x * 2; }\nfn inc(x) { return x + 1; }\n"
   "print double(21);\nprint inc(inc(inc(0)));\n",
   true},
  {"fn_arrays", // arenas are global: allocation order is call-ordered
   "fn fill_sum(n) {\n  let a = array(n);\n  let i = 0;\n"
   "  while (i < n) { a[i] = i * i; i = i + 1; }\n"
   "  let s = 0;\n  i = 0;\n  while (i < n) { s = s + a[i]; i = i + 1; }\n"
   "  return s;\n}\nprint fill_sum(4);\nprint fill_sum(5);\n",
   true},
  {"fn_multi_return",
   "fn sign(x) {\n  if (x < 0) { return 0 - 1; }\n"
   "  if (x == 0) { return 0; }\n  return 1;\n}\n"
   "print sign(0 - 9);\nprint sign(0);\nprint sign(42);\n",
   true},
  // ---- loop unrolling: constant trip counts vanish at compile time ----
  {"unroll_const_trip", // fully unrolled: s folds to 6, i folds to 4
   "let s = 0;\nlet i = 0;\nwhile (i < 4) { s = s + i; i = i + 1; }\n"
   "print s;\nprint i;\n",
   true},
  {"unroll_nested",
   "let t = 0;\nlet i = 0;\nwhile (i < 3) {\n  let j = 0;\n"
   "  while (j < 3) { t = t + i * j; j = j + 1; }\n  i = i + 1;\n}\n"
   "print t;\n",
   true},
  {"unroll_runtime_bound", // variable bound: NOT unrolled, still correct
   "let n = input();\nlet s = 0;\nlet i = 0;\n"
   "while (i < n) { s = s + i * 2; i = i + 1; }\nprint s;\n",
   true, DEMO_INS_CANON, 2},
  // ---- reassociation: canonical linear forms inside a block ----
  {"reassoc_chain", // the body chain collapses to s += 45*i - 45
   "let s = 0;\nlet i = 0;\nwhile (i < 6) {\n"
   "  s = s + i * 1 - 1;\n  s = s + i * 2 - 2;\n  s = s + i * 3 - 3;\n"
   "  i = i + 1;\n}\nprint s;\n",
   true},
  // ---- closed-form constant-trip loops: the loop disappears entirely ----
  {"lcf_additive_series", // 1M iterations -> one Add at runtime (folds to a
   // constant here because s0/i0 are const): arithmetic series, a = 1
   "let s = 0;\nlet i = 0;\nwhile (i < 1000000) { s = s + i * 9 - 7; "
   "i = i + 1; }\nprint s;\n",
   true},
  {"lcf_geometric_series", // x = x*3 + 1: geometric series, a = 3 (wraps!)
   "let x = 2;\nlet k = 0;\nwhile (k < 40) { x = x * 3 + 1; k = k + 1; }\n"
   "print x;\n",
   true},
  {"lcf_runtime_init", // closed form with a RUNTIME initial value: the loop
   // still vanishes; what remains is K1*x0 + K0 on the input value
   "let x = input();\nlet k = 0;\n"
   "while (k < 1000) { x = x * 2 + 3; k = k + 1; }\nprint x;\n",
   true, DEMO_INS_ECHO, 1},
  {"lcf_rt_dynamic_bound", // RUNTIME trip count: the loop is replaced by a
   // guarded closed form; the fallback path keeps the original loop for the
   // (astronomically rare) signed-wrap case
   "let n = input();\nlet s = 0;\nlet i = 0;\n"
   "while (i < n) { s = s + i * 9 - 7; i = i + 1; }\nprint s;\nprint i;\n",
   true, DEMO_INS_LCFRT, 1},
  {"lcf_rt_le", // <= variant: N = floor((B-i0)/d) + 1
   "let n = input();\nlet s = 0;\nlet i = 1;\n"
   "while (i <= n) { s = s + i; i = i + 1; }\nprint s;\nprint i;\n",
   true, DEMO_INS_LCFRT, 1},
  {"lcf_rt_edge_max", // guard math at the very top of the signed range
   "let i = 9223372036854775800;\nlet s = 0;\n"
   "while (i < 9223372036854775806) { s = s + 1; i = i + 7; }\n"
   "print s;\nprint i;\n",
   true},
  {"lcf_sim_fallback", // not affine (x*x): exact compile-time simulation
   "let x = 3;\nlet k = 0;\n"
   "while (k < 20) { x = x * x % 1000003 + 1; k = k + 1; }\nprint x;\n",
   true},
  {"lcf_nested", // inner folds first (closed form), then the outer does too
   "let s = 0;\nlet i = 0;\nwhile (i < 200) {\n  let j = 0;\n"
   "  while (j < 300) { s = s + i * j - j; j = j + 1; }\n  i = i + 1;\n}\n"
   "print s;\n",
   true},
  // ---- division strength reduction: /constant -> mulhi + shifts ----
  {"divmagic", // x/7, x%7, x/-12, x%12, x/64 (pow2), negatives included;
   //            every backend must agree with the oracle bit-for-bit
   "let x = input();\nlet y = input();\n"
   "print x / 7;\nprint x % 7;\nprint x / -12;\nprint x % 12;\n"
   "print x / 64;\nprint x % 64;\nprint y / 1000000007;\nprint y % 998244353;\n"
   "print (0 - x) / 7;\nprint (0 - x) % 7;\nprint x / -64;\n",
   true, DEMO_INS_DIVMAGIC, 2},
  // ---- the tour: every optimizer tier in one program ----------------------
  {"tour", // sum-of-evens folds (LCF additive), CSE folds x*x, the &&
   // short-circuit folds without any div-trap check (range/alias: d==0),
   // (2+3)*4 const-folds, and Euclid's loop folds via the GENERAL
   // simulator (no induction variable: `h != 0` + g%h) to the constant 21.
   // After destruction + slot allocation the whole program is five
   // constant prints on a single branchless path.
   "let s = 0;\nlet i = 1;\nwhile (i <= 10) {\n"
   "  if (i % 2 == 0) { s = s + i; } else { s = s + 0; }\n  i = i + 1;\n}\n"
   "print s;\n"
   "let x = 3;\nprint x * x + x * x;\n"
   "let d = 0;\nif (d != 0 && 100 / d > 1) { print 1; } else { print 2; }\n"
   "print (2 + 3) * 4;\n"
   "let g = 252;\nlet h = 105;\n"
   "while (h != 0) { let r = g % h; g = h; h = r; }\nprint g;\n",
   true},
  // ---- break / continue -------------------------------------------------
  {"break_continue", // while(1) + break: exits only from a non-header
   // block, so no loop pass may touch it; continue re-tests the header
   "let s = 0;\nlet i = 0;\nwhile (1) {\n  i = i + 1;\n"
   "  if (i > 10) { break; }\n  if (i % 2 == 1) { continue; }\n  s = s + i;\n}\n"
   "print s;\nprint i;\n",
   true},
  {"break_continue_nested",
   "let t = 0;\nlet i = 0;\nwhile (i < 5) {\n  i = i + 1;\n"
   "  let j = 0;\n  while (j < 5) {\n    j = j + 1;\n"
   "    if (j == 2) { continue; }\n    if (j == 4) { break; }\n"
   "    t = t + i * j;\n  }\n}\nprint t;\n",
   true},
  // ---- array memory-to-register promotion (mem2reg) ---------------------
  {"array_lift", // single handle, constant in-range indices: the array
   // elements become SSA values (slot phis where control flow merges); the
   // Alloc stays in the arena so handle parity is preserved
   "let a = array(4);\nlet i = 0;\nwhile (i < 4) {\n"
   "  if (i % 2 == 0) { a[0] = a[0] + i; } else { a[1] = a[1] + i; }\n"
   "  i = i + 1;\n}\n"
   "print a[0] + a[1] + a[2] + a[3];\nprint len(a);\n",
   true},
  {"array_dead_stores", // never read: every store is dead weight. The
   // RUNTIME index needs the range analysis (trapelim) to prove it inside
   // [0,1000) first; round 2 of the tier-1 fixpoint then deletes the stores
   "let a = array(1000);\nlet i = 0;\n"
   "while (i < 1000) { a[i] = i; i = i + 1; }\nprint 7;\n",
   true},
};

static void run_demos() {
  std::printf("=== curated programs: compile | analyze | optimize | execute ===\n");
  for (const Demo &d : demos) {
    std::printf("--- %s ---\n", d.name);
    Program P = compile(d.src);
    if (!P.ok) {
      std::printf("  COMPILE ERROR: %s (at %u)\n", P.error ? P.error : "?",
                  (unsigned)P.errpos);
      CHECK(false, "curated program must compile");
      continue;
    }
    {
      OptStats os = sum_ostats(P);
      size_t ssa_i = 0, opt_i = 0, dst_i = 0;
      uint32_t phis = 0, de_c = 0, dz_c = 0, dse = 0, dcy = 0, dfu = 0;
      for (const FnUnit &U : P.fns) {
        ssa_i += U.ssa.code.size();
        opt_i += U.opt.code.size();
        dst_i += U.destroyed.code.size();
        phis += U.sstats.phis;
        de_c += U.dstats.edges_split;
        dz_c += U.dstats.copies;
        dse += U.dstats.self_copies;
        dcy += U.dstats.cycles_broken;
        dfu += U.dstats.fused;
      }
      print_analysis(P.an_pre_main, P.fns[0].ssa, "analysis (mjz graph)");
      std::printf(
          "  fns: %zu | ssa: %zu instrs, %u phis | opt: %zu | destroyed: %zu\n"
          "  opt report: iters %u | folds %u | copies %u | copyprop %u | "
          "cse %u | canon %u+%u | brs %u | cbr== %u | dead blocks %u | "
          "splices %u | phi-degen %u | dce %u | hoisted %u | INLINED %u | "
          "UNROLLED %u | REASSOC %u(-%u) | LCF %u+%u+g%u|rt%u | DIVMAGIC %u | "
          "NOTCOMB %u | TRAP-KILL %u | MEM2REG %u | DSE %u\n"
          "  destroy: edges split %u | copies %u | self %u | cycles %u | "
          "copy-fused %u\n",
          P.fns.size(), ssa_i, (unsigned)phis, opt_i, dst_i,
          (unsigned)os.iters, (unsigned)os.folds, (unsigned)os.copies,
          (unsigned)os.copyprops, (unsigned)os.cses, (unsigned)os.canon_swaps,
          (unsigned)os.canon_cmps, (unsigned)os.brs_folded,
          (unsigned)os.cbr_same, (unsigned)os.blocks_gone,
          (unsigned)os.splices, (unsigned)os.phi_degens, (unsigned)os.dce,
          (unsigned)os.hoisted, (unsigned)os.inlines, (unsigned)os.unrolls,
          (unsigned)os.reassocs, (unsigned)os.reassoc_gone,
          (unsigned)os.lcf_loops, (unsigned)os.lcf_sims,
          (unsigned)os.lcf_gensims, (unsigned)os.lcf_rt, (unsigned)os.divmagics,
          (unsigned)os.notcombs, (unsigned)os.traps_killed,
          (unsigned)os.mem2reg_lifts, (unsigned)os.mem2reg_dses,
          (unsigned)de_c, (unsigned)dz_c, (unsigned)dse, (unsigned)dcy,
          (unsigned)dfu);
    }
    std::vector<val_t> ins{};
    if (d.ins) ins.assign(d.ins, d.ins + d.nins);
    // generous limit: the closed-form demos keep a 1,000,000-iteration loop
    // in their UNOPTIMIZED code, which the oracle/plain runners must still
    // grind through step by step
    const uint64_t demo_steps = 100000000ull;
    RunOut oracle = run_ast(P.P.ast, P.P.root, demo_steps, ins);
    RunOut plain = run_prog(fns_of(P, 0), demo_steps, ins);
    RunOut optimized = run_prog(fns_of(P, 1), demo_steps, ins);
    RunOut lowered = run_prog(fns_of(P, 2), demo_steps, ins);
    std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
    RunOut vm = vm_run(bc, demo_steps, ins);
    std::printf("  oracle : ");
    print_run(oracle);
    std::printf("  ssa    : ");
    print_run(plain);
    std::printf("  opt    : ");
    print_run(optimized);
    std::printf("  lower  : ");
    print_run(lowered);
    std::printf("  vm     : ");
    print_run(vm);
    CHECK(same_run(oracle, plain), "unoptimized SSA must match oracle");
    CHECK(same_run(oracle, optimized), "optimized SSA must match oracle");
    CHECK(same_run(oracle, lowered), "destroyed (phi-free) code must match");
    CHECK(same_run(oracle, vm), "bytecode VM must match");
    if (d.show_ir) {
      for (size_t f = 0; f < P.fns.size(); ++f) {
        char t1[80], t2[80];
        std::snprintf(t1, sizeof t1, "SSA (before opt) [%s]",
                      std::string(P.fn_names[f]).c_str());
        std::snprintf(t2, sizeof t2, "SSA (after opt) [%s]",
                      std::string(P.fn_names[f]).c_str());
        print_fn(P.fns[f].ssa, t1);
        print_fn(P.fns[f].opt, t2);
      }
    }
  }
}

// ============================================================================
// 15. differential fuzzer: random programs, five-way agreement
//     (oracle vs SSA vs optimized vs destroyed vs bytecode VM), now with
//     arrays (in-scope bounds known; occasional deliberate OOB traps) and
//     input() reads from a fixed per-seed tape.
// ============================================================================
struct FuzzStats {
  long cases{}, timeouts{}, traps{}, licm_hoists{}, dce_instrs{}, phis{};
  long arr_progs{}, in_progs{}, native_checked{};
  long fn_progs{}, inlined_total{}, unrolled_total{};
  long brk_progs{}, lifted{}, dead_stores{}, gensims{}, traps_killed{};
};

struct GenCtx {
  struct Arr {
    std::string name;
    int len;
  };
  struct FnG { // a generated function available for calls
    std::string name;
    int nparams;
  };
  std::mt19937 rng;
  int counter_id{};
  std::string src{};
  std::vector<std::string> vars{};
  std::vector<Arr> arrays{}; // in-scope arrays (safe bounds known here)
  std::vector<FnG> fns{};    // generated functions callable in exprs
  std::vector<val_t> ins{};  // deterministic stdin tape for input()
  bool use_input{};          // may this program call input()?
  bool has_brk_cont{};       // generated break/continue (for the report)
  int loop_depth{};          // 'break'/'continue' are only legal inside loops
  explicit GenCtx(uint32_t seed) : rng(seed) {}
  int pick(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
  }
  void gen_expr(std::string &out, int depth);
  void gen_stmts(int count, int depth);
  void gen_stmt(int depth);
  void gen_fn_defs();
};

static const char *VAR_NAMES[] = {"v", "w", "u", "z", "q", "r", "p", "m"};

// leaves: variable / in-bounds array load / len(a) / input() / constant
void GenCtx::gen_expr(std::string &out, int depth) {
  if (depth <= 0 || pick(0, 5) == 0) {
    int leaf = pick(0, 5);
    if (leaf <= 1 && !vars.empty()) {
      out += vars[pick(0, (int)vars.size() - 1)];
      return;
    }
    if (leaf == 2 && !arrays.empty()) { // in-bounds array load
      const Arr &A = arrays[pick(0, (int)arrays.size() - 1)];
      out += A.name + "[" + std::to_string(pick(0, A.len - 1)) + "]";
      return;
    }
    if (leaf == 3 && !arrays.empty()) { // len(a)
      out += "len(" + arrays[pick(0, (int)arrays.size() - 1)].name + ")";
      return;
    }
    if (leaf == 4 && use_input) {
      out += "input()";
      return;
    }
    if ((leaf == 5 || leaf == 0) && !fns.empty()) { // call a generated fn
      const FnG &F = fns[pick(0, (int)fns.size() - 1)];
      out += F.name + "(";
      for (int k = 0; k < F.nparams; ++k) {
        if (k) out += ", ";
        if (F.nparams) out += std::to_string(pick(-3, 8));
      }
      out += ")";
      return;
    }
    out += std::to_string(pick(-9, 9));
    return;
  }
  switch (pick(0, 6)) {
  case 0: out += "("; gen_expr(out, depth - 1); out += " + ";
          gen_expr(out, depth - 1); out += ")"; break;
  case 1: out += "("; gen_expr(out, depth - 1); out += " - ";
          gen_expr(out, depth - 1); out += ")"; break;
  case 2: out += "("; gen_expr(out, depth - 1); out += " * ";
          gen_expr(out, depth - 1); out += ")"; break;
  case 3: out += "("; gen_expr(out, depth - 1); out +=
           pick(0, 1) ? " < " : " > "; // both directions: canon bait
           gen_expr(out, depth - 1); out += ")"; break;
  case 4: out += "("; gen_expr(out, depth - 1); out +=
           pick(0, 1) ? " == " : " >= "; // canon: >= -> <=
           gen_expr(out, depth - 1); out += ")"; break;
  case 5: out += "-("; gen_expr(out, depth - 1); out += ")"; break;
  default: out += "("; gen_expr(out, depth - 1); out += " && ";
           gen_expr(out, depth - 1); out += ")"; break;
  }
}

// `depth` is a hard budget for compound-statement nesting: ifs and whiles
// consume one unit for their bodies, so the program tree is finite and small
// (if-arms pick(1,2) stmts: branching factor stays subcritical).
void GenCtx::gen_stmt(int depth) {
  // break/continue bait: inside loops only, always guarded by an `if` so
  // the statements after it stay reachable (the generated loops decrement
  // their counter FIRST, so a guarded `continue` cannot spin forever)
  if (loop_depth > 0 && pick(0, 11) == 0) {
    std::string c;
    gen_expr(c, 2);
    src += "if (" + c + ") { " + (pick(0, 1) ? "break" : "continue") +
           "; }\n";
    has_brk_cont = true;
    return;
  }
  // leaves: print / assignment / array store
  int r = pick(0, depth <= 0 ? 4 : 10);
  if (r <= (depth <= 0 ? 4 : 3)) {
    int leaf = pick(0, 2);
    if (leaf == 2 && !arrays.empty()) { // array store (10%: deliberate OOB)
      const Arr &A = arrays[pick(0, (int)arrays.size() - 1)];
      int i = pick(0, A.len - 1);
      if (pick(0, 9) == 0) i = A.len + pick(0, 2); // trap-parity bait
      std::string e;
      gen_expr(e, 3);
      src += A.name + "[" + std::to_string(i) + "] = " + e + ";\n";
      return;
    }
    if (leaf == 1 && !vars.empty()) { // assignment (never loop counters)
      std::vector<int> idx{};
      for (int i = 0; i < (int)vars.size(); ++i)
        if (vars[i][0] != 'c') idx.push_back(i);
      if (idx.empty()) { // only counters in scope: print instead
        std::string e;
        gen_expr(e, 3);
        src += "print " + e + ";\n";
        return;
      }
      std::string e;
      gen_expr(e, 3);
      src += vars[idx[pick(0, (int)idx.size() - 1)]] + " = " + e + ";\n";
      return;
    }
    std::string e; // print
    if (vars.empty() && arrays.empty())
      e = std::to_string(pick(-9, 9));
    else
      gen_expr(e, 3);
    src += "print " + e + ";\n";
    return;
  }
  if (r <= 5) { // if/else (block scoping: vars AND arrays die at '}')
    std::string c;
    gen_expr(c, 2);
    src += "if (" + c + ") {\n";
    {
      auto saved_vars = vars;
      auto saved_arrs = arrays;
      gen_stmts(pick(1, 2), depth - 1);
      vars = std::move(saved_vars);
      arrays = std::move(saved_arrs);
    }
    src += "} else {\n";
    {
      auto saved_vars = vars;
      auto saved_arrs = arrays;
      gen_stmts(pick(1, 2), depth - 1);
      vars = std::move(saved_vars);
      arrays = std::move(saved_arrs);
    }
    src += "}\n";
    return;
  }
  if (r == 6 && depth >= 2) { // bounded while with a fresh counter
    std::string cn = "c" + std::to_string(counter_id++);
    src += "let " + cn + " = " + std::to_string(pick(1, 5)) + ";\n";
    src += "while (" + cn + " > 0) {\n";
    src += "  " + cn + " = " + cn + " - 1;\n";
    {
      auto saved_vars = vars;
      auto saved_arrs = arrays;
      vars.push_back(cn);
      ++loop_depth;
      gen_stmts(pick(1, 2), depth - 1);
      --loop_depth;
      vars = std::move(saved_vars);
      arrays = std::move(saved_arrs);
    }
    src += "}\n";
    return;
  }
  { // let: 1/3 array declaration, else scalar
    if (pick(0, 2) == 0) {
      std::string n = "a" + std::to_string(counter_id++);
      int len = pick(1, 6);
      src += "let " + n + " = array(" + std::to_string(len) + ");\n";
      arrays.push_back({n, len});
      return;
    }
    std::string n = std::string(VAR_NAMES[pick(0, 7)]) +
                    std::to_string(counter_id++);
    std::string e;
    if (vars.empty())
      e = std::to_string(pick(-9, 9));
    else
      gen_expr(e, 3);
    src += "let " + n + " = " + e + ";\n";
    vars.push_back(n);
  }
}
void GenCtx::gen_stmts(int count, int depth) {
  for (int i = 0; i < count; ++i) gen_stmt(depth);
}

// generate 0-3 plain functions (params, body stmts, 1-2 returns) plus, with
// fixed shapes, a self-recursive and/or a mutually-recursive pair. Bodies run
// with a FRESH scope (params only); every call site uses small constant
// arguments so recursion depth stays far below the 512-frame trap.
void GenCtx::gen_fn_defs() {
  int n = pick(0, 2);
  if (pick(0, 1)) ++n;
  for (int f = 0; f < n; ++f) {
    int np = pick(0, 3);
    std::string name = "h" + std::to_string(counter_id++);
    src += "fn " + name + "(";
    std::vector<std::string> saved_vars = vars;
    std::vector<Arr> saved_arrs = arrays;
    vars.clear();
    arrays.clear();
    for (int p = 0; p < np; ++p) {
      if (p) src += ", ";
      std::string pn = "p" + std::to_string(p);
      src += pn;
      vars.push_back(pn);
    }
    src += ") {\n";
    gen_stmts(pick(1, 3), 2);
    std::string e1, e2, c;
    gen_expr(e1, 2);
    gen_expr(e2, 2);
    gen_expr(c, 2);
    if (pick(0, 1)) {
      src += "  if (" + c + ") { return " + e1 + "; }\n";
      src += "  return " + e2 + ";\n";
    } else {
      src += "  return " + e1 + ";\n";
    }
    src += "}\n";
    vars = std::move(saved_vars);
    arrays = std::move(saved_arrs);
    fns.push_back({name, np});
  }
  if (pick(0, 3)) { // self recursion (never inlined: SCC self-loop)
    src += "fn rec(n) { if (n <= 0) { return 0; } "
           "return rec(n - 1) + n; }\n";
    fns.push_back({"rec", 1});
  }
  if (pick(0, 4)) { // mutual recursion (a 2-member SCC)
    src += "fn pe(n) { if (n <= 0) { return 1; } return po(n - 1); }\n";
    src += "fn po(n) { if (n <= 0) { return 0; } return pe(n - 1); }\n";
    fns.push_back({"pe", 1});
    fns.push_back({"po", 1});
  }
}

static GenCtx gen_program(uint32_t seed) {
  GenCtx g(seed);
  g.use_input = (seed & 1) != 0; // half the programs read stdin
  for (int i = 0; i < 16; ++i) g.ins.push_back(g.pick(-40, 40));
  g.gen_fn_defs(); // functions first (hoisting makes order irrelevant)
  g.gen_stmts(g.pick(2, 4), 4); // leading lets mostly
  g.gen_stmts(g.pick(4, 12), 4);
  return g;
}

static void run_fuzz(long n, const std::string &cc_in, long fuzz_native,
                     const std::string &out_dir) {
  std::printf("=== differential fuzz: oracle vs SSA vs optimized vs destroyed "
              "vs VM (rotating -O configs) ===\n");
  FuzzStats F{};
  long mismatches = 0;
  const std::string cc = cc_in.empty() ? find_cc() : cc_in;
  std::string ndir{};
  if (fuzz_native > 0 && !cc.empty()) {
    ndir = out_dir + "/fuzz_native";
    std::system(("mkdir -p '" + ndir + "'").c_str());
  }
  for (long seed = 1; seed <= n; ++seed) {
    GenCtx g = gen_program((uint32_t)seed);
    const std::string &src = g.src;
    if (std::getenv("MJZ_FUZZ_DUMP") && seed >= n - 2)
      std::fprintf(stderr, "=== SEED %ld ===\n%s\n", seed, src.c_str());
    // rotate the pass configuration: every run exercises a different slice
    // of the pipeline (defaults are level 3 + register allocation)
    PassCfg cfg{};
    cfg.set_level(3);
    switch (seed % 8) {
    case 1: cfg.p_lcf = false; break;      // no closed forms / simulators
    case 2: cfg.p_unroll = false; break;   // no unrolling
    case 3: cfg.p_mem2reg = false; break;  // no array promotion
    case 4: cfg.p_trapelim = false; break; // no range/alias trap proofs
    case 5: cfg.set_level(2); break;       // mid pipeline
    case 6: cfg.set_level(1); break;       // cheap tier only
    case 7: cfg.p_cse = false; cfg.p_licm = false; cfg.p_reassoc = false;
      cfg.p_divmagic = false; break;       // scalar opts off
    }
    Program P = compile(src, cfg);
    if (!P.ok) {
      std::printf("FUZZ COMPILE ERROR seed %ld: %s at %u\nsource:\n%s\n",
                  seed, P.error ? P.error : "?", (unsigned)P.errpos,
                  src.c_str());
      CHECK(false, "fuzz program must compile");
      continue;
    }
    if (src.find("array(") != std::string::npos) F.arr_progs++;
    if (P.uses_input) F.in_progs++;
    if (src.find("fn ") != std::string::npos) F.fn_progs++;
    if (g.has_brk_cont) F.brk_progs++;
    {
      OptStats os = sum_ostats(P);
      F.inlined_total += os.inlines;
      F.unrolled_total += os.unrolls;
      F.lifted += os.mem2reg_lifts;
      F.dead_stores += os.mem2reg_dses;
      F.gensims += os.lcf_gensims;
      F.traps_killed += os.traps_killed;
    }
    RunOut oracle = run_ast(P.P.ast, P.P.root, 200000, g.ins);
    RunOut plain = run_prog(fns_of(P, 0), 200000, g.ins);
    RunOut optimized = run_prog(fns_of(P, 1), 200000, g.ins);
    RunOut lowered = run_prog(fns_of(P, 2), 200000, g.ins);
    std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
    RunOut vm = vm_run(bc, 2000000, g.ins);
    F.cases++;
    F.timeouts += oracle.timeout ? 1 : 0;
    F.traps += oracle.trapped ? 1 : 0;
    F.licm_hoists += sum_ostats(P).hoisted;
    F.dce_instrs += sum_ostats(P).dce;
    for (const FnUnit &U : P.fns) F.phis += U.sstats.phis;
    bool bad = !same_run(oracle, plain) || !same_run(oracle, optimized);
    if (!oracle.timeout) { // step-limited runners only compared when finite
      bad |= !same_run(oracle, lowered);
      bad |= !same_run(oracle, vm);
    }
    if (bad) {
      ++mismatches;
      std::printf("MISMATCH seed %ld\nsource:\n%s\n", seed, src.c_str());
      std::printf("oracle : trapped=%d timeout=%d out=", oracle.trapped,
                  oracle.timeout);
      for (val_t v : oracle.out) std::printf("%lld ", (long long)v);
      std::printf("\nssa    : trapped=%d timeout=%d out=", plain.trapped,
                  plain.timeout);
      for (val_t v : plain.out) std::printf("%lld ", (long long)v);
      std::printf("\nopt    : trapped=%d timeout=%d out=", optimized.trapped,
                  optimized.timeout);
      for (val_t v : optimized.out) std::printf("%lld ", (long long)v);
      std::printf("\nlower  : trapped=%d timeout=%d out=", lowered.trapped,
                  lowered.timeout);
      for (val_t v : lowered.out) std::printf("%lld ", (long long)v);
      std::printf("\nvm     : trapped=%d timeout=%d out=", vm.trapped,
                  vm.timeout);
      for (val_t v : vm.out) std::printf("%lld ", (long long)v);
      std::printf("\n");
      CHECK(false, "fuzz: all five runners must agree");
    }
    // --- native parity sweep (--fuzz-native): build C+asm at -O0/-O2 and
    // compare their output against the oracle, tape and all. This is the
    // end-to-end gate for the emission plan (layout, immediates, fusion,
    // dropped zero-init) — any of those going wrong shows up here.
    if (fuzz_native > 0 && !cc.empty() && !oracle.timeout) {
      --fuzz_native;
      std::string stem = ndir + "/fz" + std::to_string(seed);
      std::string c_path = stem + ".c", s_path = stem + ".s";
      write_file(c_path, emit_c(fns_of(P, 2), P.fn_names));
      write_file(s_path, emit_x64(fns_of(P, 2), P.fn_names));
      std::string tape = stem + ".in";
      {
        std::string t{};
        for (val_t v : g.ins) t += std::to_string(v) + "\n";
        write_file(tape, t);
      }
      std::string ref_text = text_of(oracle);
      const char *tags[2] = {"c", "asm"};
      for (int ti = 0; ti < 2; ++ti)
        for (const char *lvl : {"0", "2"}) {
          std::string exe =
              stem + "." + tags[ti] + ".O" + lvl + ".exe";
          std::string srcp = ti ? s_path : c_path;
          std::string build =
              "timeout 60 " + cc + " -w -O" + lvl + " " + srcp + " -o " + exe;
          if (std::system(build.c_str()) != 0) {
            std::printf("NATIVE BUILD FAILED seed %ld (%s -O%s)\n", seed,
                        tags[ti], lvl);
            CHECK(false, "fuzz-native build");
            ++mismatches;
            continue;
          }
          std::string cap = stem + ".out";
          std::string out_text{};
          int rc = run_capture("timeout 20 " + exe, out_text, cap, tape);
          bool timed_out =
              (rc != 0 && (((unsigned)rc >> 8) == 124 ||
                           ((unsigned)rc >> 8) == 137));
          if (timed_out) out_text += "TIMEOUT\n";
          if (out_text != ref_text) {
            ++mismatches;
            std::printf("NATIVE MISMATCH seed %ld (%s -O%s)\nsource:\n%s\n"
                        "expected: %s\ngot     : %s\n",
                        seed, tags[ti], lvl, src.c_str(), ref_text.c_str(),
                        out_text.c_str());
            CHECK(false, "fuzz-native: native output must match oracle");
          }
        }
      F.native_checked++;
    }
  }
  std::printf("fuzz: %ld programs | mismatches: %ld | timeouts: %ld | "
              "traps: %ld\n  phis placed: %ld | instrs DCE'd: %ld | "
              "LICM hoists: %ld | programs w/ arrays: %ld | w/ input(): %ld"
              " | w/ fns: %ld | w/ break/continue: %ld\n  inlined: %ld | "
              "unrolled: %ld | arrays lifted: %ld | dead stores: %ld | "
              "general-sim folds: %ld | traps proven impossible: %ld | "
              "native-checked (C+asm x -O0/-O2): %ld\n",
              F.cases, mismatches, F.timeouts, F.traps, F.phis, F.dce_instrs,
              F.licm_hoists, F.arr_progs, F.in_progs, F.fn_progs, F.brk_progs,
              F.inlined_total, F.unrolled_total, F.lifted, F.dead_stores,
              F.gensims, F.traps_killed, F.native_checked);
  CHECK(mismatches == 0, "fuzz must be clean");
}

// ============================================================================
// 16. constexpr pipeline — the whole compiler runs at compile time
// ============================================================================
constexpr bool cx_pipeline() {
  // A program with loops, an if, short-circuit logic, phi-worthy vars,
  // ARRAYS (incl. a 2-D store through a handle chain), and input() reads:
  constexpr std::string_view src =
      "fn add2(a, b) { return a + b; }\n"
      "fn fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); }\n"
      "fn is_even(n) { if (n == 0) { return 1; } return is_odd(n - 1); }\n"
      "fn is_odd(n) { if (n == 0) { return 0; } return is_even(n - 1); }\n"
      "let s = 0;\n"
      "let i = 1;\n"
      "while (i <= 8) {\n"
      "  if (i % 2 == 0) { s = s + i; } else { s = s + 0; }\n"
      "  i = i + 1;\n"
      "}\n"
      "print s;\n"
      "let a = array(4);\n"
      "i = 0;\n"
      "while (i < 4) { a[i] = i * i; i = i + 1; }\n"
      "let t = 0;\n"
      "i = 0;\n"
      "while (i < 4) { t = t + a[i]; i = i + 1; }\n"
      "print t;\n"
      "print len(a);\n"
      "let m = array(2);\n"
      "m[0] = array(2);\n"
      "m[0][1] = 40;\n"
      "print m[0][1] + 2;\n"
      "let x = input();\n"
      "let y = input();\n"
      "print x + y;\n"
      "let d = 0;\n"
      "if (d != 0 && 100 / d > 1) { print 1; } else { print 2; }\n"
      "print 40 + 2;\n"
      "print add2(fact(4), 16);\n"
      "print is_even(8) + is_odd(8);\n"
      "let u = 0;\n"
      "let k = 0;\n"
      "while (k < 3) { u = u + k; k = k + 1; }\n"
      "print u;\n"
      "let g = 2;\n"
      "let kk = 0;\n"
      "while (kk < 30) { g = g * 3 + 1; kk = kk + 1; }\n"
      "print g;\n" // geometric-series closed form (must equal 3^30*2+S(30))
      "let dd = input();\n"
      "print dd / 7;\n"
      "print dd % 7;\n"; // divmagic lowering, runtime dividend
  std::vector<val_t> tape{19, 23, 1000003}; // the input() reads (EOF -> 0)
  Program P = compile(src);
  if (!P.ok) return false;
  if (!P.uses_input) return false;
  // the analyses really ran: one loop with nesting info from the lib
  bool loop_ok = false;
  for (uint32_t h = 0; h < P.an_pre_main.n; ++h)
    if (P.an_pre_main.inner[h] == h && P.an_pre_main.nest[h] == 0)
      loop_ok = true;
  if (!loop_ok) return false;
  RunOut oracle = run_ast(P.P.ast, P.P.root, 1000000, tape);
  RunOut plain = run_prog(fns_of(P, 0), 1000000, tape);
  RunOut opt = run_prog(fns_of(P, 1), 1000000, tape);
  RunOut lowered = run_prog(fns_of(P, 2), 1000000, tape);
  std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
  RunOut vm = vm_run(bc, 10000000, tape);
  if (!same_run(oracle, plain) || !same_run(oracle, opt)) return false;
  if (!same_run(oracle, lowered) || !same_run(oracle, vm)) return false;
  // 20 (even sum); 14 (0+1+4+9); 4 (len); 42 (m[0][1]+2); 42 (19+23);
  // 2 (short-circuit); 42 (fold); 40 (add2(fact(4),16) inlined+folded);
  // 1 (mutual recursion); 3 (unrolled loop folded to a constant);
  // 514727830236622 (geometric closed form: 3^30*2 + (3^30-1)/2);
  // 142857 / 4 (divmagic: 1000003 / 7 and % 7)
  return oracle.out.size() == 13 && oracle.out[0] == 20 &&
         oracle.out[1] == 14 && oracle.out[2] == 4 && oracle.out[3] == 42 &&
         oracle.out[4] == 42 && oracle.out[5] == 2 && oracle.out[6] == 42 &&
         oracle.out[7] == 40 && oracle.out[8] == 1 && oracle.out[9] == 3 &&
         oracle.out[10] == 514727830236622ll && oracle.out[11] == 142857 &&
         oracle.out[12] == 4;
}
// The whole pipeline — including SSA destruction and the bytecode VM — is
// constexpr. The static_assert is enabled with -DMJZ_CX_ASSERT (it needs
// generous -fconstexpr-depth / -fconstexpr-ops-limit); --selftest always
// runs the same function at runtime.
#ifdef MJZ_CX_ASSERT
static_assert(cx_pipeline(),
              "the full MiniJZ pipeline (lexer->parser->CFG->lib analyses->"
              "SSA->opt->destroy->VM) must succeed at compile time");
#endif

// ============================================================================
// 17. CLI: input commands, emission, native build&run, comparison matrix
// ============================================================================
struct Cli {
  std::string file{};
  std::string inline_src{};
  std::string cc{};          // "" = auto-detect
  std::string out_dir{"."};
  std::string stdin_path{};  // --stdin=FILE (input() tape)
  std::string stdin_text{};  // raw text of the tape (fed to native builds)
  std::vector<val_t> inputs{}; // parsed tape (fed to in-process runners)
  std::vector<std::string> native_O{"0"};
  std::string exe_name{};    // -o NAME: name of the --emit=exe executable
  bool emit_c{false}, emit_asm{false}, emit_bc{false}, emit_exe{false};
  bool do_run{false}, no_opt{false}, print_ir{false}, print_bc{false};
  bool no_inline{false};
  bool no_reassoc{false}, no_lcf{false}, no_divmagic{false};
  int opt_level{-1};  // -O0..-O3 preset (-1: default, i.e. 3)
  unsigned debug{0};  // --debug=N: pass-manager dump volume (0..3)
  unsigned fp_limit{24}; // --fp-limit=N: fixpoint round cap (0 = default)
  bool regalloc{true};   // --no-regalloc: identity slots ("SSA-like" C)
  std::vector<std::string> enable_passes{}, disable_passes{};
  unsigned unroll{16}; // max constant trip count to fully unroll (0 = off)
  bool no_exec{false}, quiet{false}, check_all{false}, debug_plan{false};
  unsigned repeats{3};
  uint64_t steps{200000000ull};
  long fuzz{0}, fuzz_native{0};
  bool demo{false}, selftest{false}, help{false};
};

static void usage() {
  std::printf(
      "minijz — a small compiler on mjz_lib graph algorithms\n"
      "\n"
      "usage: minijz [options] [file.mjz]\n"
      "  (no file, tty)   interactive: type statements, blank line runs, :q "
      "quits\n"
      "  (no file, pipe)  the program is read from stdin\n"
      "  -e \"<source>\"    compile the inline source instead of a file\n"
      "\n"
      "actions:\n"
      "  --demo            curated demo suite\n"
      "  --fuzz[=N]        differential fuzz of N random programs (default "
      "1000)\n"
      "  --fuzz-native[=K] additionally build & run C+asm at -O0/-O2 for the\n"
      "                    first K fuzz programs (slow; proves the emitted\n"
      "                    code byte-for-byte against the oracle)\n"
      "  --selftest        demos + fuzz(200)\n"
      "\n"
      "compile & run:\n"
      "  --run             emit, build and run ALL backends, verify identical "
      "output,\n"
      "                    print a timing table (uses the native compiler "
      "below)\n"
      "  --emit=c,asm,bc   write backend files (.c / .s / .mbc + .mbc.lst)\n"
      "  --emit=exe         also build an executable with the native\n"
      "                    compiler (uses --cc and the first --native-O\n"
      "                    level; -o NAME renames it)\n"
      "  -o NAME           output executable name for --emit=exe\n"
      "  --no-exec         compile/emit only: do not run the program\n"
      "                    (no oracle/compiled lines, stdin untouched)\n"
      "  --stdin=FILE      whitespace-separated integers fed to input():\n"
      "                    parsed for the in-process runners AND piped verbatim\n"
      "                    into the native binaries (default: real stdin if it\n"
      "                    is not a terminal; reads return 0 at EOF)\n"
      "  --native-O=L      optimization level(s) for the native compiler,\n"
      "                    comma list (e.g. 0,2); default 0 — measure OUR "
      "codegen,\n"
      "                    not the native compiler's improvements\n"
      "  --cc=CMD          native compiler driver (default: clang++ if "
      "present, else g++)\n"
      "  --repeats=N       timing repeats for --run (default 3, min taken)\n"
      "  --steps=N         interpreter/VM step limit (default 200000000)\n"
      "\n"
      "optimizer (pass manager):\n"
      "  -O0 .. -O3        preset (default -O3). -O0: nothing (SSA +\n"
      "                    destruction only); -O1: cheap tier to fixpoint\n"
      "                    (fold/copyprop/canon/cse/unreach/splice/dce);\n"
      "                    -O2: + mem2reg, trapelim (range+alias), licm,\n"
      "                    divmagic, reassoc; -O3: + unroll, lcf, inline\n"
      "                    (expensive CFG restructuring runs only after\n"
      "                    the cheaper tiers reach their fixpoint)\n"
      "  --enable=P,P2      force passes on (folds, copyprop, canon, cse,\n"
      "  --disable=P,P2     force passes off  unreach, splice, dce, mem2reg,\n"
      "                    trapelim, licm, divmagic, reassoc, unroll, lcf,\n"
      "                    inline) — applied on top of the -O preset, so\n"
      "                    e.g. `-O1 --enable=lcf` is legal\n"
      "  --unroll=N         full-unroll loops with constant trip count <= N\n"
      "                    (default 16; 0 disables unrolling)\n"
      "  --fp-limit=N       fixpoint round cap per tier (default 24)\n"
      "  --debug=N          pass-manager dumps: 1 = one line per pass;\n"
      "                    2 = + the IR after each pass that changed;\n"
      "                    3 = + per-fixpoint-round headers\n"
      "  --no-regalloc      keep one named local per vreg (SSA-like C, one\n"
      "                    VM slot per vreg) instead of interference-colored\n"
      "                    shared slots (register-like C: r0..r{K-1})\n"
      "  --no-opt           skip the optimizer entirely (same as -O0)\n"
      "  --no-inline        skip interprocedural inlining (call-graph SCC\n"
      "                    analysis still runs; recursive fns never inline)\n"
      "  --no-reassoc       skip linear-combination reassociation\n"
      "  --no-lcf           skip closed-form loop evaluation (compile-time\n"
      "                    arithmetic/geometric series + simulation, AND the\n"
      "                    guarded runtime-trip-count closed form)\n"
      "  --no-divmagic      skip div/mod-by-constant -> mulhi+shift lowering\n"
      "  --print-ir        dump SSA (pre/post opt) and destroyed IR\n"
      "  --print-bc        print the bytecode listing\n"
      "  --quiet           no reports, just results\n"
      "  --help            this text\n");
}

static void split_commas(const std::string &s, std::vector<std::string> &out) {
  std::string cur{};
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
}

static bool parse_cli(int argc, char **argv, Cli &C) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char *pre) -> const char * {
      size_t n = std::strlen(pre);
      if (a.compare(0, n, pre) != 0) return nullptr;
      const char *v = a.c_str() + n;
      if (!*v) return nullptr;
      return v;
    };
    if (a == "--help" || a == "-h") C.help = true;
    else if (a == "--demo") C.demo = true;
    else if (a == "--selftest") C.selftest = true;
    else if (a == "--fuzz") C.fuzz = 1000;
    else if (const char *v = val("--fuzz=")) C.fuzz = atol(v);
    else if (a == "--fuzz-native") C.fuzz_native = 20;
    else if (const char *v = val("--fuzz-native=")) C.fuzz_native = atol(v);
    else if (a == "--run") C.do_run = true;
    else if (a == "--no-exec" || a == "--compile-only") C.no_exec = true;
    else if (a == "--check-all") C.check_all = true;
    else if (a == "--debug-plan") C.debug_plan = true;
    else if (a == "-O0") C.opt_level = 0;
    else if (a == "-O1") C.opt_level = 1;
    else if (a == "-O2") C.opt_level = 2;
    else if (a == "-O3") C.opt_level = 3;
    else if (const char *v = val("--opt=")) {
      int lv = atoi(v);
      if (lv < 0 || lv > 3) {
        std::printf("--opt=%s: level must be 0..3\n", v);
        return false;
      }
      C.opt_level = lv;
    } else if (const char *v = val("--enable=")) {
      split_commas(v, C.enable_passes);
    } else if (const char *v = val("--disable=")) {
      split_commas(v, C.disable_passes);
    } else if (const char *v = val("--debug=")) C.debug = (unsigned)atol(v);
    else if (const char *v = val("--fp-limit="))
      C.fp_limit = (unsigned)atol(v);
    else if (a == "--no-regalloc") C.regalloc = false;
    else if (a == "--no-opt") C.no_opt = true;
    else if (a == "--no-inline") C.no_inline = true;
    else if (a == "--no-reassoc") C.no_reassoc = true;
    else if (a == "--no-lcf") C.no_lcf = true;
    else if (a == "--no-divmagic") C.no_divmagic = true;
    else if (const char *v = val("--unroll=")) C.unroll = (unsigned)atol(v);
    else if (a == "--print-ir") C.print_ir = true;
    else if (a == "--print-bc") C.print_bc = true;
    else if (a == "--quiet") C.quiet = true;
    else if (const char *v = val("--repeats=")) C.repeats = (unsigned)atol(v);
    else if (const char *v = val("--steps=")) C.steps = strtoull(v, nullptr, 10);
    else if (const char *v = val("--cc=")) C.cc = v;
    else if (const char *v = val("--stdin=")) C.stdin_path = v;
    else if (const char *v = val("--out-dir=")) C.out_dir = v;
    else if (const char *v = val("--native-O=")) {
      C.native_O.clear();
      split_commas(v, C.native_O);
    } else if (const char *v = val("--emit=")) {
      std::vector<std::string> parts{};
      split_commas(v, parts);
      for (auto &p : parts) {
        if (p == "c") C.emit_c = true;
        else if (p == "asm") C.emit_asm = true;
        else if (p == "bc") C.emit_bc = true;
        else if (p == "exe") C.emit_exe = true;
        else {
          std::printf("unknown --emit target '%s' (c, asm, bc, exe)\n", p.c_str());
          return false;
        }
      }
    } else if (a == "-e") {
      if (i + 1 >= argc) {
        std::printf("-e needs an argument\n");
        return false;
      }
      C.inline_src = argv[++i];
    } else if (a == "-o") {
      if (i + 1 >= argc) {
        std::printf("-o needs an argument\n");
        return false;
      }
      C.exe_name = argv[++i];
    } else if (!a.empty() && a[0] == '-') {
      std::printf("unknown option '%s' (try --help)\n", a.c_str());
      return false;
    } else {
      if (!C.file.empty()) {
        std::printf("multiple input files given ('%s' and '%s')\n",
                    C.file.c_str(), a.c_str());
        return false;
      }
      C.file = a;
    }
  }
  return true;
}

// ---- CLI -> PassCfg ---------------------------------------------------------
// The -O preset selects the pass tiers; granular legacy flags and the
// --enable/--disable overrides are applied on top (in that order, so a
// later --disable wins over --enable of the same pass).
static const char *const PASS_NAMES[] = {"fold",  "copyprop", "canon",
    "cse",   "unreach",  "splice", "dce",     "mem2reg", "trapelim",
    "licm",  "divmagic", "reassoc", "unroll", "lcf",     "inline"};
static bool set_named_pass(PassCfg &p, const std::string &n, bool on,
                           std::string &err) {
  auto eq = [&](const char *s) { return n == s; };
  if (eq("fold")) p.p_fold = on;
  else if (eq("copyprop")) p.p_copyprop = on;
  else if (eq("canon")) p.p_canon = on;
  else if (eq("cse")) p.p_cse = on;
  else if (eq("unreach")) p.p_unreach = on;
  else if (eq("splice")) p.p_splice = on;
  else if (eq("dce")) p.p_dce = on;
  else if (eq("mem2reg")) p.p_mem2reg = on;
  else if (eq("trapelim")) p.p_trapelim = on;
  else if (eq("licm")) p.p_licm = on;
  else if (eq("divmagic")) p.p_divmagic = on;
  else if (eq("reassoc")) p.p_reassoc = on;
  else if (eq("unroll")) p.p_unroll = on;
  else if (eq("lcf")) p.p_lcf = on;
  else if (eq("inline")) p.do_inline = on;
  else {
    err = n;
    return false;
  }
  return true;
}
static PassCfg pass_cfg_of(const Cli &C) {
  PassCfg p{};
  p.set_level(C.opt_level < 0 ? 3u : (uint32_t)C.opt_level);
  p.unroll = C.unroll;
  p.fp_limit = C.fp_limit ? C.fp_limit : 24u;
  p.debug = (uint8_t)(C.debug > 3 ? 3 : C.debug);
  p.regalloc = C.regalloc;
  if (C.no_opt) p.do_opt = false;
  if (C.no_inline) p.do_inline = false;
  if (C.no_reassoc) p.p_reassoc = false; // legacy granular flags
  if (C.no_lcf) p.p_lcf = false;
  if (C.no_divmagic) p.p_divmagic = false;
  std::string err{};
  for (const std::string &s : C.enable_passes)
    if (!set_named_pass(p, s, true, err)) {
      std::printf("unknown pass '%s' (see --help)\n", s.c_str());
      break;
    }
  for (const std::string &s : C.disable_passes)
    if (!set_named_pass(p, s, false, err)) {
      std::printf("unknown pass '%s' (see --help)\n", s.c_str());
      break;
    }
  return p;
}

// the per-file compile report + in-process runs; returns the optimized RunOut
static RunOut compile_report(Program &P, const Cli &C, const std::string &src,
                             bool brief) {
  P = compile(src, pass_cfg_of(C));
  if (!P.ok) {
    std::printf("compile error: %s (at byte %u)\n",
                P.error ? P.error : "?", (unsigned)P.errpos);
    return {};
  }
  if (!brief && !C.quiet) {
    OptStats os = sum_ostats(P);
    size_t ssa_i = 0, opt_i = 0, dst_i = 0;
    uint32_t phis = 0, de_c = 0, dz_c = 0, dse = 0, dcy = 0, dfu = 0;
    for (const FnUnit &U : P.fns) {
      ssa_i += U.ssa.code.size();
      opt_i += U.opt.code.size();
      dst_i += U.destroyed.code.size();
      phis += U.sstats.phis;
      de_c += U.dstats.edges_split;
      dz_c += U.dstats.copies;
      dse += U.dstats.self_copies;
      dcy += U.dstats.cycles_broken;
      dfu += U.dstats.fused;
    }
    print_analysis(P.an_pre_main, P.fns[0].ssa, "analysis (mjz graph)");
    std::printf(
        "  fns: %zu | ssa: %zu instrs, %u phis | opt: %zu | destroyed: %zu\n"
        "  opt: iters %u | folds %u | copies %u | copyprop %u | cse %u | "
        "canon %u+%u | brs %u | cbr== %u | dead-blk %u | splice %u | "
        "phi-degen %u | dce %u | hoist %u | INLINED %u | UNROLLED %u | "
        "REASSOC %u(-%u) | LCF %u+%u+g%u|rt%u | DIVMAGIC %u | NOTCOMB %u | "
        "TRAP-KILL %u | MEM2REG %u | DSE %u\n"
        "  destroy: split-edges %u | copies %u | self %u | cycles %u | "
        "copy-fused %u\n",
        P.fns.size(), ssa_i, (unsigned)phis, opt_i, dst_i,
        (unsigned)os.iters, (unsigned)os.folds, (unsigned)os.copies,
        (unsigned)os.copyprops, (unsigned)os.cses, (unsigned)os.canon_swaps,
        (unsigned)os.canon_cmps, (unsigned)os.brs_folded,
        (unsigned)os.cbr_same, (unsigned)os.blocks_gone, (unsigned)os.splices,
        (unsigned)os.phi_degens, (unsigned)os.dce, (unsigned)os.hoisted,
        (unsigned)os.inlines, (unsigned)os.unrolls, (unsigned)os.reassocs,
        (unsigned)os.reassoc_gone, (unsigned)os.lcf_loops,
        (unsigned)os.lcf_sims, (unsigned)os.lcf_gensims, (unsigned)os.lcf_rt,
        (unsigned)os.divmagics, (unsigned)os.notcombs,
        (unsigned)os.traps_killed, (unsigned)os.mem2reg_lifts,
        (unsigned)os.mem2reg_dses, (unsigned)de_c,
        (unsigned)dz_c, (unsigned)dse, (unsigned)dcy, (unsigned)dfu);
  }
  if (C.print_ir) {
    for (size_t f = 0; f < P.fns.size(); ++f) {
      char t1[96], t2[96], t3[96];
      std::snprintf(t1, sizeof t1, "SSA (before opt) [%s]",
                    std::string(P.fn_names[f]).c_str());
      std::snprintf(t2, sizeof t2, "SSA (after opt) [%s]",
                    std::string(P.fn_names[f]).c_str());
      std::snprintf(t3, sizeof t3, "destroyed [%s]",
                    std::string(P.fn_names[f]).c_str());
      print_fn(P.fns[f].ssa, t1);
      print_fn(P.fns[f].opt, t2);
      print_fn(P.fns[f].destroyed, t3);
    }
  }
  if (C.no_exec && !C.do_run) return {}; // compile/emit only
  RunOut R = run_prog(fns_of(P, 1), C.steps, C.inputs);
  RunOut Ro = run_ast(P.P.ast, P.P.root, C.steps, C.inputs);
  if (!C.quiet || brief) {
    std::printf("oracle   : ");
    print_run(Ro);
    std::printf("compiled : ");
    print_run(R);
    if (!same_run(Ro, R))
      std::printf("*** MISMATCH between oracle and compiled output ***\n");
  }
  return R;
}

// the --run matrix: in-process runners + native builds, match check, timings
static void run_matrix(Program &P, const Cli &C, const std::string &stem) {
  std::string cap = C.out_dir + "/" + stem + ".out.tmp";
  // --- in-process runners (reference results & timings)
  double t0 = now_ms();
  RunOut ref = run_prog(fns_of(P, 1), C.steps, C.inputs);
  double t_interp = now_ms() - t0;
  std::string ref_text = text_of(ref);
  t0 = now_ms();
  std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
  RunOut vm = vm_run(bc, C.steps, C.inputs);
  double t_vm = now_ms() - t0;
  bool vm_ok = (text_of(vm) == ref_text);
  if (ref.timeout) {
    std::printf("interpreter hit the step limit; native runs skipped "
                "(an infinite loop would hang)\n");
    return;
  }
  // --- write the native sources (always, since we are about to build them)
  std::string c_path = C.out_dir + "/" + stem + ".c";
  std::string s_path = C.out_dir + "/" + stem + ".s";
  if (!write_file(c_path, emit_c(fns_of(P, 2), P.fn_names)) ||
      !write_file(s_path, emit_x64(fns_of(P, 2), P.fn_names))) {
    std::printf("error: cannot write %s / %s\n", c_path.c_str(),
                s_path.c_str());
    return;
  }
  std::string cc = C.cc.empty() ? find_cc() : C.cc;
  if (cc.empty()) {
    std::printf("no native compiler found (tried clang++, g++, cc); "
                "in-process results only\n");
    std::printf("  %-14s %10.3f ms  %s\n", "interp(ssa)", t_interp,
                "reference");
    std::printf("  %-14s %10.3f ms  %s\n", "vm(bytecode)", t_vm,
                vm_ok ? "match" : "MISMATCH");
    return;
  }
  if (!C.quiet)
    std::printf("native driver: %s (autodetected: clang++ preferred)\n",
                cc.c_str());
  // the native binaries read the SAME input() tape the in-process runners
  // saw (verbatim bytes); with no tape they read /dev/null (EOF -> 0)
  std::string in_path = "/dev/null";
  if (!C.stdin_text.empty()) {
    std::string p = C.out_dir + "/" + stem + ".in.tmp";
    if (write_file(p, C.stdin_text)) in_path = p;
  }
  std::printf("  %-18s %10s  %s\n", "backend", "time(ms)", "output");
  std::printf("  %-18s %10.3f  %s\n", "interp (SSA opt)", t_interp,
              "reference");
  std::printf("  %-18s %10.3f  %s\n", "vm (bytecode)", t_vm,
              vm_ok ? "match" : "MISMATCH");
  for (const std::string &lvl : C.native_O) {
    struct Job {
      const char *tag;
      std::string src_path;
      bool is_asm;
    };
    Job jobs[2] = {{"c", c_path, false}, {"asm", s_path, true}};
    for (Job &J : jobs) {
      std::string exe = C.out_dir + "/" + stem + "." + J.tag + ".O" + lvl +
                        ".exe";
      std::string build = "timeout 60 " + cc + " -w -O" + lvl + " " +
                          J.src_path + " -o " + exe;
      if (std::system(build.c_str()) != 0) {
        std::printf("  %-18s %10s  BUILD FAILED (%s -O%s)\n", J.tag, "-", cc.c_str(), lvl.c_str());
        continue;
      }
      std::string out_text{};
      double best = 1e300;
      int rc = 0;
      for (unsigned r = 0; r < (C.repeats ? C.repeats : 1); ++r) {
        t0 = now_ms();
        rc = run_capture("timeout 20 " + exe, out_text, cap, in_path);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
      }
      bool timed_out = (rc != 0 && (((unsigned)rc >> 8) == 124 ||
                                     ((unsigned)rc >> 8) == 137));
      if (timed_out) out_text += "TIMEOUT\n";
      bool match = (out_text == ref_text);
      std::string name = std::string(J.tag) + " -O" + lvl;
      std::printf("  %-18s %10.3f  %s\n", name.c_str(), best,
                  match ? "match" : "MISMATCH");
    }
  }
}

static void repl() {
  char line[4096];
  std::string buf{};
  std::printf("MiniJZ interactive — statements end with ';'; a blank line "
              "compiles+runs the buffer as an INDEPENDENT program "
              "(variables do not carry over); :r resets; :q quits\n");
  for (;;) {
    std::printf("minijz> ");
    std::fflush(stdout);
    if (!std::fgets(line, sizeof line, stdin)) break;
    std::string t(line);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' ||
                          t.back() == ' ' || t.back() == '\t'))
      t.pop_back();
    if (t == ":q") break;
    if (t == ":r") {
      buf.clear();
      std::printf("(buffer reset)\n");
      continue;
    }
    if (t.empty()) {
      if (buf.empty()) continue;
      Cli C{};
      C.steps = 10000000;
      Program P{};
      (void)compile_report(P, C, buf, true);
      buf.clear();
      continue;
    }
    buf += t;
    buf += "\n";
  }
  std::printf("\n(bye)\n");
}

int main(int argc, char **argv) {
  Cli C{};
  if (!parse_cli(argc, argv, C)) return 2;
  if (C.help) {
    usage();
    return 0;
  }
  bool have_action = C.demo || C.selftest || C.fuzz > 0;
  std::string src{};
  std::string stem = "stdin";
  bool src_from_stdin = false; // stdin already consumed as the PROGRAM
  if (!C.file.empty()) {
    src = slurp_file(C.file);
    if (src.empty()) {
      std::printf("cannot read '%s'\n", C.file.c_str());
      return 1;
    }
    size_t slash = C.file.find_last_of('/');
    std::string base =
        slash == std::string::npos ? C.file : C.file.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    stem = dot == std::string::npos ? base : base.substr(0, dot);
    if (C.out_dir == ".") {
      C.out_dir = slash == std::string::npos ? "." : C.file.substr(0, slash);
    }
  } else if (!C.inline_src.empty()) {
    src = C.inline_src;
    stem = "inline";
  } else if (!have_action) {
#ifdef MJZ_HAVE_ISATTY
    if (isatty(0)) {
      std::printf("=== MiniJZ on mjz_lib (constexpr pipeline passed: see "
                  "static_assert) ===\n");
      repl();
      return 0;
    }
#endif
    src = slurp_file("/dev/stdin");
    src_from_stdin = true;
  }

  if (C.demo || C.selftest) {
    std::printf("=== MiniJZ: a basic compiler on mjz_lib graph algorithms "
                "===\n");
#ifdef MJZ_CX_ASSERT
    std::printf("(the constexpr pipeline already passed: see "
                "static_assert)\n\n");
#else
    CHECK(cx_pipeline(),
          "the full pipeline (incl. destruction + VM) must work");
    std::printf("(pipeline self-check passed)\n\n");
#endif
    run_demos();
    std::printf("\n");
  }
  if (C.fuzz > 0 || C.selftest) {
    if (C.selftest && C.fuzz_native <= 0) C.fuzz_native = 6; // native gate
    run_fuzz(C.selftest ? 200 : C.fuzz, C.cc, C.fuzz_native, C.out_dir);
  }
  if (have_action) {
    std::printf("\ntotal failures: %d (of %d checks)\n", g_fails, g_checks);
    if (!src.empty()) std::printf("\n");
  }

  if (!src.empty()) {
    // ---- the input() tape ------------------------------------------------
    // --stdin=FILE wins; otherwise, if the program reads input() and stdin
    // still has data (program came from a file/-e, stdin is a pipe), the
    // piped bytes become the tape. Native builds get the tape verbatim.
    if (!C.stdin_path.empty()) {
      std::ifstream f(C.stdin_path, std::ios::binary);
      if (!f) {
        std::printf("cannot read --stdin file '%s'\n", C.stdin_path.c_str());
        return 1;
      }
      C.stdin_text =
          std::string((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
      C.inputs = parse_inputs(C.stdin_text);
    } else if (!C.no_exec || C.do_run) {
      // probe only when something will actually execute in-process
      PassCfg pc_probe{};
      pc_probe.do_opt = false;
      pc_probe.regalloc = false;
      Program P0 = compile(src, pc_probe); // cheap probe: uses input()?
      if (P0.ok && P0.uses_input) {
#ifdef MJZ_HAVE_ISATTY
        if (!src_from_stdin && !isatty(0)) {
          C.stdin_text = slurp_file("/dev/stdin");
          C.inputs = parse_inputs(C.stdin_text);
        } else {
          std::printf("(note: input() reads return 0 — pass --stdin=FILE or "
                      "pipe data on stdin)\n");
        }
#else
        if (!src_from_stdin) {
          C.stdin_text = slurp_file("/dev/stdin");
          C.inputs = parse_inputs(C.stdin_text);
        }
#endif
      }
    }
    Program P{};
    RunOut R = compile_report(P, C, src, false);
    if (!P.ok) return 1;
    if (C.debug_plan) { // dump the emission plan decisions per instr
      for (size_t fi = 0; fi < P.fns.size(); ++fi) {
        const Fn &F = P.fns[fi].destroyed;
        EmitPlan PL = make_emit_plan(F);
        std::printf("== fn %zu [%s] ==\norder:", fi,
                    std::string(P.fn_names[fi]).c_str());
        for (uint32_t b2 : PL.order)
          std::printf(" b%u(nxt=b%u)", b2,
                      PL.next[b2] == ~uint32_t(0) ? 999u : PL.next[b2]);
        std::printf("\n");
        for (uint32_t b = 0; b < F.nblocks(); ++b)
          for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
            const Instr &I = F.code[i];
            std::printf("i%u b%u k=%d dst=%u dead=%d kc=%d uses=%u\n", i, b,
                        (int)I.k, I.dst, PL.dead[i] ? 1 : 0,
                        (I.dst < PL.kc.size()) ? PL.kc[I.dst] : -1,
                        (I.dst < PL.uses.size()) ? PL.uses[I.dst] : 0u);
          }
      }
    }
    if (C.check_all) { // all five runners, like the fuzzer (debug aid)
      RunOut oracle = run_ast(P.P.ast, P.P.root, C.steps, C.inputs);
      RunOut plain = run_prog(fns_of(P, 0), C.steps, C.inputs);
      RunOut lowered = run_prog(fns_of(P, 2), C.steps, C.inputs);
      std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
      RunOut vm = vm_run(bc, C.steps, C.inputs);
      const char *names[] = {"oracle", "ssa", "lower", "vm"};
      RunOut vals[4] = {oracle, plain, lowered, vm};
      for (int k = 0; k < 4; ++k) {
        std::printf("%-8s: ", names[k]);
        print_run(vals[k]);
      }
      CHECK(same_run(oracle, plain) && same_run(oracle, lowered) &&
                same_run(oracle, vm),
            "--check-all: runners must agree");
    }
    if (C.emit_bc || C.do_run) {
      std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
      std::string bc_path = C.out_dir + "/" + stem + ".mbc";
      write_file_bytes(bc_path, bc);
      std::string lst = emit_bc_listing(bc);
      write_file(bc_path + ".lst", lst);
      if (C.print_bc) std::printf("%s", lst.c_str());
      if (!C.quiet && C.emit_bc)
        std::printf("wrote %s (+ .lst)\n", bc_path.c_str());
    }
    if (C.do_run) {
      run_matrix(P, C, stem);
    } else {
      if (C.emit_c || C.emit_exe) { // exe builds from the emitted C
        std::string p = C.out_dir + "/" + stem + ".c";
        write_file(p, emit_c(fns_of(P, 2), P.fn_names));
        if (!C.quiet && C.emit_c) std::printf("wrote %s\n", p.c_str());
      }
      if (C.emit_asm) {
        std::string p = C.out_dir + "/" + stem + ".s";
        write_file(p, emit_x64(fns_of(P, 2), P.fn_names));
        if (!C.quiet) std::printf("wrote %s\n", p.c_str());
      }
      if (C.emit_exe) {
        std::string cc = C.cc.empty() ? find_cc() : C.cc;
        if (cc.empty()) {
          std::printf("no native compiler found (tried clang++, g++, cc); "
                      "exe not built\n");
        } else {
          std::string exe = C.exe_name.empty() ? C.out_dir + "/" + stem + ".exe"
                                               : C.exe_name;
          std::string lvl = C.native_O.empty() ? "0" : C.native_O.front();
          std::string c_path = C.out_dir + "/" + stem + ".c";
          std::string cmd =
              cc + " -w -O" + lvl + " " + c_path + " -o " + exe;
          if (std::system(cmd.c_str()) == 0) {
            if (!C.quiet)
              std::printf("built %s (via: %s)\n", exe.c_str(), cmd.c_str());
          } else {
            std::printf("BUILD FAILED: %s\n", cmd.c_str());
            return 1;
          }
        }
      }
    }
    (void)R;
    return 0;
  }
  if (!have_action) {
    usage();
    return 0;
  }
  return g_fails ? 1 : 0;
}
// cache probe 1793991399152465055
