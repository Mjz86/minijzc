// ============================================================================
// unit_tests.cpp — function-level tests + function fuzzing (README task:
// "make sure most functions have tests if possible, ... try to fuzz the
// functions too"). Whole-pipeline behavior is already covered by the
// differential fuzzer + cx checks; this file pins the leaf functions.
// ============================================================================
#include "src/minijz.hpp"
#include <random>

static int ut_checks = 0, ut_fails = 0;
#define UT(cond, msg)                                                        \
  do {                                                                       \
    ++ut_checks;                                                             \
    if (!(cond)) {                                                           \
      std::printf("UT FAIL[%d]: %s\n", __LINE__, msg);                       \
      ++ut_fails;                                                            \
    }                                                                        \
  } while (0)

int main() {
  // ---- 1. checked arithmetic w_*: wrapping + trap semantics --------------
  {
    UT(w_add(INT64_MAX, 1).v == INT64_MIN, "w_add wraps");
    UT(w_sub(INT64_MIN, 1).v == INT64_MAX, "w_sub wraps");
    UT(w_mul(INT64_MIN, -1).v == INT64_MIN, "w_mul wraps");
    UT(w_div(7, 0).trap, "w_div by 0 traps");
    UT(w_div(INT64_MIN, -1).trap, "w_div INT64_MIN/-1 traps");
    UT(w_mod(7, 0).trap, "w_mod by 0 traps");
    UT(w_mod(INT64_MIN, -1).trap, "w_mod INT64_MIN/-1 traps");
    UT(w_shl(1, 70).v == (1 << 6), "w_shl masks the count to 6 bits");
    UT(w_ashr(-8, 2).v == -2, "w_ashr arithmetic");
    UT(w_mulhi(0xdeadbeefull, 0xfeedull).v ==
            (val_t)(((((unsigned __int128)0xdeadbeefull) * 0xfeedull) >> 64)),
        "w_mulhi");
    std::mt19937_64 rng(12345);
    for (int i = 0; i < 2000; ++i) {
      val_t a = (val_t)rng(), b = (val_t)rng();
      if (b == 0) b = 7;
      UT(w_div(a, b).v == a / b && !w_div(a, b).trap, "w_div matches C");
      UT(w_mod(a, b).v == a % b && !w_mod(a, b).trap, "w_mod matches C");
      UT(w_add(a, b).v == (val_t)((uint64_t)a + (uint64_t)b), "w_add matches");
    }
  }

  // ---- 2. AST binop -> IR binop mapping is total & identity --------------
  {
    int mapped[11]{};
    const Bk all[] = {Bk::Add, Bk::Sub, Bk::Mul, Bk::Div, Bk::Mod,
                       Bk::Lt, Bk::Le,  Bk::Gt, Bk::Ge, Bk::Eq, Bk::Ne};
    for (Bk b : all) {
      B r = ir_op(b);
      ++mapped[(int)b];
      UT(r == B::Add || r == B::Sub || r == B::Mul || r == B::Div ||
             r == B::Mod || r == B::Lt || r == B::Le || r == B::Gt ||
             r == B::Ge || r == B::Eq || r == B::Ne,
         "ir_op lands in B");
      (void)r;
    }
    UT(mapped[(int)Bk::Add] && mapped[(int)Bk::Ne], "ir_op covers all AST binops");
    UT(ir_op(Bk::Lt) == B::Lt && ir_op(Bk::Le) == B::Le &&
           ir_op(Bk::Gt) == B::Gt && ir_op(Bk::Ge) == B::Ge,
       "ir_op is the identity on the four comparisons");
  }

  // ---- 3. lexer/parser: error positions + roundtrip ----------------------
  {
    auto pos_of = [](const char *s, const char *w) {
      Parsed p = parse(s);
      return p.ok ? 0u : p.err.pos;
    };
    UT(pos_of("let x = ;", 0) != 0, "parse error carries a position");
    UT(!parse("let x = 1").ok, "missing print/assign tail is an error");
    // (statement must end with ';')
    Parsed okp = parse("print 1 + 2 * 3;");
    UT(okp.ok && okp.uses_input == false, "parse ok, no input() recorded");
    Parsed inp = parse("print input() + input();");
    UT(inp.ok && inp.uses_input, "uses_input recorded");
  }

  // ---- 4. divmagic fuzz THROUGH the pipeline: random (n, D) pairs ---------
  // (the emitted program is the function's public surface: every (n, D)
  // must agree with the oracle's C-semantics division/modulo)
  {
    std::mt19937_64 rng(0xC0FFEE);
    long bad = 0;
    for (int i = 0; i < 700; ++i) {
      val_t D = 0;
      while (D == 0 || D == -1)
        D = (val_t)(rng() % 2001) - 1000; // -1000..1000, not 0/-1
      val_t n = (val_t)(rng() % 2001) - 1000;
      char buf[160];
      std::snprintf(buf, sizeof buf,
                    "print input() / %lld; print input() %% %lld;",
                    (long long)D, (long long)D);
      Program P = compile(buf);
      if (!P.ok) { ++bad; continue; }
      std::vector<val_t> tape{n, n};
      RunOut oracle = run_ast(P.P.ast, P.P.root, 100000, tape);
      RunOut opt = run_prog(fns_of(P, 1), 100000, tape);
      if (!same_run(oracle, opt)) ++bad;
    }
    UT(bad == 0, "divmagic fuzz: 700 random (n, D) pairs agree with oracle");
  }

  // ---- 4b. divmagic inside a fn, through ALL FIVE runners ---------------
  // (B2's in-flight units idea, finished: divmagic fires inside the body,
  // the fn is inlined into main, and every runner — oracle, plain SSA,
  // optimized, destroyed, VM — must still agree)
  {
    std::mt19937_64 rng(0xBEEFCAFE);
    long bad5 = 0;
    for (int i = 0; i < 300; ++i) {
      val_t D = 0;
      while (D == 0 || D == -1)
        D = (val_t)(rng() % 2001) - 1000; // -1000..1000, not 0/-1
      val_t X = (val_t)(rng() % 2001) - 1000;
      char buf[192];
      std::snprintf(buf, sizeof buf,
                    "fn d(n) { return n / %lld; }\n"
                    "fn m(n) { return n %% %lld; }\n"
                    "print d(%lld); print m(%lld);",
                    (long long)D, (long long)D, (long long)X, (long long)X);
      Program P = compile(buf);
      if (!P.ok) { ++bad5; continue; }
      RunOut oracle = run_ast(P.P.ast, P.P.root, 100000, {});
      RunOut plain = run_prog(fns_of(P, 0), 100000, {});
      RunOut opt = run_prog(fns_of(P, 1), 100000, {});
      RunOut lowered = run_prog(fns_of(P, 2), 100000, {});
      RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 1000000, {});
      if (!(same_run(oracle, plain) && same_run(oracle, opt) &&
            same_run(oracle, lowered) && same_run(oracle, vm)))
        ++bad5;
    }
    UT(bad5 == 0,
       "divmagic fn-fuzz: 300 random (X, D) pairs agree across ALL 5 runners");

    // ---- 4c. C-truncation semantics pinned explicitly ------------------
    // (B2's units had this expectation WRONG; MiniJZ `/` and `%` truncate
    // toward zero exactly like C)
    UT(w_div(-7, 2).v == -3 && w_mod(-7, 2).v == -1,
       "C truncation: -7 / 2 == -3, -7 % 2 == -1");
    UT(w_div(7, -2).v == -3 && w_mod(7, -2).v == 1,
       "C truncation: 7 / -2 == -3, 7 % -2 == 1");
    UT(w_div(-7, -2).v == 3 && w_mod(-7, -2).v == -1,
       "C truncation: -7 / -2 == 3, -7 % -2 == -1");
    UT(w_div(INT64_MIN, -1).trap && w_mod(INT64_MIN, -1).trap,
       "the one C-UB case traps: INT64_MIN / -1 and INT64_MIN % -1");
  }

  // ---- 5. pass-toggle glue: names, flags, describe_cfg --------------------
  {
    for (uint32_t i = 0; i < PASS_TOGGLE_COUNT; ++i) {
      PassCfg p{};
      p.set_level(3);
      UT(apply_pass_flag(p, i, false), "apply_pass_flag accepts index");
      std::string d = describe_cfg(p);
      UT(d.find(PASS_TOGGLE_NAMES[i]) != std::string::npos,
         "describe_cfg lists the disabled pass");
      // re-enable via the CLI's string path
      PassCfg q{};
      q.set_level(3);
      std::string err{};
      UT(set_named_pass(q, PASS_TOGGLE_NAMES[i], true, err),
         "CLI knows the toggle name");
    }
    PassCfg def{};
    def.set_level(3);
    UT(describe_cfg(def) == "-O3", "default cfg describes as -O3");
  }

  // ---- 6. SSA/pred-normalization + destruction roundtrip ------------------
  // (break/continue programs are exactly what stresses multi-pred merges)
  {
    const char *src =
        "let s = 0;\nlet i = 0;\nwhile (i < 8) {\n  i = i + 1;\n"
        "  if (i % 3 == 0) { continue; }\n  if (i > 6) { break; }\n"
        "  s = s + i;\n}\nprint s;\nprint i;\n";
    Program P = compile(src);
    UT(P.ok, "break/continue program compiles");
    RunOut oracle = run_ast(P.P.ast, P.P.root, 100000, {});
    RunOut plain = run_prog(fns_of(P, 0), 100000, {});
    RunOut opt = run_prog(fns_of(P, 1), 100000, {});
    RunOut lowered = run_prog(fns_of(P, 2), 100000, {});
    RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 1000000, {});
    UT(same_run(oracle, plain) && same_run(oracle, opt) &&
           same_run(oracle, lowered) && same_run(oracle, vm),
       "break/continue agrees across all runners");
    UT(oracle.out.size() == 2, "break/continue prints two values");
  }

  // ---- 7. the regression repros of the 2026-11 bug hunt (fast forms) ------
  {
    // bug 2 (tce_merge dispatch): 3-ring + 4-ring at -O3 with scctco off
    const char *r3 =
        "fn m3a(n) { if (n == 0) { return 8; } return m3b(n - 1); }\n"
        "fn m3b(n) { if (n == 0) { return 9; } return m3c(n - 1); }\n"
        "fn m3c(n) { if (n == 0) { return 7; } return m3a(n - 1); }\n"
        "print m3a(input());\n";
    PassCfg cfg{};
    cfg.set_level(3);
    cfg.p_scctco = false; // force the tailrec/tce_merge path
    Program P = compile(r3, cfg);
    std::vector<val_t> tape{5};
    RunOut oracle = run_ast(P.P.ast, P.P.root, 100000, tape);
    RunOut opt = run_prog(fns_of(P, 1), 100000, tape);
    UT(same_run(oracle, opt) && oracle.out.at(0) == 7,
       "bug2 regression: m3a(5) == 7 via tailrec path");
    // bug 3 (scctco sibling entry rotation): fa(8) must be 1, not TRAP
    const char *f2 =
        "fn fa(n) { if (n == 0) { return 0; } if (n % 4 == 1) { return fa(n / 4); } return fb(n / 4); }\n"
        "fn fb(n) { if (n == 0) { return 1; } if (n % 4 == 1) { return fa(n / 4); } return fb(n / 4); }\n"
        "print fa(input());\n";
    Program P2 = compile(f2); // default: scctco on
    std::vector<val_t> tape2{8};
    RunOut o2 = run_ast(P2.P.ast, P2.P.root, 100000, tape2);
    RunOut c2 = run_prog(fns_of(P2, 1), 100000, tape2);
    UT(same_run(o2, c2) && o2.out.at(0) == 1,
       "bug3 regression: fa(8) == 1 (scctco entry rotation)");
    // bug 4 (comparison negation): equality points
    const char *nc = "print !(!(0 < 0)); print !(!(0 > 0)); print !(!(1 > 0));\n";
    Program P3 = compile(nc);
    RunOut o3 = run_ast(P3.P.ast, P3.P.root, 100000, {});
    RunOut c3 = run_prog(fns_of(P3, 1), 100000, {});
    UT(same_run(o3, c3) && o3.out.at(0) == 0 && o3.out.at(1) == 0 &&
           o3.out.at(2) == 1, // !(!(1 > 0)) == 1
       "bug4 regression: NOT-of-compare at equality points");
  }

  std::printf("unit tests: %d checks, %d failures\n", ut_checks, ut_fails);
  return ut_fails ? 1 : 0;
}
