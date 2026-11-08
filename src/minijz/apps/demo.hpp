#pragma once
// === demo.hpp ========================================
// the curated demo suite.

#include "minijz/prog/program.hpp"
#include "minijz/prog/runners.hpp"
#include "minijz/runtime/astrun.hpp"

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
static const val_t DEMO_INS_CCALL[] = {5};
static const val_t DEMO_INS_COUNT[] = {100};
// regression tape for regr_notnot_compare (fuzz seed 1211's first 16
// values; the boundary case is the EOF-0 read after the tape runs out)
static const val_t DEMO_INS_NOTNOT[] = {35, -3, 32, 23, -22, -30, 40, -37,
                                        14, -28, 15, -23, 21, -6, -12, -27};

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
  {"fn_mutual_recursion", // a 2-member call-graph SCC: never inlined, but
   // TAIL-CALLED: scctco (default) collapses each member into a loop and
   // the count-down folds to `n & 1`; with --disable=scctco the tailrec
   // path merges the SCC into one sel-dispatched leaf instead, cdfold
   // replaces its countdown by the closed form + TRAP guard and ifconv
   // folds the member select — both routes end at print(n & 1)
   "fn is_even(n) { if (n == 0) { return 1; } return is_odd(n - 1); }\n"
   "fn is_odd(n) { if (n == 0) { return 0; } return is_even(n - 1); }\n"
   "print is_even(10);\nprint is_odd(7);\nprint is_even(7);\n",
   true},
  {"tailrec_mutual_fold",
   // the full pipeline: is_odd(input()) -> the guard (the 512-activation
   // recursion cap is a language semantic, so is_odd(600) TRAPs) +
   // input() & 1. Default route: scctco's collapse + fold; run with
   // --disable=scctco to watch tailrec -> cdfold -> ifconv build the
   // same result from the sel-dispatched leaf
   "fn is_even(n) { if (n == 0) { return 1; } return is_odd(n - 1); }\n"
   "fn is_odd(n) { if (n == 0) { return 0; } return is_even(n - 1); }\n"
   "print is_odd(input());\n",
   true, DEMO_INS_CCALL, 1},
  {"tailrec_self", // single-member: a self tail call becomes a loop, then
   // the ADDITIVE-accumulator countdown closed form: count(100,0) = 5050
   // via x_N = x0 + N*n0 - N(N-1)/2 — no loop, no call, exact budget
   // guard (scctco + LCF by default; tailrec + cdfold under
   // --disable=scctco)
   "fn count(n, acc) { if (n == 0) { return acc; } "
   "return count(n - 1, acc + n); }\n"
   "print count(input(), 0);\n",
   true, DEMO_INS_COUNT, 1},
  {"const_call_eval",
   // recursion + mutual recursion + a never-read array + a runtime trip
   // count, all at once: the calls fold via the interprocedural const-call
   // evaluator (inlining can never touch an SCC, but an exact evaluator
   // walks the recursive call graph like any other CFG), the dead array's
   // store AND its allocation disappear, and the runtime loop collapses
   // to the unguarded exact closed form. Nothing but constants, one
   // input() and eleven arithmetic instructions should remain.
   "fn fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); }\n"
   "fn is_even(n) { if (n == 0) { return 1; } return is_odd(n - 1); }\n"
   "fn is_odd(n) { if (n == 0) { return 0; } return is_even(n - 1); }\n"
   "let dead = array(4);\n"
   "dead[0] = fact(3);\n"
   "print fact(6);\n"
   "print is_even(10);\n"
   "print is_odd(7);\n"
   "let n = input();\nlet t = 0;\nlet i = 0;\n"
   "while (i < n) { t = t + i * 2; i = i + 1; }\nprint t;\n",
   true, DEMO_INS_CCALL, 1},
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
  {"lcf_rt_dynamic_bound", // RUNTIME trip count, provably exact: i0 = 0
   // (constant, >= 0) with unit step can never wrap, and the body is pure
   // arithmetic -- so the closed form is emitted UNGUARDED and the loop is
   // deleted outright (unreach removes the orphaned blocks)
   "let n = input();\nlet s = 0;\nlet i = 0;\n"
   "while (i < n) { s = s + i * 9 - 7; i = i + 1; }\nprint s;\nprint i;\n",
   true, DEMO_INS_LCFRT, 1},
  {"lcf_rt_le", // <= variant: N = floor((B-i0)/d) + 1; i0 = 1 >= 0 and
   // step 1, so this closed form is also provably exact (unguarded)
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
  // ---- regression demos (each pins one fixed miscompile; all found by
  // differential fuzzing -- see examples/BUGFIXES.md) ----------------------
  {"regr_scc_dispatch", // 2-member SCC whose members dispatch on n%4 before
   // the sibling tail call: the sibling copy's entry body kept reading the
   // RAW entry argument after its merge phis existed (scctco promote())
   "fn a(n) { if (n==0) { return 1; } if (n%4==3) { return b(n/4); } "
   "return a(n/4); }\n"
   "fn b(n) { if (n==0) { return 2; } if (n%4==3) { return b(n/4); } "
   "return a(n/4); }\n"
   "print a(0);\nprint a(3);\nprint a(12);\nprint a(15);\nprint b(3);\n"
   "print a(1);\nprint a(255);\nprint a(64);\nprint a(5);\n",
   false},
  {"regr_self_multisite", // 4 self tail sites + a base case (the fun_easy
   // shape): tce_self's latch blocks were emitted in reverse order, so the
   // chain never reached the header and n never rotated
   "fn f(n) { if (n/4==0) { return n%4; } if (n%4==1) { return f(n/4); }\n"
   "  if (n%4==2) { return f(n/4); } if (n%4==3) { return f(n/4); } "
   "return f(n/4); }\n"
   "print f(255);\nprint f(64);\nprint f(5);\nprint f(0);\nprint f(3);\n",
   false},
  {"regr_scc3_members", // 3-member tail SCC (m3a/m3b/m3c): tce_merge's
   // header dispatch skipped the sel==2 test, so member c was unreachable
   // and the countdown returned the wrong member's base
   "fn a(n) { if (n == 0) { return 7; } return b(n - 1); }\n"
   "fn b(n) { if (n == 0) { return 8; } return c(n - 1); }\n"
   "fn c(n) { if (n == 0) { return 9; } return a(n - 1); }\n"
   "print a(5);\nprint a(0);\nprint a(1);\nprint a(2);\nprint a(6);\n"
   "print b(4);\nprint c(7);\n",
   false},
  {"regr_scc4_dispatch", // the fun/fun1/fun2/fun3 shape from
   // examples/general.mjz: 4 mutually tail-recursive members
   "fn f(n) { if (n==0) { return 0; } if (n%4==1) { return f1(n/4); }\n"
   "  if (n%4==2) { return f2(n/4); } if (n%4==3) { return f3(n/4); } "
   "return f(n/4); }\n"
   "fn f1(n) { if (n==0) { return 1; } if (n%4==1) { return f1(n/4); }\n"
   "  if (n%4==2) { return f2(n/4); } if (n%4==3) { return f3(n/4); } "
   "return f(n/4); }\n"
   "fn f2(n) { if (n==0) { return 2; } if (n%4==1) { return f1(n/4); }\n"
   "  if (n%4==2) { return f2(n/4); } if (n%4==3) { return f3(n/4); } "
   "return f(n/4); }\n"
   "fn f3(n) { if (n==0) { return 3; } if (n%4==1) { return f1(n/4); }\n"
   "  if (n%4==2) { return f2(n/4); } if (n%4==3) { return f3(n/4); } "
   "return f(n/4); }\n"
   "print f(255);\nprint f(64);\nprint f(5);\nprint f(0);\n"
   "print f(85);\nprint f3(255);\n",
   false},
  {"regr_notnot_compare", // !!(a < b) was mis-compiled to a <= b: the
   // notcomb negation table mapped Ge to Le instead of Lt; only visible
   // when the compare's left side hits the boundary value
   "fn h0(p0, p1, p2) { }\n"
   "let z = 0;\n"
   "if (input()) { }\n"
   "let c = 2;\n"
   "while (c > 0) {\n  c = c - 1;\n"
   "  if ((len(z) - 1) && (-47612 + h0(input(), -2, input()))) {\n"
   "    let d = 2;\n    while (d > 0) {\n      d = d - 1;\n"
   "      if (-((input() % -5))) {\n"
   "        if (((input() - input()) / -11)) { continue; }\n      }\n"
   "      print !(!((input() < z)));\n    }\n  }\n}\n",
   false, DEMO_INS_NOTNOT, 16},
};

void run_demos() {
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
      print_pre_main_analysis(P);
      std::printf(
          "  fns: %zu | ssa: %zu instrs, %u phis | opt: %zu | destroyed: %zu\n"
          "  opt report: iters %u | folds %u | copies %u | copyprop %u | "
          "cse %u | canon %u+%u | brs %u | cbr== %u | dead blocks %u | "
          "splices %u | phi-degen %u | dce %u | hoisted %u | INLINED %u | "
          "UNROLLED %u | REASSOC %u(-%u) | LCF %u+%u+g%u|rt%u | DIVMAGIC %u | "
          "NOTCOMB %u | TRAP-KILL %u | MEM2REG %u | DSE %u | CONSTCALL %u | "
          "DEADALLOC %u | SCC-TCO %u(+%u) | TAILREC %u(+%u) | CDFOLD %u | "
          "IFCONV %u\n"
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
          (unsigned)os.constcalls, (unsigned)os.deadallocs,
          (unsigned)os.scctco, (unsigned)os.scctco_folds,
          (unsigned)os.tces, (unsigned)os.tce_merges, (unsigned)os.cdfolds,
          (unsigned)os.ifconvs,
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
