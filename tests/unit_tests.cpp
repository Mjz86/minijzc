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
  {// (a) congruent merge phis unify: two vars tracking the same values
   // through the same if/else are ONE value after -O3, and the sum of
   // the pair doubles one phi instead of adding two distinct ones
   {const char *s = "let a = 0;\nlet b = 0;\n"
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
  UT(o.out.size() == 1 && o.out[0] == 10, "gvn: disabled pass, same result");
}
}

// ---- unswitch + lsimp: loop versioning and canonicalization -------------
{// (a) trivial unswitch: an invariant header condition versions the
 // loop (the false path skips it, the true clone drops the test);
 // values live past the loop keep their per-path meanings
 {const char *s = "let f = input();\nlet c = 3;\n"
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
UT(o2.out.size() == 3 && o2.out[0] == 2 && o2.out[1] == 1 && o2.out[2] == 1000,
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
{// (a) `while (a && b)` with both flags invariant: the whole guard
 // chain is versioned (preheader cbr a -> g2: cbr b -> clone / fx ->
 // exit); all three polarity paths keep the original semantics and
 // the value live past the loop is the ENTRY value on zero-iteration
 // paths
 {const char *s = "let f = input();\nlet g = input();\nlet c = 5;\n"
                  "while (f > 0 && g > 0) {\n  if (c <= 0) { break; }\n"
                  "  c = c - 1;\n  print c;\n}\n"
                  "print 1000 + c;\n";
Program P = compile(s);
UT(P.ok, "cc-unswitch: && program compiles");
OptStats os = sum_ostats(P);
UT(os.unswitches > 0, "cc-unswitch: the && guard chain versioned");
RunOut o = run_prog(fns_of(P, 1), 100000, {3, 2});
UT(o.out.size() == 6 && o.out[0] == 4 && o.out[4] == 0 && o.out[5] == 1000,
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
{// (a) the canonical bait: the head test is a real while-condition and
 // the body hides an invariant if. BOTH versions keep running the
 // loop, and the exit value of cn DIFFERS between them (the +1 side
 // lands exactly on the bound, the +2 side can overshoot by one) —
 // the version-merge blocks carry it, so this is load-bearing
 {const char *s = "let bf = input();\nlet cn = 0;\n"
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
  const char *s = "let cn = 0;\n"
                  "while (cn < 5) {\n"
                  "  if (cn % 2 == 0) { cn = cn + 1; } else { cn = cn + 2; }\n"
                  "}\n"
                  "print cn;\n";
  Program P = compile(s);
  UT(P.ok, "bif-unswitch: non-invariant branch compiles");
  OptStats os = sum_ostats(P);
  UT(os.unswitches == 0, "bif-unswitch: varying cond left alone");
  RunOut o = run_prog(fns_of(P, 1), 100000, {});
  UT(o.out.size() == 1 && o.out[0] == 5, "bif-unswitch: 0,1,3,5 lands on 5");
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
  UT(o.out.size() == 1 && o.out[0] == 5, "bif-unswitch: true breaks at once");
  RunOut o2 = run_prog(fns_of(P, 1), 100000, {0});
  UT(o2.out.size() == 1 && o2.out[0] == 0, "bif-unswitch: false counts down");
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
{// (a) the fn-tail merge: the if's arms fall straight into the exit tail
 // (the merge block + its jump vanish from every path; T's phi moves
 // to the continuation as per-arm values — here every reader is inside
 // the tail, so the copies substitute directly)
 {const char *s = "let c = input();\nlet d = input();\nlet x = 0;\n"
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
  UT(o.out.size() == 1 && o.out[0] == 7, "taildup: true arm folds to 6+1=7");
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
  UT(o.out.size() == 8 && o.out[0] == 102 && o.out[2] == 6 && o.out[7] == 21,
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
{// (a) the low-pressure baseline: one row, everything housed, no spill
 // traffic, the callee-saved subset within the 5-register pool
 {const char *s = "let n = input();\nlet s = 0;\nlet i = 1;\n"
                  "while (i <= n) {\n  s = s + i;\n  i = i + 1;\n}\n"
                  "print s;\n";
Program P = compile(s);
UT(P.ok, "reg-audit: series program compiles");
RegAuditOut A = reg_audit(fns_of(P, 2), P.fn_names);
UT(A.rows.size() == 1, "reg-audit: one fn row");
UT(A.rows[0].spill_slots == 0, "reg-audit: the two-value loop spills nothing");
UT(A.rows[0].vregs == A.rows[0].housed, "reg-audit: vregs all housed");
UT(A.rows[0].regs <= 5, "reg-audit: within the 5-register pool");
UT(A.tot_reloads == 0 && A.tot_stores == 0, "reg-audit: no spill traffic");
UT(A.zero_spill_fns == 1, "reg-audit: counted spill-free");
UT(A.worst_traffic == 0, "reg-audit: no worst row when nothing spills");
}
// (b) the pressure cooker: spills, traffic, and the totals consistency
// (every total is the sum of the rows; the worst row is the max)
{
  const char *s = "let n = input();\n"
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
  UT(A.rows.size() == 1 && A.rows[0].spill_slots == 0 && A.rows[0].housed == 0,
     "reg-audit: the folded program audits as an empty row");
  UT(A.rows.size() == 1 && A.tot_blocks >= 1,
     "reg-audit: the folded program still has its block");
}
}

// ---- checkpoint 22: loop rotation proper --------------------------------
{// (a) the basic rotation: an impure body (a per-iteration print) that
 // defeats lcf/unroll/unswitch — the test moves to the latch edge, the
 // after-loop reads arrive through the exit phis, and the five runners
 // must all agree
 {const char *s = "let n = input();\nlet s = 0;\nlet i = 1;\n"
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
  UT(o.out.size() == 6 && o.out[0] == -1 && o.out[1] == 2 && o.out[2] == -1 &&
         o.out[3] == 2 && o.out[4] == -1 && o.out[5] == 7,
     "rotate: work(2), then spin: -1 2 -1, then 3+4=7");
}
}

// ---- 16. the generic SSA kernel (mjz::ssa_ns, ir/ssa_generic.hpp) -------
// Pure index-space contracts, no Fn/Instr involved: placement, the
// N-ary phi records (including a 3-pred join this IR cannot even
// spell -- the multi-branch answer), renaming as tables, the reflow
// plan, the merge-chain lowering.
{
  using ssa_ns::ssa_def_v;
  using ssa_ns::ssa_event_t;
  using ssa_ns::ssa_none_v;
  using ssa_ns::ssa_use_v;
  // dominator bundle from succ adjacency (what An::analyze does)
  auto an_of = [](const std::vector<std::vector<ulen>> &adjs) {
    struct A_ {
      gns::treversal_result_t<V> fs, fp, dom_tree, df;
      std::vector<ulen> idom;
    } a;
    a.fs = gns::make_basic_forest<V>(adjs);
    a.fp = gns::make_basic_inv_forest<V>(adjs);
    a.idom = gns::calculate_dominators_from_entry<V>(0, a.fs, a.fp);
    a.dom_tree = gns::make_dominator_forest_given_dominators<V>(a.idom);
    a.df = gns::calculate_dominance_frontiers_given_dominators<V>(a.idom, a.fp);
    return a;
  };
  // ascending positional pred rows (the fn.preds order the adapters use)
  auto preds_of = [](const std::vector<std::vector<ulen>> &adjs) {
    std::vector<std::vector<ulen>> prs(adjs.size());
    for (ulen b = 0; b < adjs.size(); ++b)
      for (ulen s : adjs[b])
        prs[s].push_back(b);
    return prs;
  };
  auto row_is = [](const auto &forest, ulen b, std::vector<ulen> want) -> bool {
    auto r = forest.range()[b];
    if (std::ranges::size(r) != want.size())
      return false;
    for (ulen i = 0; i < want.size(); ++i)
      if (r[i] != want[i])
        return false;
    return true;
  };

  // 16a. placement: diamond -> phi at the join; loop -> phi at the
  // header; single-def vars are skipped (the classic construction)
  {
    const std::vector<std::vector<ulen>> diamond = {{1, 2}, {3}, {3}, {}};
    auto A = an_of(diamond);
    auto prs = preds_of(diamond);
    auto pf = gns::make_basic_forest<V>(prs);
    std::vector<std::vector<ulen>> defs = {{1, 2}, {0}};
    auto pl = ssa_ns::calculate_phi_placements<V>(A.df, pf, defs);
    UT(row_is(pl, 3, {0}), "placement: diamond's join gets var 0");
    UT(row_is(pl, 0, {}) && row_is(pl, 1, {}) && row_is(pl, 2, {}),
       "placement: no phis outside the join");
    const std::vector<std::vector<ulen>> loop = {{1}, {2, 3}, {1}, {}};
    auto A2 = an_of(loop);
    auto prs2 = preds_of(loop);
    auto pf2 = gns::make_basic_forest<V>(prs2);
    std::vector<std::vector<ulen>> defs2 = {{0, 2}, {2}};
    auto pl2 = ssa_ns::calculate_phi_placements<V>(A2.df, pf2, defs2);
    UT(row_is(pl2, 1, {0}), "placement: loop header gets var 0");
    UT(row_is(pl2, 0, {}) && row_is(pl2, 2, {}) && row_is(pl2, 3, {}),
       "placement: single-def var 1 places nothing");
  }
  // 16b. renaming on the diamond: reaching defs, the classic
  // stack-depth-at-push ordinals, both phi entries seeded per edge
  {
    const std::vector<std::vector<ulen>> diamond = {{1, 2}, {3}, {3}, {}};
    auto A = an_of(diamond);
    auto prs = preds_of(diamond);
    auto pf = gns::make_basic_forest<V>(prs);
    auto sf = gns::make_basic_forest<V>(diamond);
    std::vector<std::vector<ulen>> defs = {{1, 2}};
    auto pl = ssa_ns::calculate_phi_placements<V>(A.df, pf, defs);
    std::vector<std::vector<ssa_event_t>> evs = {{{ssa_def_v, 0, 0}},
                                                 {{ssa_def_v, 0, 1}},
                                                 {{ssa_def_v, 0, 2}},
                                                 {{ssa_use_v, 0, 3}}};
    auto R = ssa_ns::rename_ssa<V>(1, 4, A.dom_tree, sf, pf, pl, evs);
    UT(R.visit_order.size() == 4 && R.visit_order[0] == 0,
       "rename: dom-tree pre-order from block 0, all blocks");
    UT(R.reaching_def[3] == 4, "rename: the join's use reads the phi");
    UT(R.def_ordinal[0] == 0, "rename: entry def is version 0");
    UT(R.reaching_def[1] == ssa_none_v && R.reaching_def[2] == ssa_none_v,
       "rename: def sites get no reaching-def row (defs are not uses)");
    UT(R.phi_block.size() == 1 && R.phi_block[0] == 3 && R.phi_var[0] == 0,
       "rename: one phi record, at the join, for var 0");
    UT(R.phi_def_site[0] == 4, "rename: phi site ids start at site_count");
    UT(R.phi_entry_begin[1] - R.phi_entry_begin[0] == 2,
       "rename: the join's phi has 2 entries (arity = pred count)");
    UT(R.phi_entry_pred[R.phi_entry_begin[0] + 0] == 1 &&
           R.phi_entry_def[R.phi_entry_begin[0] + 0] == 1,
       "rename: entry 0 = the edge from arm 1, carrying arm 1's def");
    UT(R.phi_entry_pred[R.phi_entry_begin[0] + 1] == 2 &&
           R.phi_entry_def[R.phi_entry_begin[0] + 1] == 2,
       "rename: entry 1 = the edge from arm 2, carrying arm 2's def");
  }
  // 16c. minimal SSA: an edge with no reaching def keeps arg NONE
  // (the loop header's entry edge, var not live-in from entry)
  {
    const std::vector<std::vector<ulen>> loop = {{1}, {2, 3}, {1}, {}};
    auto A = an_of(loop);
    auto prs = preds_of(loop);
    auto pf = gns::make_basic_forest<V>(prs);
    auto sf = gns::make_basic_forest<V>(loop);
    std::vector<std::vector<ulen>> defs = {{2, 2}}; // 2 defs in the body
    auto pl = ssa_ns::calculate_phi_placements<V>(A.df, pf, defs);
    std::vector<std::vector<ssa_event_t>> evs = {
        {}, {{ssa_use_v, 0, 0}}, {{ssa_def_v, 0, 1}, {ssa_def_v, 0, 2}}, {}};
    auto R = ssa_ns::rename_ssa<V>(1, 3, A.dom_tree, sf, pf, pl, evs);
    UT(R.reaching_def[0] == 3, "rename: header use reads the phi");
    UT(R.phi_entry_pred[R.phi_entry_begin[0] + 0] == 0 &&
           R.phi_entry_def[R.phi_entry_begin[0] + 0] == ssa_none_v,
       "rename: undef arg on the entry edge (minimal SSA)");
    UT(R.phi_entry_pred[R.phi_entry_begin[0] + 1] == 2 &&
           R.phi_entry_def[R.phi_entry_begin[0] + 1] == 2,
       "rename: the back edge carries the body's LAST def");
  }
  // 16d. THE MULTI-BRANCH CASE: a 3-pred join gets a 3-entry phi
  // record -- arbitrary arity is native to the kernel (the IR's own
  // Phi instr cannot spell this; pred_norm lowers such joins into
  // binary chains BEFORE SSA, which is exactly why the kernel also
  // exports the chain planner)
  {
    const std::vector<std::vector<ulen>> fork3 = {{1, 2, 3}, {4}, {4}, {4}, {}};
    auto A = an_of(fork3);
    auto prs = preds_of(fork3);
    auto pf = gns::make_basic_forest<V>(prs);
    auto sf = gns::make_basic_forest<V>(fork3);
    std::vector<std::vector<ulen>> defs = {{1, 2}};
    auto pl = ssa_ns::calculate_phi_placements<V>(A.df, pf, defs);
    UT(row_is(pl, 4, {0}), "n-ary: the 3-pred join places the phi");
    std::vector<std::vector<ssa_event_t>> evs = {
        {}, {{ssa_def_v, 0, 0}}, {{ssa_def_v, 0, 1}}, {}, {{ssa_use_v, 0, 2}}};
    auto R = ssa_ns::rename_ssa<V>(1, 3, A.dom_tree, sf, pf, pl, evs);
    UT(R.phi_entry_begin[1] - R.phi_entry_begin[0] == 3,
       "n-ary: the phi record has THREE entries");
    UT(R.phi_entry_pred[R.phi_entry_begin[0] + 0] == 1 &&
           R.phi_entry_def[R.phi_entry_begin[0] + 0] == 0,
       "n-ary: entry 0 = edge from block 1");
    UT(R.phi_entry_pred[R.phi_entry_begin[0] + 1] == 2 &&
           R.phi_entry_def[R.phi_entry_begin[0] + 1] == 1,
       "n-ary: entry 1 = edge from block 2");
    UT(R.phi_entry_pred[R.phi_entry_begin[0] + 2] == 3 &&
           R.phi_entry_def[R.phi_entry_begin[0] + 2] == ssa_none_v,
       "n-ary: entry 2 = edge from block 3, undef (no def on it)");
    UT(R.reaching_def[2] == 3, "n-ary: the join's use reads the phi");
  }
  // 16e. duplicate pred rows (cbr X X) mixed into an N-ary join: the
  // row [0,0,2] has arity 3 with block 0 twice; only the LAST slot of
  // the repeated pred is seeded -- the adapters' lowering rule pins
  // to it. (A pure [0,0] block, by contrast, is its pred's dominated
  // child: empty frontier, no phi at all -- the dup is one real edge.)
  {
    const std::vector<std::vector<ulen>> dup = {{1, 1, 2}, {}, {1}};
    auto A = an_of(dup);
    auto prs = preds_of(dup); // preds[1] = [0, 0, 2]
    auto pf = gns::make_basic_forest<V>(prs);
    auto sf = gns::make_basic_forest<V>(dup);
    std::vector<std::vector<ulen>> defs = {{0, 2}};
    auto pl = ssa_ns::calculate_phi_placements<V>(A.df, pf, defs);
    UT(row_is(pl, 1, {0}), "dup-pred: the [0,0,2] join places the phi");
    std::vector<std::vector<ssa_event_t>> evs = {
        {{ssa_def_v, 0, 0}}, {{ssa_use_v, 0, 2}}, {{ssa_def_v, 0, 1}}};
    auto R = ssa_ns::rename_ssa<V>(1, 3, A.dom_tree, sf, pf, pl, evs);
    UT(R.phi_entry_begin[1] - R.phi_entry_begin[0] == 3,
       "dup-pred: the phi record has 3 entries (dups count)");
    UT(R.phi_entry_pred[R.phi_entry_begin[0] + 0] == ssa_none_v,
       "dup-pred: the FIRST slot of the repeated pred stays unseeded");
    UT(R.phi_entry_pred[R.phi_entry_begin[0] + 1] == 0 &&
           R.phi_entry_def[R.phi_entry_begin[0] + 1] == 0,
       "dup-pred: the LAST slot of the repeated pred carries its value");
    UT(R.phi_entry_pred[R.phi_entry_begin[0] + 2] == 2 &&
           R.phi_entry_def[R.phi_entry_begin[0] + 2] == 1,
       "dup-pred: the third edge is block 2's, carrying its def");
    UT(R.reaching_def[2] == 3, "dup-pred: the join's use reads the phi");
    // and the degenerate case: cbr(c, X, X) alone places NOTHING at X
    // (X is its pred's dominated child -- one real edge, empty DF)
    const std::vector<std::vector<ulen>> dup1 = {{1, 1}, {}};
    auto A1 = an_of(dup1);
    auto prs1 = preds_of(dup1);
    auto pf1 = gns::make_basic_forest<V>(prs1);
    std::vector<std::vector<ulen>> defs1 = {{0, 0}};
    auto pl1 = ssa_ns::calculate_phi_placements<V>(A1.df, pf1, defs1);
    UT(row_is(pl1, 1, {}),
       "dup-pred: a lone cbr X X needs no phi (empty frontier)");
  }
  // 16f. reflow: drop + head inserts + anchored afters, one plan
  {
    std::vector<ulen> ob = {0, 3}, oe = {3, 5};
    std::vector<ulen> keep = {1, 1, 0, 1, 1}, head = {2, 0},
                      after = {0, 0, 0, 5, 0};
    auto plan = ssa_ns::calculate_reflow_plan<V>(ob, oe, keep, head, after);
    UT(plan.total == 11, "reflow: total = heads + kept + afters");
    UT(plan.new_begin[0] == 0 && plan.new_end[0] == 4,
       "reflow: block 0 = 2 heads + 2 kept");
    UT(plan.new_begin[1] == 4 && plan.new_end[1] == 11,
       "reflow: block 1 = kept + 5 anchored afters + kept");
    UT(plan.old_to_new[0] == 2 && plan.old_to_new[1] == 3,
       "reflow: survivors shift past the heads");
    UT(plan.old_to_new[2] == ssa_none_v,
       "reflow: the dropped instr maps to NONE");
    UT(plan.old_to_new[3] == 4 && plan.old_to_new[4] == 10,
       "reflow: afters shift the following survivors");
  }
  // 16g. the merge-chain plan: N preds -> a right-leaning binary chain
  {
    auto plan = ssa_ns::calculate_merge_chain_plan<V>(
        7, std::vector<ulen>{7, 9, 11, 13, 15});
    UT(plan.block == 7 && plan.chain_count == 3,
       "chain: 5 preds -> 3 fresh chain blocks");
    UT(plan.redirect_pred.size() == 4 && plan.redirect_pred[0] == 9 &&
           plan.redirect_chain[0] == 0 && plan.redirect_pred[1] == 11 &&
           plan.redirect_chain[1] == 0 && plan.redirect_pred[2] == 13 &&
           plan.redirect_chain[2] == 1 && plan.redirect_pred[3] == 15 &&
           plan.redirect_chain[3] == 2,
       "chain: p1,p2 -> C0; p3 -> C1; p4 -> C2 (the pred_norm shape)");
    auto plan3 =
        ssa_ns::calculate_merge_chain_plan<V>(2, std::vector<ulen>{0, 1, 5});
    UT(plan3.chain_count == 1 && plan3.redirect_pred.size() == 2 &&
           plan3.redirect_pred[0] == 1 && plan3.redirect_chain[0] == 0 &&
           plan3.redirect_pred[1] == 5 && plan3.redirect_chain[1] == 0,
       "chain: 3 preds -> 1 chain block, both extras retargeted to it");
    auto plan2 =
        ssa_ns::calculate_merge_chain_plan<V>(2, std::vector<ulen>{0, 1});
    UT(plan2.chain_count == 0 && plan2.redirect_pred.empty(),
       "chain: 2 preds need no chain");
  }
  // 16h. kernel-vs-adapter end to end: a break/continue-heavy program
  // (multi-pred normalization + placement + renaming + destruction,
  // all through the generic kernel) still matches the oracle
  {
    const char *src = "let s = 0; let i = 0;\n"
                      "while (i < 9) {\n"
                      "  i = i + 1;\n"
                      "  if (i == 3) { continue; }\n"
                      "  if (i == 5) { break; }\n"
                      "  if (i > 7) { s = s + 100; } else { s = s + i; }\n"
                      "}\n"
                      "print s; print i;\n";
    Program P = compile(src);
    UT(P.ok, "kernel: break/continue program compiles");
    RunOut o = run_prog(fns_of(P, 1), 100000, {});
    UT(o.out.size() == 2 && o.out[0] == 1 + 2 + 4 && o.out[1] == 5,
       "kernel: s = 1+2+4 (3 skipped, 5 breaks), i = 5");
  }
  // 16i. the PURE graph_ns layer vs the policy wrapper: graph_ns::
  // calculate_phi_placements is the Cytron relation with NO IR
  // assumptions -- single-def vars place their DF(def) phis (the
  // wrapper drops them: the builder's dominance guarantee), one-pred
  // blocks can host phis (an entry sitting in its own cycle; the
  // wrapper's >= 2-slot rule drops them), and the universe transpose
  // handles the var-space/block-space mismatch the square sibling
  // indexes out of bounds on.
  {
    // the diamond of 16a, var-major: var 0 (defs in both arms) ->
    // the join; var 1 (single def at the entry, which dominates
    // everything) -> nothing anywhere
    const std::vector<std::vector<ulen>> diamond = {{1, 2}, {3}, {3}, {}};
    auto A = an_of(diamond);
    std::vector<std::vector<ulen>> defs = {{1, 2}, {0}};
    const auto pure = gns::calculate_phi_placements<V>(A.df, defs);
    UT(row_is(pure, 0, {3}),
       "pure: the diamond's var 0 places at the join (var-major)");
    UT(row_is(pure, 1, {}),
       "pure: single-def-at-entry var places nothing (empty frontier)");
    const auto inv = gns::calculate_inv_phi_placements<V>(A.df, defs);
    UT(row_is(inv, 3, {0}) && row_is(inv, 0, {}) && row_is(inv, 1, {}) &&
           row_is(inv, 2, {}),
       "pure: the transposed index matches 16a's wrapper output");
    // the loop of 16a: the SINGLE-DEF body var gets its DF(def) phi
    // at the header in the pure layer; the wrapper drops it
    const std::vector<std::vector<ulen>> loop = {{1}, {2, 3}, {1}, {}};
    auto A2 = an_of(loop);
    std::vector<std::vector<ulen>> defs2 = {{0, 2}, {2}};
    const auto pure2 = gns::calculate_phi_placements<V>(A2.df, defs2);
    UT(row_is(pure2, 0, {1}) && row_is(pure2, 1, {1}),
       "pure: BOTH vars place at the header -- the single-def one too");
    const auto inv2 = gns::calculate_inv_phi_placements<V>(A2.df, defs2);
    UT(row_is(inv2, 1, {0, 1}), "pure: the header hosts both vars, ascending");
    auto prs2 = preds_of(loop);
    auto pf2 = gns::make_basic_forest<V>(prs2);
    auto pl2 = ssa_ns::calculate_phi_placements<V>(A2.df, pf2, defs2);
    UT(row_is(pl2, 1, {0}),
       "wrapper: the <2-def policy drops var 1 (16a parity)");
    // entry-in-its-own-cycle: the pure layer places at the ENTRY -- a
    // ONE-PRED block (no <2-pred assumption exists down there) -- and
    // the renaming kernel lowers the ARITY-1 record just as naturally
    const std::vector<std::vector<ulen>> ecyc = {{1}, {0, 2}, {}};
    auto A3 = an_of(ecyc);
    std::vector<std::vector<ulen>> defs3 = {{1}};
    const auto pure3 = gns::calculate_phi_placements<V>(A3.df, defs3);
    UT(row_is(pure3, 0, {0}),
       "pure: an entry in its own cycle hosts the phi (1-pred block)");
    const auto inv3 = gns::calculate_inv_phi_placements<V>(A3.df, defs3);
    auto prs3 = preds_of(ecyc);
    auto pf3 = gns::make_basic_forest<V>(prs3);
    auto pl3 = ssa_ns::calculate_phi_placements<V>(A3.df, pf3, defs3);
    UT(row_is(pl3, 0, {}) && row_is(pl3, 1, {}) && row_is(pl3, 2, {}),
       "wrapper: both policies drop it (single def, one pred slot)");
    auto sf3 = gns::make_basic_forest<V>(ecyc);
    std::vector<std::vector<ssa_event_t>> evs3 = {
        {}, {{ssa_def_v, 0, 0}}, {{ssa_use_v, 0, 1}}};
    auto R3 = ssa_ns::rename_ssa<V>(1, 2, A3.dom_tree, sf3, pf3, inv3, evs3);
    UT(R3.phi_block.size() == 1 && R3.phi_block[0] == 0 && R3.phi_var[0] == 0,
       "arity-1: the phi record lives at the entry, var 0");
    UT(R3.phi_entry_begin[1] - R3.phi_entry_begin[0] == 1,
       "arity-1: ONE entry (the lone back-edge slot)");
    UT(R3.phi_entry_pred[R3.phi_entry_begin[0] + 0] == 1 &&
           R3.phi_entry_def[R3.phi_entry_begin[0] + 0] == 0,
       "arity-1: the back edge carries block 1's def");
    UT(R3.reaching_def[1] == 0,
       "arity-1: the dominated exit reads block 1's def, not the phi");
    UT(R3.def_ordinal[2] == 0,
       "arity-1: the phi itself is version 0 at the entry head");
    // zero-def vars: no seeds, nothing placed
    const auto pure0 = gns::calculate_phi_placements<V>(
        A2.df, std::vector<std::vector<ulen>>{{}, {}});
    UT(row_is(pure0, 0, {}) && row_is(pure0, 1, {}),
       "pure: zero-def vars place nothing (no seeds)");
    // the universe transpose: non-square relations both ways -- more
    // rows than the universe, and entries beyond the row count (the
    // square sibling sizes by the row count and would index OOB)
    const std::vector<std::vector<ulen>> rows4 = {{2}, {0, 2}, {1}, {2}};
    auto uni = gns::make_basic_inv_forest_universe<V>(rows4, 3);
    UT(row_is(uni, 0, {1}) && row_is(uni, 1, {2}) && row_is(uni, 2, {0, 1, 3}),
       "universe: 4 var rows over 3 blocks, ascending fill");
    auto uni2 = gns::make_basic_inv_forest_universe<V>(
        std::vector<std::vector<ulen>>{{4}, {0, 4}}, 5);
    UT(row_is(uni2, 0, {1}) && row_is(uni2, 4, {0, 1}),
       "universe: entries beyond the row count (the square trap)");
  }
}

// ---- 17. the generic optimization kernels (mjz::opt_ns,
// opt/opt_generic.hpp) ---------------------------------- Pure index-space
// contracts, no Fn/Instr involved: mark-sweep liveness, scoped value
// numbering (incl. N-ary phi congruence), the conditional-constant engine
// (incl. a 3-pred phi merge and a loop-carried constant), parallel-copy
// sequentialization, the edge-split plan, the CFG flood + splice pairing,
// loop-invariant selection, the copy-rename guard, the trivial-phi pick.
{
  using opt_ns::none_v;
  auto an_of = [](const std::vector<std::vector<ulen>> &adjs) {
    struct A_ {
      gns::treversal_result_t<V> fs, fp, dom_tree, df;
      std::vector<ulen> idom;
    } a;
    a.fs = gns::make_basic_forest<V>(adjs);
    a.fp = gns::make_basic_inv_forest<V>(adjs);
    a.idom = gns::calculate_dominators_from_entry<V>(0, a.fs, a.fp);
    a.dom_tree = gns::make_dominator_forest_given_dominators<V>(a.idom);
    a.df = gns::calculate_dominance_frontiers_given_dominators<V>(a.idom, a.fp);
    return a;
  };
  auto preds_of = [](const std::vector<std::vector<ulen>> &adjs) {
    std::vector<std::vector<ulen>> prs(adjs.size());
    for (ulen b = 0; b < adjs.size(); ++b)
      for (ulen s : adjs[b])
        prs[s].push_back(b);
    return prs;
  };

  // 17a. mark_ns: liveness over the def/use bipartite tables
  {
    std::vector<uintlen_t> def_of{0, 1};
    std::vector<std::vector<uintlen_t>> args{{}, {}, {0}};
    auto live = opt_ns::calculate_live_sites<V>(def_of, args,
                                                std::vector<uintlen_t>{2});
    UT(live.size() == 3 && live[0] && live[2] && !live[1],
       "mark: the root's operand chain lives, the unreferenced def dies");
    std::vector<uintlen_t> def_of2{0, 2};
    std::vector<std::vector<uintlen_t>> args2{{}, {}, {0}, {1}};
    auto live2 = opt_ns::calculate_live_sites<V>(def_of2, args2,
                                                 std::vector<uintlen_t>{3});
    UT(live2[0] && live2[2] && live2[3],
       "mark: transitively feeding the root lives");
    std::vector<std::vector<uintlen_t>> args3{{none_v, 99}};
    auto live3 = opt_ns::calculate_live_sites<V>(
        std::vector<uintlen_t>{none_v}, args3, std::vector<uintlen_t>{0});
    UT(live3[0],
       "mark: none/junk operands bail out harmlessly (no def, no crash)");
  }

  // 17b. vn_ns: dominator-scoped value numbering
  {
    // diamond 0->{1,2}, 1->3, 2->3
    const std::vector<std::vector<ulen>> diamond = {{1, 2}, {3}, {3}, {}};
    auto A = an_of(diamond);
    std::vector<char> cmask(10, 0);
    std::vector<int64_t> cval(10, 0);
    cmask[2] = 1;
    cval[2] = 7; // v2 is the const 7
    cmask[3] = 1;
    cval[3] = 7; // v3 is ALSO the const 7 (a different temp)
    auto bin_site = [](uintlen_t dst, uintlen_t op, bool comm) {
      opt_ns::vn_site_t<V> s{};
      s.tag = opt_ns::vn_bin_v;
      s.dst = dst;
      s.op = op;
      s.flags = comm ? opt_ns::vn_comm_v : 0;
      return s;
    };
    // block 0: d4 = Mul(v0, v2), d9 = Sub(v0, v1)
    // block 1: d5 = Add(v0, v1) -- no Add in scope: MISS
    // block 2: d6 = Add(v0, v1) -- block 1's entry truncated: MISS
    //   (sibling recomputations never unify WITH EACH OTHER)
    // block 3 (dominated by 0): d7 = Mul(v3, v0) -- commutative +
    //   const-by-VALUE keying hits d4 across different const temps;
    //   d8 = Sub(v0, v1) -- the plain dominated recompute hits d9
    std::vector<std::vector<uintlen_t>> sob = {{0, 1}, {2}, {3}, {4, 5}};
    std::vector<opt_ns::vn_site_t<V>> sinfo = {
        bin_site(4, 200, true), bin_site(9, 300, false),
        bin_site(5, 100, true), bin_site(6, 100, true),
        bin_site(7, 200, true), bin_site(8, 300, false)};
    std::vector<std::vector<uintlen_t>> sargs(6);
    sargs[0] = {0, 2}; // Mul(v0, v2)
    sargs[1] = {0, 1}; // Sub(v0, v1)
    sargs[2] = {0, 1}; // Add(v0, v1)
    sargs[3] = {0, 1}; // Add(v0, v1)
    sargs[4] = {3, 0}; // Mul(v3, v0) -- the OTHER operand order
    sargs[5] = {0, 1}; // Sub(v0, v1)
    std::vector<std::vector<uintlen_t>> spreds(6);
    auto R = opt_ns::value_number<V>(10, A.dom_tree, sob, sinfo, sargs, spreds,
                                     cmask, cval);
    UT(R.ren[5] == none_v && R.ren[6] == none_v,
       "vn: sibling recomputations do NOT unify (scope exit truncates)");
    UT(R.ren[7] == 4,
       "vn: commutative + const-by-VALUE keying unifies across temps");
    UT(R.ren[8] == 9,
       "vn: a recompute under the dominating def unifies onto it");
    UT(R.keep[4] == 0 && R.keep[5] == 0 && R.keep[2] != 0 && R.keep[3] != 0,
       "vn: hits are exactly the dominated recomputations");
  }
  // 17b2. phi congruence, taint, and the N-ARY record (3 preds)
  {
    // 3-way branch into one join: 0->{1,2,3}, each ->4 (a 3-pred join
    // this IR cannot spell; the kernel only sees rows)
    const std::vector<std::vector<ulen>> tri = {{1, 2, 3}, {4}, {4}, {4}, {}};
    auto A = an_of(tri);
    auto prs = preds_of(tri);
    std::vector<std::vector<uintlen_t>> sob = {{}, {}, {}, {}, {0, 1, 2}};
    std::vector<opt_ns::vn_site_t<V>> sinfo(3);
    std::vector<std::vector<uintlen_t>> sargs(3), spreds(3);
    for (int i = 0; i < 3; ++i) {
      sinfo[i].tag = opt_ns::vn_phi_v;
      sinfo[i].dst = uintlen_t(20 + i);
    }
    // phi0: (arg 4, pred 1) (arg 5, pred 2) (arg 6, pred 3)
    sargs[0] = {4, 5, 6};
    spreds[0] = {1, 2, 3};
    // phi1: the SAME incoming map -> congruent, renames onto phi0
    sargs[1] = {4, 5, 6};
    spreds[1] = {1, 2, 3};
    // phi2: references phi0's result among its args -> tainted pair
    sargs[2] = {20, 5, 6};
    spreds[2] = {1, 2, 3};
    std::vector<char> cmask(30, 0);
    std::vector<int64_t> cval(30, 0);
    auto R = opt_ns::value_number<V>(30, A.dom_tree, sob, sinfo, sargs, spreds,
                                     cmask, cval);
    UT(R.ren[21] == 20,
       "vn: 3-entry phi congruence (N-ary, same incoming map per pred)");
    UT(R.ren[22] == none_v,
       "vn: the taint rule refuses a pair whose args reference the other");
    (void)prs;
  }

  // 17c. sccp_ns: the conditional constant engine
  {
    // 17c1. branch pruning: cbr on a CONST cond makes only the taken
    // edge executable; the dead arm's def stays TOP; the phi at the
    // join merges the live edge only. (Every block that ends in an
    // edge carries a branch site -- the Br sites below -- exactly
    // like the IR's terminators.)
    opt_ns::sccp_engine_t<V> E{};
    // blocks: 0 -> {1, 2}; 1 -> {3}; 2 -> {3}; 3 -> {}
    E.succs = {{1, none_v}, {3, none_v}, {3, none_v}, {none_v, none_v}};
    E.sites_of_block = {{0, 1}, {2, 3}, {4, 5}, {6}};
    E.sites.resize(7);
    E.sites[0].kind = opt_ns::sccp_value_v; // d0 = const 1
    E.sites[0].dst = 0;
    E.sites[1].kind = opt_ns::sccp_branch_v; // cbr d0: t->1, f->2
    E.sites[2].kind = opt_ns::sccp_value_v;  // d1 = copy d0
    E.sites[2].dst = 1;
    E.sites[3].kind = opt_ns::sccp_branch_v; // br 3
    E.sites[4].kind = opt_ns::sccp_value_v;  // d2 = const 99 (dead arm)
    E.sites[4].dst = 2;
    E.sites[5].kind = opt_ns::sccp_branch_v; // br 3 (never executable)
    E.sites[6].kind = opt_ns::sccp_phi_v;    // d3 = phi(d1@1, d2@2)
    E.sites[6].dst = 3;
    E.sites[6].phi = 0;
    E.phi_block = {3};
    E.phi_entry_dst = {3};
    E.phi_entry_pred = {1, 2};
    E.phi_entry_arg = {1, 2};
    E.phi_entry_begin = {0, 2};
    E.uses = {{1}, {6}, {6}, {}}; // d0 read by the cbr; d1/d2 by the phi
    E.entry_block = 0;
    E.lat.assign(4, opt_ns::sccp_top_v);
    E.cval.assign(4, 0);
    bool ran = E.run(
        [&](uintlen_t s, uintlen_t b) {
          if (s == 0)
            E.lower(0, opt_ns::sccp_cn_v, 1); // d0 = const 1
          else if (s == 1) {                  // the cbr on d0
            if (E.lat[0] == opt_ns::sccp_cn_v)
              E.push_edge(b, E.cval[0] != 0 ? 0 : 1);
            else if (E.lat[0] == opt_ns::sccp_od_v) {
              E.push_edge(b, 0);
              E.push_edge(b, 1);
            }
          } else if (s == 2) // d1 = copy d0
            E.lower(1, E.lat[0], E.cval[0]);
          else if (s == 3 || s == 5) // br -> the only edge
            E.push_edge(b, 0);
          else if (s == 4) // d2 = const 99 (never executable)
            E.lower(2, opt_ns::sccp_cn_v, 99);
        },
        100000);
    UT(ran, "sccp: the engine terminates on the pruning case");
    UT(E.lat[0] == opt_ns::sccp_cn_v && E.cval[0] == 1,
       "sccp: the const cond is CN 1");
    UT(E.lat[2] == opt_ns::sccp_top_v && !E.bexec[2],
       "sccp: the dead arm stays TOP in a never-executable block");
    UT(E.lat[3] == opt_ns::sccp_cn_v && E.cval[3] == 1,
       "sccp: the phi merges the EXECUTABLE edge only (loop-free join)");
  }
  {
    // 17c2. loop-carried constant: i = phi(0, j) with j = i around a
    // self-loop -- the classic SCCP proof the local fold cannot make
    opt_ns::sccp_engine_t<V> E{};
    // blocks: 0 -> {1}; 1 -> {1, 2} (back edge slot 0, exit slot 1); 2 -> {}
    E.succs = {{1, none_v}, {1, 2}, {none_v, none_v}};
    E.sites_of_block = {{0, 4}, {1, 2, 3}, {}};
    E.sites.resize(5);
    E.sites[0].kind = opt_ns::sccp_value_v; // d3 = const 0
    E.sites[0].dst = 3;
    E.sites[4].kind = opt_ns::sccp_branch_v; // br 1
    E.sites[1].kind = opt_ns::sccp_phi_v;    // d1 = phi(d3@0, d2@1)
    E.sites[1].dst = 1;
    E.sites[1].phi = 0;
    E.sites[2].kind = opt_ns::sccp_value_v; // d2 = copy d1
    E.sites[2].dst = 2;
    E.sites[3].kind = opt_ns::sccp_branch_v; // cbr d0 (OVERDEF) -> both
    E.phi_block = {1};
    E.phi_entry_dst = {1};
    E.phi_entry_pred = {0, 1};
    E.phi_entry_arg = {3, 2};
    E.phi_entry_begin = {0, 2};
    E.uses = {
        {3}, {2}, {1}, {1}}; // cbr reads d0; copy reads d1; phi reads d3/d2
    E.entry_block = 0;
    E.lat.assign(4, opt_ns::sccp_top_v);
    E.cval.assign(4, 0);
    E.lat[0] = opt_ns::sccp_od_v; // the loop cond is a runtime value
    bool ran = E.run(
        [&](uintlen_t s, uintlen_t b) {
          if (s == 0)
            E.lower(3, opt_ns::sccp_cn_v, 0);
          else if (s == 2)
            E.lower(2, E.lat[1], E.cval[1]); // j = i
          else if (s == 3) {                 // cond OVERDEF: both edges
            E.push_edge(b, 0);
            E.push_edge(b, 1);
          } else if (s == 4) // br -> the only edge
            E.push_edge(b, 0);
        },
        100000);
    UT(ran && E.lat[1] == opt_ns::sccp_cn_v && E.cval[1] == 0,
       "sccp: the loop-carried i = phi(0, i) converges to CONST 0");
    UT(E.lat[2] == opt_ns::sccp_cn_v && E.cval[2] == 0,
       "sccp: the back-edge copy j = i is also 0");
  }
  {
    // 17c3. the N-ARY phi merge: a 3-way branch into one join (a CFG
    // shape this IR cannot spell); three executable in-edges, two
    // carrying 5 -> CONST 5 with a differing third -> OVERDEF
    opt_ns::sccp_engine_t<V> E{};
    // 0 -> {1, 2, 3}; each of 1..3 -> {4}
    E.succs = {{1, 2, 3}, {4}, {4}, {4}, {}};
    E.sites_of_block = {{0, 4}, {1, 5}, {2, 6}, {3, 7}, {8, 9}};
    E.sites.resize(10);
    E.sites[0].kind = opt_ns::sccp_value_v; // d0 = const 5
    E.sites[0].dst = 0;
    E.sites[4].kind = opt_ns::sccp_branch_v; // the 3-way branch
    E.sites[1].kind = opt_ns::sccp_value_v;  // d1 = copy d0
    E.sites[1].dst = 1;
    E.sites[5].kind = opt_ns::sccp_branch_v; // br 4
    E.sites[2].kind = opt_ns::sccp_value_v;  // d2 = copy d0
    E.sites[2].dst = 2;
    E.sites[6].kind = opt_ns::sccp_branch_v; // br 4
    E.sites[3].kind = opt_ns::sccp_value_v;  // d3 = const 9 (differs)
    E.sites[3].dst = 3;
    E.sites[7].kind = opt_ns::sccp_branch_v; // br 4
    E.sites[8].kind = opt_ns::sccp_phi_v;    // d4 = phi(d1@1, d2@2, d3@3)
    E.sites[8].dst = 4;
    E.sites[8].phi = 0;
    E.sites[9].kind = opt_ns::sccp_phi_v; // d5 = phi(d1@1, d2@2, d0@3)
    E.sites[9].dst = 5;
    E.sites[9].phi = 1;
    E.phi_block = {4, 4};
    E.phi_entry_dst = {4, 5};
    E.phi_entry_pred = {1, 2, 3, 1, 2, 3};
    E.phi_entry_arg = {1, 2, 3, 1, 2, 0};
    E.phi_entry_begin = {0, 3, 6};
    E.uses = {{9}, {8, 9}, {8, 9}, {8}, {}, {}}; // one row per var
    E.entry_block = 0;
    E.lat.assign(6, opt_ns::sccp_top_v);
    E.cval.assign(6, 0);
    bool ran = E.run(
        [&](uintlen_t s, uintlen_t b) {
          if (s == 0)
            E.lower(0, opt_ns::sccp_cn_v, 5);
          else if (s == 1 || s == 2) // d1/d2 = copy d0
            E.lower(s == 1 ? 1 : 2, E.lat[0], E.cval[0]);
          else if (s == 3) // d3 = const 9
            E.lower(3, opt_ns::sccp_cn_v, 9);
          else if (s == 4) { // the 3-way branch: all three edges
            E.push_edge(b, 0);
            E.push_edge(b, 1);
            E.push_edge(b, 2);
          } else if (s == 5 || s == 6 || s == 7) // br 4
            E.push_edge(b, 0);
        },
        100000);
    UT(ran && E.lat[4] == opt_ns::sccp_od_v,
       "sccp: 3-entry phi with a differing arg meets to OVERDEF");
    UT(E.lat[5] == opt_ns::sccp_cn_v && E.cval[5] == 5,
       "sccp: 3-entry phi with all-equal args is CONST (N-ary merge)");
  }
  {
    // 17c4. the duplicate-pred rule: a pred whose BOTH slots target the
    // phi block carries the arg when EITHER edge is executable
    opt_ns::sccp_engine_t<V> E{};
    // 0 -> {1, 1} (both arms on one block); 1 -> {}
    E.succs = {{1, 1}, {none_v}};
    E.sites_of_block = {{0, 1}, {2}};
    E.sites.resize(3);
    E.sites[0].kind = opt_ns::sccp_value_v; // d0 = const 3
    E.sites[0].dst = 0;
    E.sites[1].kind = opt_ns::sccp_branch_v; // cbr OVERDEF: both edges
    E.sites[2].kind = opt_ns::sccp_phi_v;    // d1 = phi(d0@0, d0@0)
    E.sites[2].dst = 1;
    E.sites[2].phi = 0;
    E.phi_block = {1};
    E.phi_entry_dst = {1};
    E.phi_entry_pred = {0, 0};
    E.phi_entry_arg = {0, 0};
    E.phi_entry_begin = {0, 2};
    E.uses = {{2}, {}}; // the phi reads d0 (both entries)
    E.entry_block = 0;
    E.lat.assign(2, opt_ns::sccp_top_v);
    E.cval.assign(2, 0);
    E.lat[0] = opt_ns::sccp_cn_v;
    E.cval[0] = 3;
    bool ran = E.run(
        [&](uintlen_t s, uintlen_t b) {
          if (s == 1) {
            E.push_edge(b, 0);
            E.push_edge(b, 1);
          }
        },
        100000);
    UT(ran && E.lat[1] == opt_ns::sccp_cn_v && E.cval[1] == 3,
       "sccp: duplicate-pred entries merge over EITHER edge (N-ary rule)");
  }

  // 17d. pcopy_ns: parallel-copy sequentialization
  {
    // chain: c<-b, b<-a: neither dst is a pending source, so both
    // emit in ROW order (c<-b first is safe: nothing reads c)
    opt_ns::pcopy_stats_t st{};
    auto ops = opt_ns::sequentialize_pcopy(
        {{2, 1}, {1, 0}}, [](uintlen_t) { return 100; }, st);
    UT(ops.size() == 2 && ops[0].dst == 2 && ops[0].src == 1 &&
           ops[1].dst == 1 && ops[1].src == 0,
       "pcopy: a hazard-free set emits in row order");
    // swap cycle: a<-b, b<-a -> one temp, three copies
    opt_ns::pcopy_stats_t st2{};
    auto ops2 = opt_ns::sequentialize_pcopy(
        {{0, 1}, {1, 0}}, [](uintlen_t) { return 100; }, st2);
    UT(ops2.size() == 3 && st2.cycles_broken == 1 && ops2[0].dst == 100 &&
           ops2[0].src == 1,
       "pcopy: the swap cycle breaks by saving ONE source into a temp");
    UT(ops2[1].dst == 1 && ops2[1].src == 0 && ops2[2].dst == 0 &&
           ops2[2].src == 100,
       "pcopy: the broken swap runs temp<-src, dst<-src, dst<-temp");
    // 3-cycle rotation
    opt_ns::pcopy_stats_t st3{};
    auto ops3 = opt_ns::sequentialize_pcopy(
        {{0, 1}, {1, 2}, {2, 0}}, [](uintlen_t) { return 200; }, st3);
    UT(ops3.size() == 4 && st3.cycles_broken == 1,
       "pcopy: a 3-cycle rotation needs one temp and four copies");
    // undef arg -> const zero; self-copy stripped
    opt_ns::pcopy_stats_t st4{};
    auto ops4 = opt_ns::sequentialize_pcopy(
        {{5, 5}, {6, none_v}}, [](uintlen_t) { return 300; }, st4);
    UT(ops4.size() == 1 && ops4[0].zero && ops4[0].dst == 6 &&
           st4.self_copies == 1 && st4.zeros == 1,
       "pcopy: self-copies strip, undef args materialize zero");
  }

  // 17e. the edge-split plan
  {
    // 0 fans out to {3, 2}; 3 holds phis -> split edge (0, slot 0)
    std::vector<std::vector<uintlen_t>> rows = {{3, 2}, {}, {}, {}};
    std::vector<char> has_phi = {0, 0, 0, 1};
    auto plan = opt_ns::calculate_edge_splits(rows, has_phi);
    UT(plan.size() == 1 && plan[0].pred == 0 && plan[0].slot == 0 &&
           plan[0].target == 3 && plan[0].fresh == 4,
       "split: the 2-succ pred's phi-targeted edge gets a fresh block");
    // both arms on one target: NO split (every path runs the copies)
    std::vector<std::vector<uintlen_t>> rows2 = {{3, 3}, {}, {}, {}};
    auto plan2 = opt_ns::calculate_edge_splits(rows2, has_phi);
    UT(plan2.empty(), "split: cbr(c, Y, Y) needs no split");
    // single-succ pred: no split
    std::vector<std::vector<uintlen_t>> rows3 = {{3, none_v}, {}, {}, {}};
    auto plan3 = opt_ns::calculate_edge_splits(rows3, has_phi);
    UT(plan3.empty(), "split: a single-succ pred hosts its own copies");
    // both targets phi-free: no split
    std::vector<char> has_phi0 = {0, 0, 0, 0};
    auto plan4 = opt_ns::calculate_edge_splits(rows, has_phi0);
    UT(plan4.empty(), "split: nothing to place without phis");
  }

  // 17f. cfg_ns: the flood + the splice pairing plan
  {
    // an unreachable island next to a reachable chain
    std::vector<std::vector<uintlen_t>> adjs = {{1}, {2}, {}, {4}, {3}};
    auto reach = opt_ns::calculate_reachable_mask(adjs, 0);
    UT(reach.size() == 5 && reach[0] && reach[1] && reach[2] && !reach[3] &&
           !reach[4],
       "flood: the island stays dark");
    // splice pairing on a chain 0->1->2->3: two matchings, one sweep
    std::vector<std::vector<uintlen_t>> srows = {{1}, {2}, {3}, {}};
    std::vector<std::vector<uintlen_t>> prows = preds_of(srows);
    std::vector<char> has_phi = {0, 0, 0, 0};
    auto plan = opt_ns::calculate_splice_plan(srows, prows, 0, has_phi);
    UT(plan.merges_of[0] == 1 && plan.absorbed_of[1] == 0,
       "splice: block 0 absorbs its single pred'd successor");
    UT(plan.merges_of[2] == 3 && plan.absorbed_of[3] == 2,
       "splice: the chain pairs twice in ONE sweep (matchings only)");
    // a SELF-LOOP successor and the ENTRY successor are kept
    auto plan_self = opt_ns::calculate_splice_plan(
        std::vector<std::vector<uintlen_t>>{{0}},
        std::vector<std::vector<uintlen_t>>{{0}}, 0, has_phi);
    UT(plan_self.merges_of[0] == none_v,
       "splice: the self-loop successor is kept (s == b)");
    auto plan_entry = opt_ns::calculate_splice_plan(
        std::vector<std::vector<uintlen_t>>{{1}, {0}},
        std::vector<std::vector<uintlen_t>>{{1}, {0}}, 0, has_phi);
    UT(plan_entry.merges_of[1] == none_v && plan_entry.absorbed_of[0] == none_v,
       "splice: nobody absorbs the entry (s == entry is kept)");
    // a phi-holding successor is refused
    std::vector<std::vector<uintlen_t>> srows3 = {{1}, {2}, {}};
    std::vector<std::vector<uintlen_t>> prows3 = preds_of(srows3);
    std::vector<char> has_phi3 = {0, 1, 0};
    auto plan3x = opt_ns::calculate_splice_plan(srows3, prows3, 0, has_phi3);
    UT(plan3x.merges_of[0] == none_v,
       "splice: a phi-holding successor is refused");
  }

  // 17g. inv_ns: loop-invariant selection
  {
    std::vector<char> in_loop = {1, 1, 1, 0, 0}; // h=0, body=1,2 inside
    auto [pre, outside] = opt_ns::find_unique_outside_pred(
        std::vector<uintlen_t>{3, 1, 2}, in_loop);
    UT(pre == 3 && outside == 1, "inv: the unique outside pred is found");
    auto [pre2, outside2] = opt_ns::find_unique_outside_pred(
        std::vector<uintlen_t>{3, 4, 1}, in_loop);
    UT(outside2 == 2, "inv: two outside preds disqualify the loop");
    // the invariance predicate + the header-first scan discipline
    // blocks: 0 = header, 1 = body, 3 = preheader, 4 = an outside def
    // that DOMINATES the preheader, 2 = an outside def that does NOT
    // dom intervals: blocks 0 and 4 dominate the preheader 3; block 2
    // (ib 7) does not; a begin of 0 is the unreachable/invalid marker
    std::vector<ulen> db{1, 6, 7, 2, 1};
    std::vector<ulen> de{9, 6, 7, 5, 9};
    std::vector<uintlen_t> def_block(6, none_v);
    def_block[0] = 4; // v0: outside, dominates pre
    def_block[1] = 1; // v1: inside the loop
    def_block[2] = 2; // v2: outside but NOT dominating pre (ib[2] = 0)
    def_block[3] = 1; // v3: a CONST defined INSIDE (position-independent)
    std::vector<char> cmask(6, 0);
    cmask[3] = 1; // v3 is that inside-loop const
    std::vector<std::vector<uintlen_t>> sob = {{0, 1}, {2}};
    std::vector<std::vector<uintlen_t>> sargs(3);
    sargs[0] = {0};
    sargs[1] = {1};
    sargs[2] = {3};
    std::vector<char> eligible = {1, 1, 1};
    auto pick = opt_ns::calculate_first_invariant_site<V>(
        std::vector<uintlen_t>{1}, 0, 3, in_loop, def_block, cmask, db, de, sob,
        sargs, eligible);
    UT(pick.first == 0 && pick.second == 0,
       "inv: the header's invariant site is found FIRST (the unswitch feed)");
    // make the header site read the inside def: the body's const site wins
    sargs[0] = {1};
    pick = opt_ns::calculate_first_invariant_site<V>(
        std::vector<uintlen_t>{1}, 0, 3, in_loop, def_block, cmask, db, de, sob,
        sargs, eligible);
    UT(pick.first == 1 && pick.second == 2,
       "inv: an inside-const operand is invariant (value, not location)");
    // an outside def that does not dominate the preheader is NOT invariant
    sargs[2] = {2};
    eligible = {1, 0, 1};
    pick = opt_ns::calculate_first_invariant_site<V>(
        std::vector<uintlen_t>{1}, 0, 3, in_loop, def_block, cmask, db, de, sob,
        sargs, eligible);
    UT(pick.first == none_v,
       "inv: outside-but-not-dominating defs refuse the hoist");
  }

  // 17h. ren_ns: the copy-rename builder + the crossed-pair guard
  {
    // chain: d1 <- s0, d2 <- d1: both resolve to s0 in one hop
    auto ren = opt_ns::build_copy_rename(
        3, std::vector<std::pair<uintlen_t, uintlen_t>>{{1, 0}, {2, 1}});
    UT(ren[1] == 0 && ren[2] == 0,
       "ren: chains die in one hop (transitive resolve at record time)");
    // crossed pair: d1 <- d0 then d0 <- d1 -- the closing copy must NOT
    // be recorded (a self-loop entry would never resolve; seed 7406)
    auto ren2 = opt_ns::build_copy_rename(
        2, std::vector<std::pair<uintlen_t, uintlen_t>>{{1, 0}, {0, 1}});
    UT(ren2[1] == 0 && ren2[0] == none_v,
       "ren: the crossed closing copy stays unrecorded");
    UT(opt_ns::resolve_capped(ren2, none_v) == none_v,
       "ren: the capped resolve passes none through");
    UT(opt_ns::resolve_capped(ren2, 0) == 0 &&
           opt_ns::resolve_capped(ren2, 1) == 0,
       "ren: d0 keeps its name (unpropagated), d1 renames to d0");
  }

  // 17i. the trivial-phi pick (N-ary)
  {
    UT(opt_ns::calculate_trivial_phi_pick(9, std::vector<uintlen_t>{4, 4}) == 4,
       "pick: phi(a, a) collapses to a");
    UT(opt_ns::calculate_trivial_phi_pick(9, std::vector<uintlen_t>{9, 4}) == 4,
       "pick: phi(self, a) collapses to a");
    UT(opt_ns::calculate_trivial_phi_pick(9, std::vector<uintlen_t>{4, 5}) ==
           none_v,
       "pick: phi(a, b) is genuine");
    UT(opt_ns::calculate_trivial_phi_pick(9, std::vector<uintlen_t>{9, 9}) ==
           none_v,
       "pick: phi(self, self) stays");
    UT(opt_ns::calculate_trivial_phi_pick(
           9, std::vector<uintlen_t>{none_v, 4}) == 4,
       "pick: an absent arg is ignored");
    UT(opt_ns::calculate_trivial_phi_pick(9, std::vector<uintlen_t>{4, 4, 4}) ==
           4,
       "pick: the N-ary phi(a, a, a) collapses (arity 3)");
    UT(opt_ns::calculate_trivial_phi_pick(9, std::vector<uintlen_t>{4, 5, 4}) ==
           none_v,
       "pick: the N-ary phi(a, b, a) is genuine");
  }
}

// ---- 18. the second kernel wave (mjz::opt_ns, opt/opt_generic.hpp):
// loop-shape CFG math, the supplier-routing fixpoint, the alias
// identity fixpoint, the dominator-scoped memory table, the interval
// lattice + the abstract-interpretation driver, the mixed sort key,
// the region leak scan, the interprocedural liveness fixpoint, class
// refinement -- with the N-ary cases the 2-arg IR cannot spell.
{
  using opt_ns::none_v;

  // 18a. header-pred classification + the loop shape
  {
    std::vector<char> inL{0, 1, 1, 0};
    auto hp =
        opt_ns::calculate_header_preds(std::vector<uintlen_t>{0, 1, 2}, inL);
    UT(hp.n_in == 2 && hp.n_out == 1 && hp.latch == 2 && hp.pre == 0,
       "header preds: last inside is the latch, last outside the pre");
    auto sh =
        opt_ns::calculate_loop_shape(std::vector<uintlen_t>{0, 1}, inL, 3, 1);
    UT(sh.ok && sh.pre == 0 && sh.latch == 1 && sh.body == 1 && sh.X == 3,
       "loop shape: one arm stays (body), one leaves (X)");
    UT(!opt_ns::calculate_loop_shape(std::vector<uintlen_t>{0, 1}, inL, 3, 3)
            .ok,
       "loop shape: both arms out is not test-first");
    UT(!opt_ns::calculate_loop_shape(std::vector<uintlen_t>{0, 1, 1}, inL, 3, 1)
            .ok,
       "loop shape: two in-preds is not single-latch");
  }

  // 18b. the exit-edge inventory
  {
    // blocks 0,1,2 in-loop; 3,4 outside; b1 exits to 3 and 4
    std::vector<char> inL{1, 1, 1, 0, 0};
    std::vector<std::vector<uintlen_t>> succs{{1}, {3, 4}, {0}, {}, {}};
    std::vector<opt_ns::exit_edge_t> ex{};
    UT(opt_ns::collect_exit_edges(succs, inL, none_v, none_v, none_v, none_v,
                                  ex),
       "exits: collected");
    UT(ex.size() == 2 && ex[0].from == 1 && ex[0].to == 3 && ex[1].to == 4,
       "exits: both of b1's out-edges, slot order");
    ex.clear();
    UT(opt_ns::collect_exit_edges(succs, inL, 1, 3, none_v, none_v, ex) &&
           ex.size() == 1 && ex[0].from == 1 && ex[0].to == 4,
       "exits: the primary edge (1,3) is skipped, the other stays");
    ex.clear();
    UT(!opt_ns::collect_exit_edges(succs, inL, none_v, none_v, 3, none_v, ex),
       "exits: an edge targeting the forbidden block bails");
    ex.clear();
    std::vector<std::vector<uintlen_t>> degen{{1}, {4, 4}, {0}, {}, {}};
    UT(!opt_ns::collect_exit_edges(degen, inL, none_v, none_v, none_v, none_v,
                                   ex),
       "exits: every arm on one outside target is degenerate");
  }

  // 18c. the supplier-routing fixpoint (taildup's mini SSA), incl.
  // a THREE-pred born phi the 2-arg IR cannot spell
  {
    // 0 entry; 1,2 dead (the duplicated tail's preds); 3 = U0, 4 = U1;
    // 5 = the re-merge of {3,4}; 6 = the dead tail T (preds {1,2})
    std::vector<std::vector<uintlen_t>> preds{{},  {0},    {0},   {1},
                                              {2}, {3, 4}, {1, 2}};
    std::vector<char> dead{0, 1, 1, 0, 0, 0, 1};
    std::vector<char> dom0{0, 0, 0, 1, 0, 0, 0}; // U0=3 dominates 3 only
    std::vector<char> dom1{0, 0, 0, 0, 1, 0, 0}; // U1=4 dominates 4 only
    auto R = opt_ns::calculate_supplier_routes(preds, dead, dom0, dom1, 2);
    UT(R.route[1] == opt_ns::route_dead_v && R.route[6] == opt_ns::route_dead_v,
       "route: the dead blocks stay dead");
    UT(R.route[3] == 0 && R.route[4] == 1,
       "route: a block under one source reads it outright");
    UT(R.route[5] == 2 && R.sites.size() == 1 && R.sites[0].W == 5,
       "route: the 2-pred re-merge births a phi there");
    UT(R.sites[0].preds.size() == 2 && R.sites[0].proutes[0] == 0 &&
           R.sites[0].proutes[1] == 1,
       "route: the born site records its preds' routes");
    // the arity bound as a PARAMETER: a 3-pred join of routes {0,0,1}
    // births an N-ary phi under bound 3, is unroutable under bound 2
    std::vector<std::vector<uintlen_t>> preds3{{}, {0}, {0}, {0}, {1, 2, 3}};
    std::vector<char> d3(5, 0);
    std::vector<char> m0{0, 1, 1, 0, 0}; // U0 dominates 1,2
    std::vector<char> m1{0, 0, 0, 1, 0}; // U1 dominates 3
    auto R3 = opt_ns::calculate_supplier_routes(preds3, d3, m0, m1, 3);
    UT(R3.route[4] == 2 && R3.sites.size() == 1 &&
           R3.sites[0].preds.size() == 3,
       "route: a 3-pred join with arity bound 3 births an N-ary phi");
    auto R2 = opt_ns::calculate_supplier_routes(preds3, d3, m0, m1, 2);
    UT(R2.route[4] == opt_ns::route_dead_v,
       "route: the same join under arity bound 2 is unroutable (the IR's "
       "policy, not the kernel's)");
    // the need closure: one read through site 0 needs both sources
    std::vector<std::pair<uintlen_t, uintlen_t>> reads{{2, 7}};
    std::vector<std::vector<char>> need{};
    std::vector<std::vector<uintlen_t>> bvals{};
    opt_ns::calculate_route_needs(2, 8, R.sites, reads, need, bvals);
    UT(need[0][7] && need[1][7] && !need[0][0] && bvals[0].size() == 1 &&
           bvals[0][0] == 7,
       "needs: a read through the born site needs both suppliers");
  }

  // 18d. the alias identity fixpoint (N-ary phi agreement)
  {
    std::vector<uintlen_t> single(8, none_v);
    single[0] = 5; // the seed: v0 names allocation 5
    std::vector<opt_ns::alias_def_t> defs{
        {.var = 1, .kind = opt_ns::alias_copy_v, .src = 0},
        {.var = 2, .kind = opt_ns::alias_copy_v, .src = 1},
        {.var = 3, .kind = opt_ns::alias_phi_v, .args = {0, 1, 2}}, // 3-ary!
        {.var = 4, .kind = opt_ns::alias_phi_v, .args = {0, 1, 6}},
        {.var = 5, .kind = opt_ns::alias_phi_v, .args = {0, none_v}},
        {.var = 6, .kind = opt_ns::alias_other_v},
        {.var = 7, .kind = opt_ns::alias_phi_v, .args = {none_v, none_v}}};
    opt_ns::resolve_single_ids<V>(defs, single, 8, 64);
    UT(single[1] == 5 && single[2] == 5,
       "alias: the copy chain resolves to the seed id");
    UT(single[3] == 5,
       "alias: the N-ary phi(a,a,a) agrees -- single (arity 3)");
    UT(single[4] == none_v, "alias: the phi over an obscure arg is obscure");
    UT(single[5] == none_v && single[7] == none_v,
       "alias: an absent arg is obscure (the conservative rule)");
  }

  // 18e. the dominator-scoped memory table (watermark scoping + kill)
  {
    // dom chain 0 -> 1 -> 2 (the kernel takes the CSR forest)
    std::vector<std::vector<ulen>> dom_rows{{1}, {2}, {}};
    auto dom = gns::make_basic_forest<V>(dom_rows);
    std::vector<std::vector<opt_ns::mem_event_t>> evs(3);
    evs[0].push_back({.kind = opt_ns::mem_store_v,
                      .group = 0,
                      .key_tag = 1,
                      .key_b = 3,
                      .val = 10});
    evs[1].push_back(
        {.kind = opt_ns::mem_load_v,
         .group = 0,
         .key_tag = 1,
         .key_b = 3,
         .val = 20}); // hits the store, then records (20, from_load)
    evs[2].push_back(
        {.kind = opt_ns::mem_load_v,
         .group = 0,
         .key_tag = 1,
         .key_b = 3,
         .val = 30}); // b1's entry died with b1's subtree: hits the STORE
    std::vector<char> drop{0, 0, 0};
    auto may_kill = [](uintlen_t eg, uintlen_t, uintlen_t, uintlen_t ekb,
                       const opt_ns::mem_event_t &ev) {
      return eg == ev.group && ev.key_tag != 0 && ekb == ev.key_b;
    };
    auto hits = opt_ns::run_dom_scoped_table<V>(dom, evs, drop, evs, may_kill);
    UT(hits.size() == 2 && hits[0].found && hits[0].val == 10 &&
           hits[0].from_store,
       "mem: the load forwards the dominating store");
    UT(hits[1].found && hits[1].val == 10 && hits[1].from_store,
       "mem: a sibling subtree's entries died by the seq watermark");
    // the kill rule + a join drop
    std::vector<std::vector<opt_ns::mem_event_t>> evs2(3);
    evs2[0].push_back({.kind = opt_ns::mem_store_v,
                       .group = 0,
                       .key_tag = 1,
                       .key_b = 3,
                       .val = 10});
    evs2[1].push_back({.kind = opt_ns::mem_store_v,
                       .group = 0,
                       .key_tag = 1,
                       .key_b = 3,
                       .val = 11});
    evs2[2].push_back({.kind = opt_ns::mem_load_v,
                       .group = 0,
                       .key_tag = 1,
                       .key_b = 3,
                       .val = 30});
    std::vector<char> drop2{0, 0, 1}; // block 2 is a join: table drops
    auto hits2 =
        opt_ns::run_dom_scoped_table<V>(dom, evs2, drop2, evs2, may_kill);
    UT(!hits2[0].found, "mem: the join drop empties the table before the load");
    auto hits3 =
        opt_ns::run_dom_scoped_table<V>(dom, evs2, drop, evs2, may_kill);
    UT(hits3[0].found && hits3[0].val == 11 && hits3[0].from_store,
       "mem: the second store kills the first (same slot)");
  }

  // 18f. the interval lattice
  {
    using opt_ns::iv_ns::iv_t;
    UT(opt_ns::iv_ns::iv_join(iv_t{0, 0, true}, iv_t{2, 5, false}).lo == 2,
       "iv: hull with bottom is the identity");
    iv_t old{0, 10, false};
    opt_ns::iv_ns::iv_narrow(old, iv_t{5, 20, false});
    UT(old.lo == 5 && old.hi == 10, "iv: narrowing intersects");
    opt_ns::iv_ns::iv_narrow(old, iv_t{50, 60, false});
    UT(old.lo == 5 && old.hi == 10,
       "iv: an empty intersection keeps the old sound state");
    UT(opt_ns::iv_ns::iv_excludes_zero(iv_t{1, 5, false}) &&
           !opt_ns::iv_ns::iv_excludes_zero(iv_t{-1, 5, false}),
       "iv: the divisor trap predicates");
  }

  // 18g. the abstract-interpretation driver: a counter widened at the
  // header only, then narrowed -- [5,5] grows to [5, MAX] and stays
  {
    struct CB {
      void entry_top(std::vector<opt_ns::iv_ns::iv_t> &st) const {
        for (auto &x : st)
          x = {INT64_MIN, INT64_MAX, false};
      }
      void refine_edge(uintlen_t, uintlen_t,
                       std::vector<opt_ns::iv_ns::iv_t> &) const {}
      void phi_join(uintlen_t, std::vector<opt_ns::iv_ns::iv_t> &,
                    const opt_ns::iv_ns::iv_t *, uintlen_t) const {}
      void transfer(uintlen_t b, std::vector<opt_ns::iv_ns::iv_t> &st) const {
        if (b == 0) {
          st[0] = {5, 5, false}; // the constant seed
          return;
        }
        // the loop body: x = x + 1 (saturating), nothing known about x1
        st[0].hi = (st[0].hi == INT64_MAX) ? INT64_MAX : st[0].hi + 1;
      }
    };
    // 0 -> 1, 1 -> 1 (the self-loop header is its own latch)
    std::vector<std::vector<uintlen_t>> preds{{}, {0, 1}};
    std::vector<char> widen_at{0, 1};
    auto R = opt_ns::ai_ns::solve_forward<V>(2, 1, preds, widen_at, 0, 4, 8, 3,
                                             CB{});
    UT(R.converged, "ai: the counter loop converges under header widening");
    UT(R.entry[1].lo == 5 && R.entry[1].hi == INT64_MAX,
       "ai: [5,5] widened to [5,MAX] at the header");
  }

  // 18h. the mixed sort key (canon/vn's orderable tag)
  {
    std::vector<char> kc{0, 1, 0};
    std::vector<val_t> kv{0, 7, 0};
    UT(opt_ns::mixed_sort_key(3, kc, kv, none_v) == 0,
       "key: an absent operand is 0");
    UT(opt_ns::mixed_sort_key(3, kc, kv, 1) ==
           ((uint64_t(1) << 63) ^ uint64_t(7)),
       "key: a known const keys by VALUE after the tag bit");
    UT(opt_ns::mixed_sort_key(3, kc, kv, 2) == 2, "key: a var keys by its id");
    UT(opt_ns::mixed_sort_key(3, kc, kv, 1) >
           opt_ns::mixed_sort_key(3, kc, kv, 2),
       "key: consts order after every var");
  }

  // 18i. the region leak scan
  {
    std::vector<uintlen_t> def_site{4, 5, none_v}; // v0@4, v1@5, v2 none
    std::vector<std::vector<uintlen_t>> args{{},  {},     {},  {},
                                             {0}, {0, 1}, {0}, {2}};
    UT(opt_ns::region_uses_escape(def_site, args, 4, 6),
       "leak: a use at 6 of the v0 defined at 4 escapes");
    UT(!opt_ns::region_uses_escape(def_site, args, 4, 7),
       "leak: uses inside [4,7) are fine");
    UT(!opt_ns::region_uses_escape(def_site, args, 0, 4),
       "leak: v2 has no def site -- nothing to leak");
  }

  // 18j. the interprocedural liveness fixpoint
  {
    std::vector<char> eligible{1, 1};
    std::vector<std::vector<char>> alive{{1, 0}, {0, 1}};
    std::vector<std::vector<std::vector<opt_ns::soft_edge_t>>> softs{
        {{}, {{1, 1}}}, {{}, {}}};
    opt_ns::calculate_live_params(eligible, softs, alive);
    UT(alive[0][1], "ipa: g0's param 1 is live -- it feeds g1's LIVE param 1");
    std::vector<std::vector<char>> alive2{{1, 0}, {0, 0}};
    std::vector<std::vector<std::vector<opt_ns::soft_edge_t>>> softs2{
        {{}, {{1, 1}}}, {{}, {}}};
    opt_ns::calculate_live_params(eligible, softs2, alive2);
    UT(!alive2[0][1] && !alive2[1][1],
       "ipa: a dead forwarded-to param keeps the forwarder dead");
    std::vector<char> inelig{1, 0};
    std::vector<std::vector<char>> alive3{{1, 0}, {0, 0}};
    std::vector<std::vector<std::vector<opt_ns::soft_edge_t>>> softs3{
        {{}, {{1, 0}}}, {{}, {}}};
    opt_ns::calculate_live_params(inelig, softs3, alive3);
    UT(alive3[0][1],
       "ipa: forwarding into a NON-candidate is live (it can never die)");
  }

  // 18k. class refinement over serial keys
  {
    std::vector<std::vector<uint64_t>> keys{{1}, {1}, {2}};
    std::vector<char> cand{1, 1, 1};
    std::vector<uintlen_t> rep{0, 1, 2};
    bool ch = opt_ns::refine_by_keys(keys.data(), cand, rep);
    UT(ch && rep[0] == 0 && rep[1] == 0 && rep[2] == 2,
       "equi: equal keys merge into the first candidate's class");
    UT(!opt_ns::refine_by_keys(keys.data(), cand, rep),
       "equi: the verified fixpoint round changes nothing");
  }
}

std::printf("unit tests: %d checks, %d failures\n", ut_checks, ut_fails);
return ut_fails ? 1 : 0;
}
