#pragma once
// === cx_tours.hpp ========================================
// constexpr tours: curated in-compile-time checks + the general
// simulator/vocabulary tours used by --selftest and the cx build.

#include "../opt/analysis/an.hpp" // analyze() for the tour checks
#include "../runtime/astrun.hpp"  // run_ast (the oracle)
#include "../prog/program.hpp"
#include "../prog/runners.hpp"
#include "../apps/fuzz_gen.hpp"

// ============================================================================
// 16. constexpr pipeline — the whole compiler runs at compile time
// ============================================================================
// The compile-time proof is split into SEPARATE constant expressions
// (static_asserts): g++ retains each one's evaluation state until the end
// of the translation, so the peak memory scales with how many run — three
// programs fit comfortably in 4 GiB, four do not. The three asserts below
// cover arena arrays + lib loop analyses, functions (recursion, mutual
// recursion, inlining, const-call evaluation), and unrolling + closed
// forms + divmagic + input(); cx_tour (branchy unrolling + short-circuit)
// runs in the same battery at RUNTIME via cx_pipeline()/--selftest.

constexpr bool cx_tour() { // branches, unrolling, short-circuit, folding
  constexpr std::string_view src =
      "let s = 0;\n"
      "let i = 1;\n"
      "while (i <= 8) {\n"
      "  if (i % 2 == 0) { s = s + i; } else { s = s + 0; }\n"
      "  i = i + 1;\n"
      "}\n"
      "print s;\n"
      "print i;\n"
      "let d = 0;\n"
      "if (d != 0 && 100 / d > 1) { print 1; } else { print 2; }\n"
      "print 40 + 2;\n";
  Program P = compile(src);
  if (!P.ok)
    return false;
  RunOut oracle = run_ast(P.P.ast, P.P.root, 1000000, {});
  RunOut plain = run_prog(fns_of(P, 0), 1000000, {});
  RunOut opt = run_prog(fns_of(P, 1), 1000000, {});
  RunOut lowered = run_prog(fns_of(P, 2), 1000000, {});
  RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 1000000, {});
  if (!same_run(oracle, plain) || !same_run(oracle, opt))
    return false;
  if (!same_run(oracle, lowered) || !same_run(oracle, vm))
    return false;
  return oracle.out.size() == 4 && oracle.out[0] == 20 && // even sum
         oracle.out[1] == 9 && oracle.out[2] == 2 &&      // loop exit, sc
         oracle.out[3] == 42;                             // folded
}

constexpr bool cx_arrays() { // arena, lifting, len, handle-chained 2-D
  constexpr std::string_view src =
      "let a = array(4);\n"
      "let i = 0;\n"
      "while (i < 4) { a[i] = i * i; i = i + 1; }\n"
      "let t = 0;\n"
      "i = 0;\n"
      "while (i < 4) { t = t + a[i]; i = i + 1; }\n"
      "print t;\n"
      "print len(a);\n"
      "let m = array(2);\n"
      "m[0] = array(2);\n"
      "m[0][1] = 40;\n"
      "print m[0][1] + 2;\n";
  Program P = compile(src);
  if (!P.ok)
    return false;
  // the mjz_lib analyses really ran: a loop with nesting info
  bool loop_ok = false;
  An A = analyze(P.ssa_all[0]); // pre-opt main analysis (was Program member)
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h && A.nest[h] == 0)
      loop_ok = true;
  if (!loop_ok)
    return false;
  RunOut oracle = run_ast(P.P.ast, P.P.root, 1000000, {});
  RunOut plain = run_prog(fns_of(P, 0), 1000000, {});
  RunOut opt = run_prog(fns_of(P, 1), 1000000, {});
  RunOut lowered = run_prog(fns_of(P, 2), 1000000, {});
  RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 1000000, {});
  if (!same_run(oracle, plain) || !same_run(oracle, opt))
    return false;
  if (!same_run(oracle, lowered) || !same_run(oracle, vm))
    return false;
  return oracle.out.size() == 3 && oracle.out[0] == 14 && // 0+1+4+9
         oracle.out[1] == 4 &&                            // len(a)
         oracle.out[2] == 42;                             // m[0][1]+2
}

constexpr bool cx_fns() { // inlining, recursion, const-call evaluation
  constexpr std::string_view src =
      "fn add2(a, b) { return a + b; }\n"
      "fn fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); }\n"
      "fn is_even(n) { if (n == 0) { return 1; } return is_odd(n - 1); }\n"
      "fn is_odd(n) { if (n == 0) { return 0; } return is_even(n - 1); }\n"
      "print add2(fact(4), 16);\n"
      "print is_even(8) + is_odd(8);\n"
      "print is_even(9);\n"
      "print fact(10);\n";
  Program P = compile(src);
  if (!P.ok)
    return false;
  RunOut oracle = run_ast(P.P.ast, P.P.root, 1000000, {});
  RunOut plain = run_prog(fns_of(P, 0), 1000000, {});
  RunOut opt = run_prog(fns_of(P, 1), 1000000, {});
  RunOut lowered = run_prog(fns_of(P, 2), 1000000, {});
  RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 1000000, {});
  if (!same_run(oracle, plain) || !same_run(oracle, opt))
    return false;
  if (!same_run(oracle, lowered) || !same_run(oracle, vm))
    return false;
  return oracle.out.size() == 4 && oracle.out[0] == 40 && // inlined+folded
         oracle.out[1] == 1 && oracle.out[2] == 0 &&      // mutual recursion
         oracle.out[3] == 3628800;                        // const-called fact
}

constexpr bool cx_series() { // unroll, closed forms, divmagic, input()
  constexpr std::string_view src =
      "let x = input();\n"
      "let y = input();\n"
      "print x + y;\n"
      "let u = 0;\n"
      "let k = 0;\n"
      "while (k < 3) { u = u + k; k = k + 1; }\n"
      "print u;\n"
      "let g = 2;\n"
      "let kk = 0;\n"
      "while (kk < 30) { g = g * 3 + 1; kk = kk + 1; }\n"
      "print g;\n" // geometric closed form: 3^30*2 + (3^30-1)/2
      "let dd = input();\n"
      "print dd / 7;\n"
      "print dd % 7;\n"; // divmagic lowering, runtime dividend
  std::vector<val_t> tape{19, 23, 1000003};
  Program P = compile(src);
  if (!P.ok || !P.uses_input)
    return false;
  RunOut oracle = run_ast(P.P.ast, P.P.root, 1000000, tape);
  RunOut plain = run_prog(fns_of(P, 0), 1000000, tape);
  RunOut opt = run_prog(fns_of(P, 1), 1000000, tape);
  RunOut lowered = run_prog(fns_of(P, 2), 1000000, tape);
  RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 10000000, tape);
  if (!same_run(oracle, plain) || !same_run(oracle, opt))
    return false;
  if (!same_run(oracle, lowered) || !same_run(oracle, vm))
    return false;
  return oracle.out.size() == 5 && oracle.out[0] == 42 && // 19+23
         oracle.out[1] == 3 &&                            // unrolled + folded
         oracle.out[2] == 514727830236622ll &&            // closed form
         oracle.out[3] == 142857 && oracle.out[4] == 4;   // divmagic /7 %7
}

// runtime entry: the whole battery in one process (--selftest)

// (regression checks for the 2026-11 bug-hunt, kept as compile-time proofs)
constexpr bool cx_rings() { // the bug-3 repro family through the DEFAULT engine
  constexpr std::string_view src =
      "fn fa(n) { if (n == 0) { return 0; } if (n % 4 == 1) { return fa(n / "
      "4); } return fb(n / 4); }\n"
      "fn fb(n) { if (n == 0) { return 1; } if (n % 4 == 1) { return fa(n / "
      "4); } return fb(n / 4); }\n"
      "print fa(8);\n"  // 1: the last hop is fb->fb, a back edge into
                        // the sibling copy that does not re-execute
                        // the root edge's defining block -- exactly
                        // the 2026-11 bug-3 shape (fa(8) used to TRAP)
      "print fa(10);\n" // 1
      "print fa(9);\n"; // 1
  // ONE compile, ONE run (g++ retains the constant expression's whole
  // state until end-of-TU). The tailrec/tce_merge engine cannot run in a
  // static proof on this hardware (its constexpr state exceeds 4 GiB);
  // it is covered by the RUNTIME battery (cx_pipeline via --selftest, the
  // unit tests, and the differential fuzzer's pass rotations).
  Program P = compile(src); // default: the scctco engine
  if (!P.ok)
    return false;
  RunOut t = run_prog(fns_of(P, 1), 1000000, {});
  return t.out.size() == 3 && t.out[0] == 1 && t.out[1] == 1 && t.out[2] == 1;
}
constexpr bool cx_notcanon() { // comparison negation at the equality points
  constexpr std::string_view src =
      "let z = 0;\n"
      "print !(!(0 < 0));\n" // bug4 class: NOT(a>=b) must be a<b, not a<=b
      "print !(!(0 < 1));\n"
      "print !(!(1 < 0));\n"
      "print !(!(0 > 0));\n"
      "print !(!(0 >= 0));\n"
      "print !(!(0 <= 0));\n"
      "print !(0 >= 1);\n"
      "print !(1 > 0);\n"
      "print !(!(input() < z));\n" // equality point through input()
      "print !(!(input() < z));\n";
  std::vector<val_t> tape{0, -5}; // 0<0 -> 0; -5<0 -> 1 (transient cx alloc)
  Program P = compile(src);
  if (!P.ok)
    return false;
  RunOut oracle = run_ast(P.P.ast, P.P.root, 1000000, tape);
  RunOut opt = run_prog(fns_of(P, 1), 1000000, tape);
  RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 1000000, tape);
  if (!same_run(oracle, opt) || !same_run(oracle, vm))
    return false;
  return oracle.out.size() == 10 && oracle.out[0] == 0 && oracle.out[1] == 1 &&
         oracle.out[2] == 0 && oracle.out[3] == 0 && oracle.out[4] == 1 &&
         oracle.out[5] == 1 && oracle.out[6] == 1 && oracle.out[7] == 0 &&
         oracle.out[8] == 0 && oracle.out[9] == 1;
}

constexpr bool cx_bkpt() { // the breakpoint statement: invisible + durable
  // observed values (scalar + array + param) survive the whole -O3
  // pipeline: DCE roots, mem2reg keeps observed arrays in the arena, the
  // loop's breakpoints survive unrolling, and every runner treats them as
  // no-ops (output parity is the contract that makes them debug-only)
  constexpr std::string_view src =
      "fn add(x, y) { breakpoint x; return x + y; }\n"
      "let a = array(2);\n"
      "a[0] = 7;\n"
      "a[1] = 9;\n"
      "breakpoint a;\n"
      "let s = 0;\n"
      "let i = 0;\n"
      "while (i < 3) {\n"
      "  breakpoint s, i;\n"
      "  s = s + add(a[i % 2], i);\n"
      "  i = i + 1;\n"
      "}\n"
      "breakpoint s;\n"
      "print s;\n";
  Program P = compile(src);
  if (!P.ok)
    return false;
  RunOut oracle = run_ast(P.P.ast, P.P.root, 1000000, {});
  RunOut opt = run_prog(fns_of(P, 1), 1000000, {});
  RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 1000000, {});
  if (!same_run(oracle, opt) || !same_run(oracle, vm))
    return false;
  if (oracle.out.size() != 1 || oracle.out[0] != 7 + 10 + 9)
    return false; // 7+0, 9+1, 7+2
  // durability: the optimized IR still carries breakpoints with intact
  // observe lists, and the observed array stayed in the arena (no lift)
  uint32_t bkpts = 0, with_args = 0, allocs = 0;
  for (const Fn &F : fns_of(P, 1)) {
    for (const Instr &I : F.code) {
      if (I.k == K::Bkpt) {
        ++bkpts;
        if (!I.args.empty())
          ++with_args;
      }
      if (I.k == K::Alloc)
        ++allocs;
    }
  }
  if (bkpts < 4 || with_args < 4 || allocs == 0)
    return false; // unrolled copies count, but never fewer than the source
  // the bytecode encodes them as self-describing Bc::Bkpt ops
  std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
  uint32_t bc_bkpts = 0;
  for (size_t p = 4; p < bc.size(); ++p)
    if (bc[p] == (uint8_t)Bc::Bkpt)
      ++bc_bkpts;
  return bc_bkpts == bkpts;
}

constexpr bool cx_pipeline() {
  return cx_tour() && cx_arrays() && cx_fns() && cx_series();
}
// The whole pipeline — including SSA destruction and the bytecode VM — is
// constexpr. The static_asserts are enabled with -DMJZ_CX_ASSERT (needs
// generous -fconstexpr-depth / -fconstexpr-ops-limit; SEPARATE constant
// expressions because g++ retains each one's evaluation state — three fit
// in 4 GiB); --selftest always runs the FULL battery (all four programs)
// at runtime.
#ifdef MJZ_CX_ASSERT
static_assert(
    cx_arrays(),
    "MiniJZ pipeline (arena arrays + lib loop analyses) at compile time");
static_assert(cx_fns(),
              "MiniJZ pipeline (recursion/inline/const-call) at compile time");
static_assert(
    cx_series(),
    "MiniJZ pipeline (unroll/closed forms/divmagic/VM) at compile time");
#endif

// runtime entry for the app layer: constexpr/inline functions are only
// emitted where used, so the multi-TU build defines this alias exactly
// once in tu/cxtours.cpp; the header-only build inlines it right here
#ifdef MJZ_MULTI_CPP
bool cx_pipeline_rt(); // tu/cxtours.cpp
bool cx_bkpt_rt();     // tu/cxtours.cpp
#else
inline bool cx_pipeline_rt() { return cx_pipeline(); }
inline bool cx_bkpt_rt() { return cx_bkpt(); }
#endif
