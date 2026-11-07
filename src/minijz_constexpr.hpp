// ============================================================================
// MiniJZ compiler — §16 constexpr battery (the compile-time proof programs)
// ----------------------------------------------------------------------------
// Ordered-include header: this file is a slice of the original single
// translation unit and relies on being included after its predecessors
// (see src/minijz.hpp for the canonical order). Do not include it
// directly in an arbitrary order.
// ============================================================================
#pragma once

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
  if (!P.ok) return false;
  RunOut oracle = run_ast(P.P.ast, P.P.root, 1000000, {});
  RunOut plain = run_prog(fns_of(P, 0), 1000000, {});
  RunOut opt = run_prog(fns_of(P, 1), 1000000, {});
  RunOut lowered = run_prog(fns_of(P, 2), 1000000, {});
  RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 1000000, {});
  if (!same_run(oracle, plain) || !same_run(oracle, opt)) return false;
  if (!same_run(oracle, lowered) || !same_run(oracle, vm)) return false;
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
  if (!P.ok) return false;
  // the mjz_lib analyses really ran: a loop with nesting info
  bool loop_ok = false;
  for (uint32_t h = 0; h < P.an_pre_main.n; ++h)
    if (P.an_pre_main.inner[h] == h && P.an_pre_main.nest[h] == 0)
      loop_ok = true;
  if (!loop_ok) return false;
  RunOut oracle = run_ast(P.P.ast, P.P.root, 1000000, {});
  RunOut plain = run_prog(fns_of(P, 0), 1000000, {});
  RunOut opt = run_prog(fns_of(P, 1), 1000000, {});
  RunOut lowered = run_prog(fns_of(P, 2), 1000000, {});
  RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 1000000, {});
  if (!same_run(oracle, plain) || !same_run(oracle, opt)) return false;
  if (!same_run(oracle, lowered) || !same_run(oracle, vm)) return false;
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
  if (!P.ok) return false;
  RunOut oracle = run_ast(P.P.ast, P.P.root, 1000000, {});
  RunOut plain = run_prog(fns_of(P, 0), 1000000, {});
  RunOut opt = run_prog(fns_of(P, 1), 1000000, {});
  RunOut lowered = run_prog(fns_of(P, 2), 1000000, {});
  RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 1000000, {});
  if (!same_run(oracle, plain) || !same_run(oracle, opt)) return false;
  if (!same_run(oracle, lowered) || !same_run(oracle, vm)) return false;
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
  if (!P.ok || !P.uses_input) return false;
  RunOut oracle = run_ast(P.P.ast, P.P.root, 1000000, tape);
  RunOut plain = run_prog(fns_of(P, 0), 1000000, tape);
  RunOut opt = run_prog(fns_of(P, 1), 1000000, tape);
  RunOut lowered = run_prog(fns_of(P, 2), 1000000, tape);
  RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 10000000, tape);
  if (!same_run(oracle, plain) || !same_run(oracle, opt)) return false;
  if (!same_run(oracle, lowered) || !same_run(oracle, vm)) return false;
  return oracle.out.size() == 5 && oracle.out[0] == 42 && // 19+23
         oracle.out[1] == 3 &&                            // unrolled + folded
         oracle.out[2] == 514727830236622ll &&            // closed form
         oracle.out[3] == 142857 && oracle.out[4] == 4;   // divmagic /7 %7
}

// runtime entry: the whole battery in one process (--selftest)
constexpr bool cx_pipeline() {
  return cx_tour() && cx_arrays() && cx_fns() && cx_series();
}
// The whole pipeline — including SSA destruction and the bytecode VM — is
// constexpr. The static_asserts moved to constexpr_check.cpp (its own
// translation unit): g++ retains each constant expression's evaluation
// state until the end of the TU, and the tail-call-elimination pipeline
// grew the per-program footprint enough that several proofs in one TU no
// longer fit a 4 GiB budget. --selftest always runs the FULL battery (all
// four programs) at runtime.

// ============================================================================
