// ============================================================================
// unit_tests.cpp — function-level tests + function fuzzing (README task:
// "make sure most functions have tests if possible, ... try to fuzz the
// functions too"). Whole-pipeline behavior is already covered by the
// differential fuzzer + cx checks; this file pins the leaf functions.
// ============================================================================
#include "../src/minijz.hpp"
#include <random>

static int ut_checks = 0, ut_fails = 0;
#define UT(cond, msg)                                                          \
  do {                                                                         \
    ++ut_checks;                                                               \
    if (!(cond)) {                                                             \
      std::printf("UT FAIL[%d]: %s\n", __LINE__, msg);                         \
      ++ut_fails;                                                              \
    }                                                                          \
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
      if (b == 0)
        b = 7;
      UT(w_div(a, b).v == a / b && !w_div(a, b).trap, "w_div matches C");
      UT(w_mod(a, b).v == a % b && !w_mod(a, b).trap, "w_mod matches C");
      UT(w_add(a, b).v == (val_t)((uint64_t)a + (uint64_t)b), "w_add matches");
    }
  }

  // ---- 2. AST binop -> IR binop mapping is total & identity --------------
  {
    int mapped[11]{};
    const Bk all[] = {Bk::Add, Bk::Sub, Bk::Mul, Bk::Div, Bk::Mod, Bk::Lt,
                      Bk::Le,  Bk::Gt,  Bk::Ge,  Bk::Eq,  Bk::Ne};
    for (Bk b : all) {
      B r = ir_op(b);
      ++mapped[(int)b];
      UT(r == B::Add || r == B::Sub || r == B::Mul || r == B::Div ||
             r == B::Mod || r == B::Lt || r == B::Le || r == B::Gt ||
             r == B::Ge || r == B::Eq || r == B::Ne,
         "ir_op lands in B");
      (void)r;
    }
    UT(mapped[(int)Bk::Add] && mapped[(int)Bk::Ne],
       "ir_op covers all AST binops");
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
      if (!P.ok) {
        ++bad;
        continue;
      }
      std::vector<val_t> tape{n, n};
      RunOut oracle = run_ast(P.P.ast, P.P.root, 100000, tape);
      RunOut opt = run_prog(fns_of(P, 1), 100000, tape);
      if (!same_run(oracle, opt))
        ++bad;
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
      if (!P.ok) {
        ++bad5;
        continue;
      }
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
    const char *f2 = "fn fa(n) { if (n == 0) { return 0; } if (n % 4 == 1) { "
                     "return fa(n / 4); } return fb(n / 4); }\n"
                     "fn fb(n) { if (n == 0) { return 1; } if (n % 4 == 1) { "
                     "return fa(n / 4); } return fb(n / 4); }\n"
                     "print fa(input());\n";
    Program P2 = compile(f2); // default: scctco on
    std::vector<val_t> tape2{8};
    RunOut o2 = run_ast(P2.P.ast, P2.P.root, 100000, tape2);
    RunOut c2 = run_prog(fns_of(P2, 1), 100000, tape2);
    UT(same_run(o2, c2) && o2.out.at(0) == 1,
       "bug3 regression: fa(8) == 1 (scctco entry rotation)");
    // bug 4 (comparison negation): equality points
    const char *nc =
        "print !(!(0 < 0)); print !(!(0 > 0)); print !(!(1 > 0));\n";
    Program P3 = compile(nc);
    RunOut o3 = run_ast(P3.P.ast, P3.P.root, 100000, {});
    RunOut c3 = run_prog(fns_of(P3, 1), 100000, {});
    UT(same_run(o3, c3) && o3.out.at(0) == 0 && o3.out.at(1) == 0 &&
           o3.out.at(2) == 1, // !(!(1 > 0)) == 1
       "bug4 regression: NOT-of-compare at equality points");
  }

  // ---- N. the breakpoint statement (debug-only observation points) --------
  {
    // (a) invisibility: adding breakpoints must not change ANY runner's
    // output — the same program with and without `breakpoint` lines
    const char *plain = "fn add(x, y) { return x + y; }\n"
                        "let a = array(2);\n"
                        "a[0] = 7;\n"
                        "a[1] = 9;\n"
                        "let s = 0;\n"
                        "let i = 0;\n"
                        "while (i < 3) {\n"
                        "  s = s + add(a[i % 2], i);\n"
                        "  i = i + 1;\n"
                        "}\n"
                        "print s;\n";
    const char *brked = "fn add(x, y) { breakpoint x; return x + y; }\n"
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
    Program Pp = compile(plain);
    Program Pb = compile(brked);
    UT(Pp.ok && Pb.ok, "both breakpoint variants compile");
    RunOut want = run_prog(fns_of(Pp, 1), 1000000, {});
    RunOut oracle = run_ast(Pb.P.ast, Pb.P.root, 1000000, {});
    RunOut plainS = run_prog(fns_of(Pb, 0), 1000000, {});
    RunOut opt = run_prog(fns_of(Pb, 1), 1000000, {});
    RunOut low = run_prog(fns_of(Pb, 2), 1000000, {});
    RunOut vm = vm_run(emit_bytecode(fns_of(Pb, 2)), 1000000, {});
    UT(same_run(want, oracle) && same_run(want, plainS) &&
           same_run(want, opt) && same_run(want, low) && same_run(want, vm),
       "breakpoints are no-ops in every runner (parity)");
    UT(want.out.size() == 1 && want.out[0] == 26, "expected value 26");
    // (b) durability: breakpoints + their observe lists survive -O3
    uint32_t bks = 0, with_args = 0, allocs = 0;
    for (const Fn &F : fns_of(Pb, 1))
      for (const Instr &I : F.code) {
        if (I.k == K::Bkpt) {
          ++bks;
          if (!I.args.empty())
            ++with_args;
        }
        if (I.k == K::Alloc)
          ++allocs;
      }
    UT(bks >= 4, "breakpoints survive the optimizer (>= 4 sites)");
    UT(with_args >= 4, "observe lists survive the optimizer");
    UT(allocs >= 1, "observed array stays in the arena (not lifted)");
    // (c) deadargs keeps a breakpoint-observed param alive
    {
      PassCfg cfg{};
      cfg.set_level(3);
      cfg.do_inline = false; // keep the fn around so nparams is observable
      cfg.p_deadargs = true;
      const char *da = "fn f(x, y) { breakpoint x; return y; }\n"
                       "print f(input(), 3);\n";
      Program Pd = compile(da, cfg);
      UT(Pd.ok, "deadargs+breakpoint program compiles");
      bool kept = false;
      for (const FnUnit &U : Pd.fns) // the surviving f: its observed param
        if (U.opt.nparams >= 2)      // must still be a parameter (x kept)
          for (const Instr &I : U.opt.code)
            if (I.k == K::Bkpt && !I.args.empty())
              kept = true;
      UT(kept, "deadargs keeps breakpoint-observed params (hard use)");
      RunOut od = run_ast(Pd.P.ast, Pd.P.root, 100000, {5});
      RunOut cd = run_prog(fns_of(Pd, 1), 100000, {5});
      UT(same_run(od, cd) && cd.out.at(0) == 3, "observed-param fn runs");
    }
    // (d) bare breakpoints are legal everywhere a statement is
    {
      const char *bare = "breakpoint;\nif (1) { breakpoint; }\nprint 1;\n";
      Program Pb2 = compile(bare);
      UT(Pb2.ok, "bare breakpoint compiles");
      RunOut ob = run_prog(fns_of(Pb2, 1), 100000, {});
      UT(ob.out.size() == 1 && ob.out[0] == 1, "bare breakpoint runs");
      uint32_t n = 0;
      for (const Fn &F : fns_of(Pb2, 1))
        for (const Instr &I : F.code)
          if (I.k == K::Bkpt)
            ++n;
      UT(n == 2, "both bare breakpoints survive (DCE roots)");
    }
    // (e) parse errors for the statement itself
    {
      Program Pe = compile("breakpoint nosuch;\n");
      UT(!Pe.ok && Pe.errpos != 0, "breakpoint observes must be declared");
      Program Pe2 = compile("breakpoint 5;\n");
      UT(!Pe2.ok, "breakpoint list takes names, not expressions");
    }
    // (f) diagnostics carry positions (located errors + traps)
    {
      Program Pe = compile("print zzz;\n");
      UT(!Pe.ok && Pe.errpos == 6, "undeclared var error position");
      Program Pa = compile("fn f(a, b) { return a; }\nprint f(1);\n");
      UT(!Pa.ok && Pa.errpos == 31, "arity error at the call site");
      const char *dz = "let z = 0;\nprint 5 / z;\n";
      Program Pz = compile(dz);
      RunOut rz = run_prog(fns_of(Pz, 1), 100000, {});
      UT(rz.trapped && rz.trap_src != NONE && rz.trap_kind &&
             std::string(rz.trap_kind) == "division by zero",
         "located trap: kind + source offset recorded");
    }
  }

  // ---- gvn: value numbering (consts + phi congruence + bins) ---------------
  {
    // (a) congruent merge phis unify: two vars tracking the same values
    // through the same if/else are ONE value after -O3, and the sum of
    // the pair doubles one phi instead of adding two distinct ones
    {
      const char *s = "let a = 0;\nlet b = 0;\n"
                      "if (input()) { a = 5; b = 5; } else { a = 7; b = 7; }\n"
                      "print a + b;\n";
      Program P = compile(s);
      UT(P.ok, "gvn: shadow-pair program compiles");
      RunOut o = run_prog(fns_of(P, 1), 100000, {1});
      UT(o.out.size() == 1 && o.out[0] == 10, "gvn: shadow-pair result (10)");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {0});
      UT(o2.out.size() == 1 && o2.out[0] == 14, "gvn: shadow-pair result (14)");
      OptStats os = sum_ostats(P);
      UT(os.gvns > 0, "gvn: the pass actually unified something");
    }
    // (b) duplicate constants across blocks fold to one def
    {
      const char *s = "let x = 3;\nif (input()) { x = 9; } else { x = 9; }\n"
                      "print x;\n";
      Program P = compile(s);
      UT(P.ok, "gvn: dup-const program compiles");
      OptStats os = sum_ostats(P);
      UT(os.gvns > 0, "gvn: duplicate consts were unified");
      RunOut o = run_prog(fns_of(P, 1), 100000, {1});
      UT(o.out.size() == 1 && o.out[0] == 9, "gvn: dup-const result");
    }
    // (c) a loop's counter and accumulator must NOT unify (the regression
    // that motivated the conservative taint guard: phis referencing each
    // other's results stay put)
    {
      const char *s = "let x = 0;\nlet i = 0;\n"
                      "while (i < 4) { x = x + i; i = i + 1; }\n"
                      "print x;\n";
      Program P = compile(s);
      UT(P.ok, "gvn: loop program compiles");
      RunOut o = run_prog(fns_of(P, 1), 200000, {});
      UT(o.out.size() == 1 && o.out[0] == 6, "gvn: loop still counts to 6");
    }
    // (d) the toggle: --disable=gvn keeps the program correct, just fatter
    {
      const char *s = "let a = 0;\nlet b = 0;\n"
                      "if (input()) { a = 5; b = 5; } else { a = 7; b = 7; }\n"
                      "print a + b;\n";
      PassCfg cfg{};
      cfg.set_level(3);
      cfg.p_gvn = false;
      Program P = compile(s, cfg);
      OptStats os = sum_ostats(P);
      UT(os.gvns == 0, "gvn: the toggle turns the pass off");
      RunOut o = run_prog(fns_of(P, 1), 100000, {1});
      UT(o.out.size() == 1 && o.out[0] == 10,
         "gvn: disabled pass, same result");
    }
  }

  // ---- unswitch + lsimp: loop versioning and canonicalization -------------
  {
    // (a) trivial unswitch: an invariant header condition versions the
    // loop (the false path skips it, the true clone drops the test);
    // values live past the loop keep their per-path meanings
    {
      const char *s = "let f = input();\nlet c = 3;\n"
                      "while (f > 0) {\n  c = c - 1;\n"
                      "  if (c == 0) { break; }\n  print c;\n}\n"
                      "print 1000 + c;\n";
      Program P = compile(s);
      UT(P.ok, "unswitch: invariant-flag loop compiles");
      OptStats os = sum_ostats(P);
      UT(os.unswitches > 0, "unswitch: the pass actually versioned a loop");
      RunOut o = run_prog(fns_of(P, 1), 100000, {0});
      UT(o.out.size() == 1 && o.out[0] == 1003,
         "unswitch: false path = zero iterations (1003)");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {5});
      UT(o2.out.size() == 3 && o2.out[0] == 2 && o2.out[1] == 1 &&
             o2.out[2] == 1000,
         "unswitch: true path runs the body (2 1 1000)");
    }
    // (b) a modified condition is NOT invariant: no versioning, same math
    {
      const char *s = "let f = input();\nlet s = 0;\n"
                      "while (f > 0) { s = s + 3; f = f - 1; }\n"
                      "print s;\n";
      Program P = compile(s);
      UT(P.ok, "unswitch: countdown compiles");
      OptStats os = sum_ostats(P);
      UT(os.unswitches == 0, "unswitch: non-invariant cond left alone");
      RunOut o = run_prog(fns_of(P, 1), 100000, {4});
      UT(o.out.size() == 1 && o.out[0] == 12, "unswitch: countdown result");
    }
    // (c) the break-style trip polarity: a constant while-test folded away
    // leaves the `break` condition as the header test; the trip count
    // must be computed with the EXIT-on-true polarity (the latent bug the
    // unswitch bait window exposed)
    {
      const char *s = "let c = 5;\n"
                      "while (3 > 0) {\n"
                      "  if (c <= 0) { break; }\n"
                      "  c = c - 1;\n"
                      "  print 7;\n"
                      "}\n";
      Program P = compile(s);
      UT(P.ok, "unswitch: break-style loop compiles");
      RunOut o = run_prog(fns_of(P, 1), 100000, {});
      UT(o.out.size() == 5, "unswitch: break-style trip count is 5");
    }
    // (d) lsimp: a loop-carried phi whose update folded to itself
    // dissolves to its entry value (dce's cascade usually wins the race;
    // the lsimp toggle must at least never break the program)
    {
      const char *s = "let k = input();\nlet c = 4;\n"
                      "while (c > 0) { k = k + 0; c = c - 1; }\n"
                      "print k;\n";
      Program P = compile(s);
      UT(P.ok, "lsimp: trivial-phi loop compiles");
      RunOut o = run_prog(fns_of(P, 1), 100000, {9});
      UT(o.out.size() == 1 && o.out[0] == 9, "lsimp: k keeps its entry value");
    }
    // (e) licm's new header scan: the invariant loop test hoists out of
    // the header (one compare saved per iteration, the unswitch enabler)
    {
      const char *s = "let f = input();\nlet c = 3;\n"
                      "while (f > 0) {\n  if (c <= 0) { break; }\n"
                      "  c = c - 1;\n}\n"
                      "print c;\n";
      Program P = compile(s);
      UT(P.ok, "licm: header-hoist program compiles");
      OptStats os = sum_ostats(P);
      UT(os.hoisted > 0, "licm: the header test hoisted to the preheader");
      RunOut o = run_prog(fns_of(P, 1), 100000, {2});
      UT(o.out.size() == 1 && o.out[0] == 0,
         "licm: hoisted test, same trip (3 decrements to 0)");
    }
  }

  // ---- checkpoint 18: compound-condition loop unswitching -----------------
  {
    // (a) `while (a && b)` with both flags invariant: the whole guard
    // chain is versioned (preheader cbr a -> g2: cbr b -> clone / fx ->
    // exit); all three polarity paths keep the original semantics and
    // the value live past the loop is the ENTRY value on zero-iteration
    // paths
    {
      const char *s = "let f = input();\nlet g = input();\nlet c = 5;\n"
                      "while (f > 0 && g > 0) {\n  if (c <= 0) { break; }\n"
                      "  c = c - 1;\n  print c;\n}\n"
                      "print 1000 + c;\n";
      Program P = compile(s);
      UT(P.ok, "cc-unswitch: && program compiles");
      OptStats os = sum_ostats(P);
      UT(os.unswitches > 0, "cc-unswitch: the && guard chain versioned");
      RunOut o = run_prog(fns_of(P, 1), 100000, {3, 2});
      UT(o.out.size() == 6 && o.out[0] == 4 && o.out[4] == 0 &&
             o.out[5] == 1000,
         "cc-unswitch: && true path runs the body (4..0, 1000)");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {0, 2});
      UT(o2.out.size() == 1 && o2.out[0] == 1005,
         "cc-unswitch: && first-false = zero iterations (entry c)");
      RunOut o3 = run_prog(fns_of(P, 1), 100000, {3, 0});
      UT(o3.out.size() == 1 && o3.out[0] == 1005,
         "cc-unswitch: && second-false = zero iterations (entry c)");
    }
    // (b) `while (a || b)`: the true arms short-circuit into the loop,
    // both-false exits at zero iterations
    {
      const char *s = "let f = input();\nlet g = input();\nlet c = 5;\n"
                      "while (f > 0 || g > 0) {\n  if (c <= 0) { break; }\n"
                      "  c = c - 1;\n  print c;\n}\n"
                      "print 1000 + c;\n";
      Program P = compile(s);
      UT(P.ok, "cc-unswitch: || program compiles");
      OptStats os = sum_ostats(P);
      UT(os.unswitches > 0, "cc-unswitch: the || guard chain versioned");
      RunOut o = run_prog(fns_of(P, 1), 100000, {0, 5});
      UT(o.out.size() == 6 && o.out[5] == 1000,
         "cc-unswitch: || one-true path runs the body");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {0, 0});
      UT(o2.out.size() == 1 && o2.out[0] == 1005,
         "cc-unswitch: || both-false = zero iterations");
      RunOut o3 = run_prog(fns_of(P, 1), 100000, {7, 0});
      UT(o3.out.size() == 6 && o3.out[5] == 1000,
         "cc-unswitch: || other-true path runs the body");
    }
    // (c) a WRITTEN flag is not invariant: no versioning, same math
    // (the counter still bounds the loop; f's decrement terminates it)
    {
      const char *s = "let f = input();\nlet g = input();\nlet s = 0;\n"
                      "while (f > 0 && g > 0) { s = s + 2; f = f - 1; }\n"
                      "print s;\n";
      Program P = compile(s);
      UT(P.ok, "cc-unswitch: non-invariant && compiles");
      OptStats os = sum_ostats(P);
      UT(os.unswitches == 0, "cc-unswitch: written cond left alone");
      RunOut o = run_prog(fns_of(P, 1), 100000, {4, 9});
      UT(o.out.size() == 1 && o.out[0] == 8,
         "cc-unswitch: non-invariant && result (4 trips x 2)");
    }
    // (d) the && guard around a division: the guard is control-flow
    // sensitive, the condition chain is pure, the versioning must not
    // move the division (the && diamond's blocks are pure; this pins
    // the division staying under the guard; the counter bounds the loop)
    {
      const char *s = "let z = input();\nlet s = 0;\n"
                      "let fl = input();\nlet gl = input();\nlet c = 3;\n"
                      "while (fl > 0 && gl > 0) {\n"
                      "  if (z != 0) { s = s + 100 / z; }\n"
                      "  s = s + 1;\n"
                      "  c = c - 1;\n"
                      "  if (c <= 0) { break; }\n}\n"
                      "print s;\n";
      Program P = compile(s);
      UT(P.ok, "cc-unswitch: guarded-division && compiles");
      RunOut o = run_prog(fns_of(P, 1), 100000, {0, 1, 1});
      UT(o.out.size() == 1 && o.out[0] == 3,
         "cc-unswitch: z=0 guards the division (3 iters x +1)");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {2, 1, 1});
      UT(o2.out.size() == 1 && o2.out[0] == 153,
         "cc-unswitch: z=2 divides per iteration (3 x 51)");
    }
  }

  // ---- checkpoint 19: body-if loop unswitching -------------------------
  {
    // (a) the canonical bait: the head test is a real while-condition and
    // the body hides an invariant if. BOTH versions keep running the
    // loop, and the exit value of cn DIFFERS between them (the +1 side
    // lands exactly on the bound, the +2 side can overshoot by one) —
    // the version-merge blocks carry it, so this is load-bearing
    {
      const char *s = "let bf = input();\nlet cn = 0;\n"
                      "while (cn < 5) {\n"
                      "  if (bf > 0) { cn = cn + 1; } else { cn = cn + 2; }\n"
                      "}\n"
                      "print cn;\n";
      Program P = compile(s);
      UT(P.ok, "bif-unswitch: variant-head program compiles");
      OptStats os = sum_ostats(P);
      UT(os.unswitches > 0, "bif-unswitch: the loop was versioned");
      UT(os.body_unswitches > 0, "bif-unswitch: the body-if form fired");
      RunOut o = run_prog(fns_of(P, 1), 100000, {3});
      UT(o.out.size() == 1 && o.out[0] == 5,
         "bif-unswitch: true version steps +1 to exactly 5");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {0});
      UT(o2.out.size() == 1 && o2.out[0] == 6,
         "bif-unswitch: false version overshoots to 6 (the VM merge)");
    }
    // (b) per-iteration observation inside the loop: both versions print
    // their own strides; the outputs differ per version and must match
    // the oracle either way
    {
      const char *s = "let bf = input();\nlet cn = 0;\n"
                      "while (cn < 6) {\n"
                      "  if (bf > 0) { cn = cn + 1; print cn; }\n"
                      "  else { cn = cn + 3; print cn; }\n"
                      "}\n";
      Program P = compile(s);
      UT(P.ok, "bif-unswitch: arm-print program compiles");
      OptStats os = sum_ostats(P);
      UT(os.body_unswitches > 0, "bif-unswitch: arm prints do not block");
      RunOut o = run_prog(fns_of(P, 1), 100000, {5});
      UT(o.out.size() == 6 && o.out[0] == 1 && o.out[5] == 6,
         "bif-unswitch: +1 version prints 1..6");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {0});
      UT(o2.out.size() == 2 && o2.out[0] == 3 && o2.out[1] == 6,
         "bif-unswitch: +3 version prints 3, 6");
    }
    // (c) the break-arm read: the guarded break's arm READS the loop
    // counter (a plain outside use of a loop-defined value) — the VM
    // below the break arm must merge it, and the post-loop print reads
    // a 2-pred merge (the header exit AND the break arm) whose delivered
    // phi is born at the merge
    {
      const char *s = "let bf = input();\nlet c = 5;\nlet s = 0;\n"
                      "while (c > 1) {\n"
                      "  if (c == 2) { print 900 + c; break; }\n"
                      "  if (bf > 0) { c = c - 1; } else { c = c - 2; }\n"
                      "  s = s + c;\n"
                      "}\n"
                      "print s * 10 + c;\n";
      Program P = compile(s);
      UT(P.ok, "bif-unswitch: break-arm-read program compiles");
      OptStats os = sum_ostats(P);
      UT(os.body_unswitches > 0, "bif-unswitch: break arms do not block");
      RunOut o = run_prog(fns_of(P, 1), 100000, {3});
      UT(o.out.size() == 2 && o.out[0] == 902 && o.out[1] == 92,
         "bif-unswitch: true version breaks at 2 (902, then 92)");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {0});
      UT(o2.out.size() == 1 && o2.out[0] == 41,
         "bif-unswitch: false version runs to the header exit (41)");
    }
    // (d) rejection: a WRITTEN flag is not invariant — no versioning,
    // same math (the counter still bounds the loop)
    {
      const char *s =
          "let cn = 0;\n"
          "while (cn < 5) {\n"
          "  if (cn % 2 == 0) { cn = cn + 1; } else { cn = cn + 2; }\n"
          "}\n"
          "print cn;\n";
      Program P = compile(s);
      UT(P.ok, "bif-unswitch: non-invariant branch compiles");
      OptStats os = sum_ostats(P);
      UT(os.unswitches == 0, "bif-unswitch: varying cond left alone");
      RunOut o = run_prog(fns_of(P, 1), 100000, {});
      UT(o.out.size() == 1 && o.out[0] == 5,
         "bif-unswitch: 0,1,3,5 lands on 5");
    }
    // (e) rejection: an arm that BREAKS leaves the loop (the then-arm is
    // outside the natural loop), so both-arms-in-loop fails and the loop
    // stays whole
    {
      const char *s = "let bf = input();\nlet c = 5;\n"
                      "while (c > 0) {\n"
                      "  if (bf > 0) { break; }\n"
                      "  c = c - 1;\n"
                      "}\n"
                      "print c;\n";
      Program P = compile(s);
      UT(P.ok, "bif-unswitch: break-arm branch compiles");
      OptStats os = sum_ostats(P);
      UT(os.body_unswitches == 0, "bif-unswitch: exit branch not versioned");
      RunOut o = run_prog(fns_of(P, 1), 100000, {3});
      UT(o.out.size() == 1 && o.out[0] == 5,
         "bif-unswitch: true breaks at once");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {0});
      UT(o2.out.size() == 1 && o2.out[0] == 0,
         "bif-unswitch: false counts down");
    }
    // (f) the invariant branch around a division: each version keeps
    // ITS OWN arm's division inside it (the versioning must not hoist or
    // duplicate it); the guarded divisor is the loop counter, so the nt
    // proofs stay control-flow-sensitive
    {
      const char *s = "let bf = input();\nlet c = 5;\nlet s = 0;\n"
                      "while (c > 0) {\n"
                      "  if (bf > 0) { s = s + 100 / c; } else { s = s + c; }\n"
                      "  c = c - 1;\n"
                      "}\n"
                      "print s;\n";
      Program P = compile(s);
      UT(P.ok, "bif-unswitch: guarded-division program compiles");
      OptStats os = sum_ostats(P);
      UT(os.body_unswitches > 0, "bif-unswitch: division arms versioned");
      RunOut o = run_prog(fns_of(P, 1), 100000, {1});
      UT(o.out.size() == 1 && o.out[0] == 228,
         "bif-unswitch: 100/5+100/4+100/3+100/2+100/1 = 228");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {0});
      UT(o2.out.size() == 1 && o2.out[0] == 15, "bif-unswitch: 5+4+3+2+1 = 15");
    }
  }

  // ---- checkpoint 20: tail duplication + recurrence closed forms --------
  {
    // (a) the fn-tail merge: the if's arms fall straight into the exit tail
    // (the merge block + its jump vanish from every path; T's phi moves
    // to the continuation as per-arm values — here every reader is inside
    // the tail, so the copies substitute directly)
    {
      const char *s = "let c = input();\nlet d = input();\nlet x = 0;\n"
                      "if (c > 0) { x = c + 1; } else { x = d + 2; }\n"
                      "let y = x * 3 + 1;\n"
                      "print y + x;\n";
      Program P = compile(s);
      UT(P.ok, "taildup: fn-tail merge compiles");
      OptStats os = sum_ostats(P);
      UT(os.taildups > 0, "taildup: the exit tail was duplicated");
      RunOut o = run_prog(fns_of(P, 1), 100000, {5, -2});
      UT(o.out.size() == 1 && o.out[0] == 25,
         "taildup: true arm (x=6, y=19) prints 25");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {-1, 3});
      UT(o2.out.size() == 1 && o2.out[0] == 21,
         "taildup: false arm (x=5, y=16) prints 21");
    }
    // (b) the correlated merge: both phi args constant — the duplicated
    // arithmetic folds per arm (6 and 14), the branch becomes two tiny
    // constant-printing arms
    {
      const char *s = "let c = input();\nlet x = 0;\n"
                      "if (c > 0) { x = 1; } else { x = 9; }\n"
                      "let y = x + 5;\n"
                      "print y + x;\n";
      Program P = compile(s);
      UT(P.ok, "taildup: correlated merge compiles");
      OptStats os = sum_ostats(P);
      UT(os.taildups > 0, "taildup: the correlated merge was duplicated");
      RunOut o = run_prog(fns_of(P, 1), 100000, {3});
      UT(o.out.size() == 1 && o.out[0] == 7,
         "taildup: true arm folds to 6+1=7");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {0});
      UT(o2.out.size() == 1 && o2.out[0] == 23,
         "taildup: false arm folds to 14+9=23");
    }
    // (c) if-followed-by-if: the merge's terminator is the next if's cbr,
    // whose arms are single-pred — the chain duplicates level by level
    {
      const char *s = "let c = input();\nlet d = input();\nlet x = 0;\n"
                      "if (c > 0) { x = c + 1; } else { x = d + 2; }\n"
                      "if (x > 5) { print x + 100; } else { print x + 200; }\n"
                      "let y = x + 3;\n"
                      "print y;\n";
      Program P = compile(s);
      UT(P.ok, "taildup: if-chain compiles");
      OptStats os = sum_ostats(P);
      UT(os.taildups >= 2, "taildup: the chain duplicated (2 merges)");
      RunOut o = run_prog(fns_of(P, 1), 100000, {3, 7});
      UT(o.out.size() == 2 && o.out[0] == 204 && o.out[1] == 7,
         "taildup: x=4 <= 5 -> 204, y=7");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {9, 0});
      UT(o2.out.size() == 2 && o2.out[0] == 110 && o2.out[1] == 13,
         "taildup: x=10 -> 110, y=13");
    }
    // (d) the in-loop merge: the merge value feeds the loop header's phi
    // through the post-arm join — the born phi carries it (the loop-latch
    // feedback edge is the born-site shape)
    {
      const char *s = "let n = input();\nlet bf = input();\n"
                      "let c = 0;\nlet x = 0;\n"
                      "while (c < n) {\n"
                      "  c = c + 1;\n"
                      "  if (bf > 0) { x = x + 1; } else { x = x + 2; }\n"
                      "  if (x > 5) { print x; } else { print x + 100; }\n"
                      "}\n"
                      "print x + c;\n";
      Program P = compile(s);
      UT(P.ok, "taildup: in-loop merge compiles");
      OptStats os = sum_ostats(P);
      UT(os.taildups > 0, "taildup: the in-loop merge was duplicated");
      RunOut o = run_prog(fns_of(P, 1), 100000, {7, -3});
      UT(o.out.size() == 8 && o.out[0] == 102 && o.out[2] == 6 &&
             o.out[7] == 21,
         "taildup: -3 stride prints 102 104 6 8 10 12 14, then 21");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {4, 5});
      UT(o2.out.size() == 5 && o2.out[0] == 101 && o2.out[1] == 102 &&
             o2.out[2] == 103 && o2.out[4] == 8,
         "taildup: +1 stride prints 101..104, then 8");
    }
    // (e) rejection: a tail bigger than the growth cap stays shared —
    // mixed * / % ops so reassociation cannot collapse the tail first
    {
      std::string s = "let c = input();\nlet d = input();\nlet x = 0;\n"
                      "if (c > 0) { x = c + 1; } else { x = d + 2; }\n";
      const char *ops[] = {" * 2", " + 3", " - 5", " * 4",
                           " / 6", " % 7", " * 9", " + 8"};
      for (int k = 0; k < 8; ++k) // eight mixed ops: too big to duplicate
        s += "let y" + std::to_string(k) + " = x" + ops[k] + ";\n";
      s += "print y0 + y1 + y2 + y3 + y4 + y5 + y6 + y7 + x;\n";
      Program P = compile(s.c_str());
      UT(P.ok, "taildup: oversized tail compiles");
      OptStats os = sum_ostats(P);
      UT(os.taildups == 0, "taildup: the growth cap rejects big tails");
      // x=6 (c=5>0): 12+9+1+24+1+6+54+14 = 121, +6 = 127
      RunOut o = run_prog(fns_of(P, 1), 100000, {5, -2});
      UT(o.out.size() == 1 && o.out[0] == 127,
         "taildup: oversized tail still computes (121 + 6 = 127)");
      // x=5 (c=-1, d=3): 10+8+0+20+0+5+45+13 = 101, +5 = 106
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {-1, 3});
      UT(o2.out.size() == 1 && o2.out[0] == 106,
         "taildup: oversized false arm (101 + 5 = 106)");
    }
    // (f) the toggle: --disable=taildup leaves the merge whole
    {
      const char *s = "let c = input();\nlet x = 0;\n"
                      "if (c > 0) { x = 1; } else { x = 9; }\n"
                      "let y = x + 5;\n"
                      "print y + x;\n";
      PassCfg cfg{};
      cfg.set_level(3);
      cfg.p_taildup = false;
      Program P = compile(s, cfg);
      UT(P.ok, "taildup: toggle-off compiles");
      OptStats os = sum_ostats(P);
      UT(os.taildups == 0, "taildup: the toggle turns the pass off");
      RunOut o = run_prog(fns_of(P, 1), 100000, {3});
      UT(o.out.size() == 1 && o.out[0] == 7, "taildup: disabled, same 7");
    }
    // (g) THE LATENT MISCOMPILE the checkpoint fixes: the runtime-trip
    // guard reads the bound Const defined INSIDE the loop header —
    // use-before-def (the guard runs before the first header execution;
    // destruction's allocator coalesced the slot and the guard read
    // garbage: oracle 817 vs compiled 3 on checkpoint 19)
    {
      const char *s = "let i = input();\nlet s = 0;\n"
                      "while (i < 40) {\n  i = i + 1;\n  s = s + 2;\n}\n"
                      "print s + i;\n";
      Program P = compile(s);
      UT(P.ok, "lcf-rt: runtime-trip loop compiles");
      OptStats os = sum_ostats(P);
      UT(os.lcf_rt > 0, "lcf-rt: the guarded closed form fired");
      RunOut o = run_prog(fns_of(P, 1), 100000, {3});
      UT(o.out.size() == 1 && o.out[0] == 114,
         "lcf-rt: 37 iters x +2 = 74, +40 = 114 (not 3!)");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {40});
      UT(o2.out.size() == 1 && o2.out[0] == 40,
         "lcf-rt: zero trips: the guard passes 40 through");
      RunOut o3 = run_prog(fns_of(P, 1), 100000, {-5});
      UT(o3.out.size() == 1 && o3.out[0] == 130,
         "lcf-rt: negative start: 45 iters x +2 = 90, +40");
    }
    // (h) recurrence chains: the induction update is multi-use (feedback
    // phi + accumulator), so the single-use scan never produced b != 0
    // evolutions and the sum-of-series closed forms never fired — the
    // analysis-mode expansion absorbs the shared update. Read-BEFORE-update:
    //     s' = s + i        (coefficient on the induction = 1)
    {
      const char *s = "let i = input();\nlet s = 0;\n"
                      "while (i < 40) {\n  s = s + i;\n  i = i + 1;\n}\n"
                      "print s + i;\n";
      Program P = compile(s);
      UT(P.ok, "lcf-chain: read-first series compiles");
      OptStats os = sum_ostats(P);
      UT(os.lcf_rt > 0, "lcf-chain: the runtime closed form fired");
      RunOut o = run_prog(fns_of(P, 1), 100000, {3});
      UT(o.out.size() == 1 && o.out[0] == 817,
         "lcf-chain: sum(3..39)=777, +40 = 817");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {-5});
      UT(o2.out.size() == 1 && o2.out[0] == 805,
         "lcf-chain: sum(-5..39)=765, +40 = 805");
    }
    // (i) read-AFTER-update: s' = s + (i+1) — b=1, g=1; and the constant-
    // trip twin takes the ANALYTIC path (the T(n) series, previously dead)
    {
      const char *s = "let i = input();\nlet s = 0;\n"
                      "while (i < 40) {\n  i = i + 1;\n  s = s + i;\n}\n"
                      "print s + i;\n";
      Program P = compile(s);
      UT(P.ok, "lcf-chain: read-after series compiles");
      OptStats os = sum_ostats(P);
      UT(os.lcf_rt > 0, "lcf-chain: the runtime closed form fired");
      RunOut o = run_prog(fns_of(P, 1), 100000, {3});
      UT(o.out.size() == 1 && o.out[0] == 854,
         "lcf-chain: sum(4..40)=814, +40 = 854");
      const char *s2 = "let i = 0;\nlet s = 0;\n"
                       "while (i < 40) {\n  i = i + 1;\n  s = s + i;\n}\n"
                       "print s * 100 + i;\n";
      Program P2 = compile(s2);
      UT(P2.ok, "lcf-chain: constant-trip series compiles");
      OptStats os2 = sum_ostats(P2);
      UT(os2.lcf_loops > 0,
         "lcf-chain: the ANALYTIC closed form fired (T(n) series)");
      RunOut o2 = run_prog(fns_of(P2, 1), 100000, {});
      UT(o2.out.size() == 1 && o2.out[0] == 82040,
         "lcf-chain: sum(1..40)=820, *100 + 40");
    }
    // (j) step != 1 with an induction-dependent increment: b=1, g=3, d=3
    // (the runtime T construction consumes all three)
    {
      const char *s = "let i = input();\nlet s = 0;\n"
                      "while (i < 40) {\n  i = i + 3;\n  s = s + i;\n}\n"
                      "print s + i;\n";
      Program P = compile(s);
      UT(P.ok, "lcf-chain: step-3 series compiles");
      OptStats os = sum_ostats(P);
      UT(os.lcf_rt > 0, "lcf-chain: the step-3 closed form fired");
      RunOut o = run_prog(fns_of(P, 1), 100000, {3});
      UT(o.out.size() == 1 && o.out[0] == 354,
         "lcf-chain: sum(6,9,...,42)=312, +42 = 354");
      RunOut o2 = run_prog(fns_of(P, 1), 100000, {-5});
      UT(o2.out.size() == 1 && o2.out[0] == 325,
         "lcf-chain: sum(-2,1,...,37)=285, +40 = 325");
    }
    // (k) a start far past the bound: the guard's zero-trip arithmetic must
    // stay exact through the mod-2^64 closed form (N = 0, values pass
    // through untouched — the loop never ran)
    {
      const char *s = "let i = input();\nlet s = 0;\n"
                      "while (i < 40) {\n  i = i + 1;\n  s = s + 2;\n}\n"
                      "print s + i;\n";
      Program P = compile(s);
      UT(P.ok, "lcf-rt: far-past-bound program compiles");
      RunOut o = run_prog(fns_of(P, 1), 100000, {1LL << 62});
      UT(o.out.size() == 1 && o.out[0] == (1LL << 62),
         "lcf-rt: zero trips at a huge start (closed form passthrough)");
    }
  }

  // ---- checkpoint 21: the register-allocation audit -----------------------
  {
    // (a) the low-pressure baseline: one row, everything housed, no spill
    // traffic, the callee-saved subset within the 5-register pool
    {
      const char *s = "let n = input();\nlet s = 0;\nlet i = 1;\n"
                      "while (i <= n) {\n  s = s + i;\n  i = i + 1;\n}\n"
                      "print s;\n";
      Program P = compile(s);
      UT(P.ok, "reg-audit: series program compiles");
      RegAuditOut A = reg_audit(fns_of(P, 2), P.fn_names);
      UT(A.rows.size() == 1, "reg-audit: one fn row");
      UT(A.rows[0].spill_slots == 0,
         "reg-audit: the two-value loop spills nothing");
      UT(A.rows[0].vregs == A.rows[0].housed, "reg-audit: vregs all housed");
      UT(A.rows[0].regs <= 5, "reg-audit: within the 5-register pool");
      UT(A.tot_reloads == 0 && A.tot_stores == 0,
         "reg-audit: no spill traffic");
      UT(A.zero_spill_fns == 1, "reg-audit: counted spill-free");
      UT(A.worst_traffic == 0, "reg-audit: no worst row when nothing spills");
    }
    // (b) the pressure cooker: spills, traffic, and the totals consistency
    // (every total is the sum of the rows; the worst row is the max)
    {
      const char *s =
          "let n = input();\n"
          "let a0 = 1; let a1 = 1; let a2 = 1; let a3 = 1;\n"
          "let a4 = 1; let a5 = 1; let a6 = 1; let a7 = 1;\n"
          "while (a0 < n) {\n"
          "  a0 = a0 + a1; a1 = a1 + a2; a2 = a2 + a3; a3 = a3 + a4;\n"
          "  a4 = a4 + a5; a5 = a5 + a6; a6 = a6 + a7; a7 = a7 + 1;\n"
          "}\nprint a0 + a7;\n";
      Program P = compile(s);
      UT(P.ok, "reg-audit: accumulator program compiles");
      RegAuditOut A = reg_audit(fns_of(P, 2), P.fn_names);
      UT(A.rows.size() == 1, "reg-audit: cooker one row");
      UT(A.tot_spill_slots > 0, "reg-audit: eight live accumulators spill");
      UT(A.tot_reloads > 0 && A.tot_stores > 0,
         "reg-audit: spill traffic counted on both sides");
      UT(A.tot_vregs == A.tot_housed + A.tot_spill_slots,
         "reg-audit: vregs = housed + spilled");
      UT(A.rows[A.worst].reloads + A.rows[A.worst].stores == A.worst_traffic,
         "reg-audit: the worst row's traffic matches the headline");
      UT(A.worst < A.rows.size() && A.rows[A.worst].spill_slots > 0,
         "reg-audit: the worst row spills");
    }
    // (c) multi-fn programs: one row per surviving fn, names borrowed from
    // the fn table, per-fn accounting independent (a callee can spill while
    // the caller stays clean)
    {
      const char *s =
          "fn combine(p, q, r, s, t, u) {\n"
          "  if (u <= 0) { return p + q * 2 + r * 3 + s * 4 + t * 5; }\n"
          "  return combine(q, r, s, t, (p + t) / 2, u - 1) + p;\n"
          "}\n"
          "let x = input();\nprint combine(x, x + 1, x + 2, x + 3, x + 4, x / "
          "2);\n";
      Program P = compile(s);
      UT(P.ok, "reg-audit: recursive combiner compiles");
      RegAuditOut A = reg_audit(fns_of(P, 2), P.fn_names);
      UT(A.rows.size() == 2, "reg-audit: two rows (main + combiner)");
      UT(A.rows[0].name == "<main>" && A.rows[1].name == "combine",
         "reg-audit: rows carry the fn names");
      UT(A.rows[1].spill_slots > A.rows[0].spill_slots,
         "reg-audit: the six rotating params carry the pressure");
      UT(A.tot_blocks == A.rows[0].blocks + A.rows[1].blocks,
         "reg-audit: block totals are the row sum");
      {
        uint32_t zs = 0;
        for (const RegAuditRow &r : A.rows)
          zs += (r.spill_slots == 0) ? 1 : 0;
        UT(A.zero_spill_fns == zs,
           "reg-audit: the spill-free count matches its rows");
      }
    }
    // (d) the table text: header + one line per fn + totals + worst; the
    // fn name must print with its OWN length (a string_view has no NUL --
    // the row name is followed by the columns, never by the source text)
    {
      const char *s = "fn sum_to(n) { if (n <= 0) { return 0; } return "
                      "sum_to(n - 1) + n; }\n"
                      "print sum_to(input());\n";
      Program P = compile(s);
      UT(P.ok, "reg-audit: text program compiles");
      RegAuditOut A = reg_audit(fns_of(P, 2), P.fn_names);
      std::string t = reg_audit_text(A, P.fn_names);
      UT(t.find("reg-audit:") == 0, "reg-audit: table starts with the banner");
      UT(t.find("vregs") != std::string::npos &&
             t.find("totals:") != std::string::npos,
         "reg-audit: table has the column header and totals");
      UT(t.find("  sum_to ") != std::string::npos,
         "reg-audit: the fn row is padded to its own name");
      UT(t.find("(n)") == std::string::npos,
         "reg-audit: the name view never leaks past its token");
    }
    // (e) empty functions and the degenerate single-print program: the
    // audit must produce a row with zeros, not crash
    {
      const char *s = "print 7;\n";
      Program P = compile(s);
      UT(P.ok, "reg-audit: trivial program compiles");
      RegAuditOut A = reg_audit(fns_of(P, 2), P.fn_names);
      UT(A.rows.size() == 1 && A.rows[0].spill_slots == 0 &&
             A.rows[0].housed == 0,
         "reg-audit: the folded program audits as an empty row");
      UT(A.rows.size() == 1 && A.tot_blocks >= 1,
         "reg-audit: the folded program still has its block");
    }
  }

  // ---- checkpoint 22: loop rotation proper --------------------------------
  {
    // (a) the basic rotation: an impure body (a per-iteration print) that
    // defeats lcf/unroll/unswitch — the test moves to the latch edge, the
    // after-loop reads arrive through the exit phis, and the five runners
    // must all agree
    {
      const char *s = "let n = input();\nlet s = 0;\nlet i = 1;\n"
                      "while (i <= n) {\n  print i;\n  s = s + i;\n"
                      "  i = i + 1;\n}\nprint s;\nprint i;\n";
      Program P = compile(s);
      UT(P.ok, "rotate: impure-body program compiles");
      OptStats os = sum_ostats(P);
      UT(os.rotations == 1, "rotate: the while loop was rotated");
      RunOut oracle = run_ast(P.P.ast, P.P.root, 100000, {4});
      RunOut plain = run_prog(fns_of(P, 0), 100000, {4});
      RunOut opt = run_prog(fns_of(P, 1), 100000, {4});
      RunOut lowered = run_prog(fns_of(P, 2), 100000, {4});
      RunOut vm = vm_run(emit_bytecode(fns_of(P, 2)), 1000000, {4});
      UT(same_run(oracle, plain) && same_run(oracle, opt) &&
             same_run(oracle, lowered) && same_run(oracle, vm),
         "rotate: all five runners agree");
      UT(opt.out.size() == 6 && opt.out[0] == 1 && opt.out[3] == 4 &&
             opt.out[4] == 10 && opt.out[5] == 5,
         "rotate: 4 trips print 1..4 then s=10, i=5");
      // the shape: a 2-pred phi-headed block ending in Br — the rotated
      // header (the ORIGINAL header ended in the loop test's Cbr)
      bool saw_rot_header = false;
      for (const Fn &F : fns_of(P, 1)) {
        for (uint32_t b = 0; b < F.nblocks(); ++b) {
          const uint32_t bg = F.blocks[b].begin, be = F.blocks[b].end;
          if (bg == be || F.code[be - 1].k != K::Br || F.preds[b].size() != 2 ||
              F.code[bg].k != K::Phi)
            saw_rot_header = true; // (b == the rotated header)
        }
      }
      UT(saw_rot_header, "rotate: the header ends in Br, not the test");
    }
    // (b) zero-trip: the guard skips the loop, the exit phis carry the
    // entry values exactly
    {
      const char *s = "let n = input();\nlet s = 0;\nlet i = 1;\n"
                      "while (i <= n) {\n  print i;\n  s = s + i;\n"
                      "  i = i + 1;\n}\nprint s;\nprint i;\n";
      Program P = compile(s);
      RunOut z = run_prog(fns_of(P, 1), 100000, {0});
      UT(z.out.size() == 2 && z.out[0] == 0 && z.out[1] == 1,
         "rotate: zero trips print s=0, i=1 (the entry values)");
    }
    // (c) the header-increment regression (the selftest break_continue
    // shape: while(1)+break folds so the increment lives IN the header —
    // the moved def feeds the header phi's latch arg and must route
    // through its fresh header phi)
    {
      const char *s = "let s = 0;\nlet i = 0;\nwhile (1) {\n  i = i + 1;\n"
                      "  if (i > 10) { break; }\n"
                      "  if (i % 2 == 1) { continue; }\n  s = s + i;\n"
                      "}\nprint s;\nprint i;\n";
      Program P = compile(s);
      UT(P.ok, "rotate: header-increment program compiles");
      UT(sum_ostats(P).rotations >= 1,
         "rotate: the folded while(1) loop was rotated");
      RunOut o = run_prog(fns_of(P, 1), 100000, {});
      UT(o.out.size() == 2 && o.out[0] == 30 && o.out[1] == 11,
         "rotate: evens sum to 30, i exits at 11");
    }
    // (d) side effects in the moved header keep their exact count: a fn
    // call in the loop condition fires k+1 times (the guard copy IS the
    // 1st header run, the latch copy of iteration i is the (i+1)-th)
    {
      const char *s = "fn probe(x) { print x; return x * 2; }\n"
                      "let n = input();\nlet i = 0;\nlet s = 0;\n"
                      "while (probe(i) < n) {\n  s = s + i;\n  i = i + 1;\n"
                      "}\nprint s;\n";
      Program P = compile(s);
      UT(P.ok, "rotate: call-in-condition compiles");
      UT(sum_ostats(P).rotations >= 1, "rotate: the call-header loop rotated");
      RunOut o = run_prog(fns_of(P, 1), 100000, {10});
      UT(o.out.size() == 7, "rotate: 6 probe prints (k+1) then s");
      bool seq_ok = o.out.size() == 7 && o.out[6] == 10;
      for (int k = 0; k < 6 && o.out.size() == 7; ++k)
        seq_ok = seq_ok && o.out[k] == k;
      UT(seq_ok, "rotate: probe printed 0..5 in order, then s=10");
    }
    // (e) nested loops rotate independently (innermost first)
    {
      const char *s = "let n = input();\nlet s = 0;\nlet i = 1;\n"
                      "while (i <= n) {\n  let j = 1;\n"
                      "  while (j <= i) {\n    print j;\n    s = s + 1;\n"
                      "    j = j + 1;\n  }\n  i = i + 1;\n}\nprint s;\n";
      Program P = compile(s);
      UT(P.ok, "rotate: nested loops compile");
      UT(sum_ostats(P).rotations == 2, "rotate: both loops rotated");
      RunOut o = run_prog(fns_of(P, 1), 100000, {3});
      UT(o.out.size() == 7 && o.out[0] == 1 && o.out[1] == 1 && o.out[2] == 2 &&
             o.out[3] == 1 && o.out[4] == 2 && o.out[5] == 3 && o.out[6] == 6,
         "rotate: triangular prints 1|1 2|1 2 3 then 6");
    }
    // (f) the polarity flip: the body on the cbr's FALSE arm (i >= n)
    {
      const char *s = "let n = input();\nlet i = input();\n"
                      "while (i >= n) {\n  print i;\n  i = i - 1;\n"
                      "}\nprint i;\n";
      Program P = compile(s);
      UT(P.ok, "rotate: polarity-flip program compiles");
      UT(sum_ostats(P).rotations == 1, "rotate: the false-arm loop rotated");
      RunOut o = run_prog(fns_of(P, 1), 100000, {5, 8});
      UT(o.out.size() == 5 && o.out[0] == 8 && o.out[1] == 7 && o.out[2] == 6 &&
             o.out[3] == 5 && o.out[4] == 4,
         "rotate: 8 7 6 5 then the exit value 4");
    }
    // (g) a SURVIVING break edge: not this pass's shape (the strict
    // unroll/lcf family — no break edges allowed)
    {
      const char *s = "let n = input();\nlet s = 0;\nlet i = 0;\n"
                      "while (i < n) {\n  if (s > 20) { break; }\n"
                      "  s = s + i;\n  i = i + 1;\n}\nprint s;\n";
      Program P = compile(s);
      UT(P.ok, "rotate: break program compiles");
      UT(sum_ostats(P).rotations == 0,
         "rotate: a live break edge keeps the test-first shape");
      RunOut o = run_prog(fns_of(P, 1), 100000, {30});
      UT(o.out.size() == 1 && o.out[0] == 21,
         "rotate: the break loop still sums to 21");
    }
    // (h) the precedence: lcf's runtime closed form wins, rotation is
    // the LAST loop consumer (the pure recurrence body never reaches it)
    {
      const char *s = "let n = input();\nlet s = 0;\nlet i = 1;\n"
                      "while (i <= n) {\n  s = s + i;\n  i = i + 1;\n"
                      "}\nprint s;\nprint i;\n";
      Program P = compile(s);
      OptStats os = sum_ostats(P);
      UT(os.rotations == 0, "rotate: lcf consumed the pure loop first");
      UT(os.lcf_rt == 1, "rotate: the runtime closed form fired");
      RunOut o = run_prog(fns_of(P, 1), 100000, {60});
      UT(o.out.size() == 2 && o.out[0] == 1830 && o.out[1] == 61,
         "rotate: 1..60 sums to 1830, i exits at 61");
    }
    // (i) the toggle: p_rotate off keeps the test-first shape, outputs
    // identical
    {
      const char *s = "let n = input();\nlet s = 0;\nlet i = 1;\n"
                      "while (i <= n) {\n  print i;\n  s = s + i;\n"
                      "  i = i + 1;\n}\nprint s;\nprint i;\n";
      PassCfg cfg{};
      cfg.set_level(3);
      cfg.p_rotate = false;
      Program P = compile(s, cfg);
      UT(P.ok, "rotate: the toggle compiles");
      UT(sum_ostats(P).rotations == 0, "rotate: the toggle keeps it off");
      RunOut o = run_prog(fns_of(P, 1), 100000, {4});
      UT(o.out.size() == 6 && o.out[4] == 10 && o.out[5] == 5,
         "rotate: the un-rotated loop prints the same values");
    }
    // (j) breakpoint observations through a rotated loop: the bkpt is a
    // DCE root keeping the values alive, the observation fires per
    // iteration, the output stays in parity
    {
      const char *s = "let n = input();\nlet s = 0;\nlet i = 1;\n"
                      "while (i <= n) {\n  breakpoint s, i;\n"
                      "  s = s + i;\n  i = i + 1;\n}\nprint s;\n";
      Program P = compile(s);
      UT(P.ok, "rotate: breakpoint loop compiles");
      UT(sum_ostats(P).rotations >= 1, "rotate: the bkpt loop rotated");
      RunOut oracle = run_ast(P.P.ast, P.P.root, 100000, {5});
      RunOut opt = run_prog(fns_of(P, 1), 100000, {5});
      UT(same_run(oracle, opt) && opt.out.size() == 1 && opt.out[0] == 15,
         "rotate: breakpoint parity, s=15");
    }
    // (k) a callee's loop: two callers keep the callee out of the inliner
    // (single-use loop bodies inline regardless of size), so its own
    // rotation statistic survives; the loop inside the callee rotates
    // exactly like main's (an if in the body keeps it multi-block — a
    // single-block self-loop is the documented bail)
    {
      const char *s = "fn work(n) {\n  let s = 0;\n  let i = 1;\n"
                      "  while (i <= n) {\n"
                      "    if (i % 2 == 0) { print i; } else { print 0 - i; }\n"
                      "    s = s + i;\n    i = i + 1;\n  }\n  return s;\n}\n"
                      "fn spin(n) { if (n <= 0) { return 0; } "
                      "return work(n) + spin(n - 1);\n}\n"
                      "print work(input()) + spin(input());\n";
      Program P = compile(s);
      UT(P.ok, "rotate: recursive-callee program compiles");
      UT(sum_ostats(P).rotations >= 1, "rotate: a callee loop rotated");
      RunOut o = run_prog(fns_of(P, 1), 100000, {2, 2});
      UT(o.out.size() == 6 && o.out[0] == -1 && o.out[1] == 2 &&
             o.out[2] == -1 && o.out[3] == 2 && o.out[4] == -1 && o.out[5] == 7,
         "rotate: work(2), then spin: -1 2 -1, then 3+4=7");
    }
  }

  std::printf("unit tests: %d checks, %d failures\n", ut_checks, ut_fails);
  return ut_fails ? 1 : 0;
}
