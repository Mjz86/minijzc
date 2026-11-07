#pragma once

struct GenCtx {
  struct Arr {
    std::string name;
    int len;
    int inner{0}; // >0: 2-D (len x inner; slots hold inner handles)
  };
  struct FnG { // a generated function available for calls
    std::string name;
    int nparams;
    int arr_param{-1}; // >= 0: that arg position takes an array handle
    bool rec{false}; // recursive: call sites must keep tiny constants
    bool rt{false}; // recursive BUT trip-guard bounded: runtime args safe
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
  bool pure{false};          // inside a side-effect-free expression?
  bool allow_mat{false};     // this program may grow 2-D arrays at all
  bool bait{false};          // deliberate trap bait allowed in this program?
  bool allow_rti{false};     // runtime-index array accesses allowed?
  bool has_div{}, has_mat{}, has_aparam{}; // vocabulary report flags
  explicit GenCtx(uint32_t seed) : rng(seed) {}
  int pick(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
  }
  void gen_expr(std::string &out, int depth);
  void gen_pure(std::string &out, int depth);
  void gen_stmts(int count, int depth);
  void gen_stmt(int depth);
  void gen_fn_defs();
};

static const char *VAR_NAMES[] = {"v", "w", "u", "z", "q", "r", "p", "m"};

// leaves: variable / in-bounds flat-array load (constant, counter-mod or
// double-mod variable index) / len(a) / input() / constant / fn call /
// 2-D load / len(v) / division guarded by an && or || short-circuit.
// `pure` expressions read no stdin, call nothing, and never trap: they are
// safe as fn-call arguments and inside compound loop conditions.
void GenCtx::gen_pure(std::string &out, int depth) {
  bool sv = pure;
  pure = true;
  gen_expr(out, depth);
  pure = sv;
}

void GenCtx::gen_expr(std::string &out, int depth) {
  if (depth <= 0 || pick(0, 5) == 0) {
    int leaf = pick(0, 9);
    if (leaf <= 1 && !vars.empty()) {
      out += vars[pick(0, (int)vars.size() - 1)];
      return;
    }
    if (leaf == 2) { // flat array load: three index flavors
      std::vector<int> flat{};
      for (int i = 0; i < (int)arrays.size(); ++i)
        if (arrays[i].inner == 0) flat.push_back(i);
      if (!flat.empty()) {
        const Arr &A = arrays[flat[pick(0, (int)flat.size() - 1)]];
        int flavor = pick(0, 5);
        if (allow_rti && flavor == 1) { // runtime index: counters >= 0
          std::vector<int> cs{};
          for (int k = 0; k < (int)vars.size(); ++k)
            if (vars[k][0] == 'c') cs.push_back(k);
          if (!cs.empty()) {
            out += A.name + "[" + vars[cs[pick(0, (int)cs.size() - 1)]] +
                   " % " + std::to_string(A.len) + "]";
            return;
          }
        } else if (allow_rti && flavor == 2 && !vars.empty()) { // any var
          const std::string &v = vars[pick(0, (int)vars.size() - 1)];
          std::string L = std::to_string(A.len);
          out += A.name + "[(" + v + " % " + L + " + " + L + ") % " + L + "]";
          return;
        }
        int i = pick(0, A.len - 1); // else constant index (bait: OOB)
        if (bait && !pure && pick(0, 11) == 0) i = A.len + pick(0, 2);
        out += A.name + "[" + std::to_string(i) + "]";
        return;
      }
    }
    if (leaf == 3 && !arrays.empty()) { // len(): outer, or inner via m[0]
      const Arr &A = arrays[pick(0, (int)arrays.size() - 1)];
      if (A.inner > 0 && pick(0, 1))
        out += "len(" + A.name + "[0])";
      else
        out += "len(" + A.name + ")";
      return;
    }
    if (leaf == 4 && use_input && !pure) {
      out += "input()";
      return;
    }
    if ((leaf == 5 || leaf == 9) && !fns.empty() && !pure) { // fn call
      std::vector<int> flat{};
      for (int i = 0; i < (int)arrays.size(); ++i)
        if (arrays[i].inner == 0) flat.push_back(i);
      std::vector<int> call{};
      for (int i = 0; i < (int)fns.size(); ++i)
        if (fns[i].arr_param < 0 || !flat.empty()) call.push_back(i);
      if (!call.empty()) {
        const FnG &F = fns[call[pick(0, (int)call.size() - 1)]];
        // recursive fns take runtime arguments only when every internal
        // edge is a tail call (scctco's trip guard bounds the loop); plain
        // non-tail recursion with runtime args would routinely hit the
        // 512-frame trap and kill the rest of the program
        bool expr_args = F.rec ? F.rt : true;
        out += F.name + "(";
        for (int k = 0; k < F.nparams; ++k) {
          if (k) out += ", ";
          if (F.arr_param >= 0 && k == F.arr_param) { // the array handle
            const Arr &A = arrays[flat[pick(0, (int)flat.size() - 1)]];
            out += A.name;
          } else if (F.arr_param >= 0 && k == F.arr_param + 1) {
            const Arr &A = arrays[flat[pick(0, (int)flat.size() - 1)]];
            int i = pick(0, A.len - 1); // its index (bait: OOB)
            if (bait && pick(0, 11) == 0) i = A.len + pick(0, 2);
            out += std::to_string(i);
          } else if (expr_args && pick(0, 4) == 0) { // pure expression arg
            std::string e;
            gen_pure(e, 2);
            out += e;
          } else {
            out += std::to_string(pick(-3, 8));
          }
        }
        out += ")";
        return;
      }
    }
    if (leaf == 6) { // 2-D load (constant in-bounds indices)
      std::vector<int> mats{};
      for (int i = 0; i < (int)arrays.size(); ++i)
        if (arrays[i].inner > 0) mats.push_back(i);
      if (!mats.empty()) {
        const Arr &A = arrays[mats[pick(0, (int)mats.size() - 1)]];
        out += A.name + "[" + std::to_string(pick(0, A.len - 1)) + "][" +
               std::to_string(pick(0, A.inner - 1)) + "]";
        return;
      }
    }
    if (leaf == 7 && !vars.empty() && pick(0, 2) == 0) { // len of a scalar
      out += "len(" + vars[pick(0, (int)vars.size() - 1)] + ")"; // -> 0
      return;
    }
    if (leaf == 8 && !vars.empty()) { // division guarded by short-circuit
      const std::string &v = vars[pick(0, (int)vars.size() - 1)];
      has_div = true;
      std::string K = std::to_string(pick(2, 99));
      const char *cmp = pick(0, 1) ? " > " : " < ";
      std::string rhs = std::to_string(pick(-3, 3));
      bool trap = bait && !pure && pick(0, 2) == 0; // 1/3: guard inverted
      if (pick(0, 1))
        out += "(" + v + (trap ? " == 0 && " : " != 0 && ") + K + " / " +
               v + cmp + rhs + ")";
      else
        out += "(" + v + (trap ? " != 0 || " : " == 0 || ") + K + " / " +
               v + cmp + rhs + ")";
      return;
    }
    int r = pick(0, 3); // constants: widen the range sometimes
    if (r == 0) out += std::to_string(pick(-99, 99));
    else if (r == 1) out += std::to_string(pick(-99999, 99999));
    else out += std::to_string(pick(-9, 9));
    return;
  }
  switch (pick(0, 11)) {
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
           pick(0, 3) == 0 ? " == " : // canon: >= -> <=, != -> !x == y
           pick(0, 2) == 0 ? " >= " :
           pick(0, 1) ? " <= " : " != ";
           gen_expr(out, depth - 1); out += ")"; break;
  case 5: out += pick(0, 1) ? "-(" : "!("; // negation or logical not
           gen_expr(out, depth - 1); out += ")"; break;
  case 6: out += "("; gen_expr(out, depth - 1); out += " && ";
          gen_expr(out, depth - 1); out += ")"; break;
  case 7: out += "("; gen_expr(out, depth - 1); out += " || ";
          gen_expr(out, depth - 1); out += ")"; break;
  case 8: { // divide by a constant (bait: deliberate /0 trap)
    int d = pick(2, 12);
    if (pick(0, 1)) d = -d; // negative divisors: signed divmagic bait
    if (bait && !pure && pick(0, 11) == 0) d = 0;
    has_div = true;
    out += "("; gen_expr(out, depth - 1);
    out += " / " + std::to_string(d) + ")";
    break;
  }
  case 9: { // modulo by a constant (ditto)
    int d = pick(2, 12);
    if (pick(0, 1)) d = -d;
    if (bait && !pure && pick(0, 11) == 0) d = 0;
    has_div = true;
    out += "("; gen_expr(out, depth - 1);
    out += " % " + std::to_string(d) + ")";
    break;
  }
  case 10: out += "("; gen_expr(out, depth - 1); out +=
            pick(0, 1) ? " + " : " - ";
            gen_expr(out, depth - 1); out += ")"; break;
  case 11: out += "("; gen_expr(out, depth - 1); out +=
            pick(0, 1) ? " < " : " > ";
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
  // leaves: print / assignment / flat array store / 2-D store
  int r = pick(0, depth <= 0 ? 4 : 10);
  if (r <= (depth <= 0 ? 4 : 3)) {
    int leaf = pick(0, 5);
    if (leaf <= 1) { // flat array store: constant / runtime index
      std::vector<int> flat{};
      for (int i = 0; i < (int)arrays.size(); ++i)
        if (arrays[i].inner == 0) flat.push_back(i);
      if (!flat.empty()) {
        const Arr &A = arrays[flat[pick(0, (int)flat.size() - 1)]];
        std::string idx{};
        int flavor = pick(0, 5);
        if (allow_rti && flavor == 1) { // runtime index: counters >= 0
          std::vector<int> cs{};
          for (int k = 0; k < (int)vars.size(); ++k)
            if (vars[k][0] == 'c') cs.push_back(k);
          if (!cs.empty())
            idx = vars[cs[pick(0, (int)cs.size() - 1)]] + " % " +
                  std::to_string(A.len);
        } else if (allow_rti && flavor == 2 && !vars.empty()) { // any var
          const std::string &v = vars[pick(0, (int)vars.size() - 1)];
          std::string L = std::to_string(A.len);
          idx = "(" + v + " % " + L + " + " + L + ") % " + L;
        }
        int i = pick(0, A.len - 1); // else constant index
        if (idx.empty() && bait && pick(0, 9) == 0)
          i = A.len + pick(0, 2); // trap-parity bait
        if (idx.empty()) idx = std::to_string(i);
        std::string e;
        gen_expr(e, 3);
        src += A.name + "[" + idx + "] = " + e + ";\n";
        return;
      }
    }
    if (leaf == 2) { // 2-D store (constant in-bounds indices)
      std::vector<int> mats{};
      for (int i = 0; i < (int)arrays.size(); ++i)
        if (arrays[i].inner > 0) mats.push_back(i);
      if (!mats.empty()) {
        const Arr &A = arrays[mats[pick(0, (int)mats.size() - 1)]];
        std::string e;
        gen_expr(e, 3);
        src += A.name + "[" + std::to_string(pick(0, A.len - 1)) + "][" +
               std::to_string(pick(0, A.inner - 1)) + "] = " + e + ";\n";
        return;
      }
    }
    if (leaf == 3 && !vars.empty()) { // assignment (never loop counters)
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
    std::string e; // print (occasionally a raw handle value: stkarr bait)
    if (!arrays.empty() && pick(0, 11) == 0) {
      const Arr &A = arrays[pick(0, (int)arrays.size() - 1)];
      src += "print " + A.name + ";\n";
      return;
    }
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
    if (pick(0, 2) == 0) { // one third: no else arm
      src += "}\n";
      return;
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
    if (pick(0, 2) == 0) {
      // count UP to a bound; the increment comes first, so a guarded
      // 'continue' cannot spin (same argument as the countdown below)
      src += "let " + cn + " = 0;\n";
      src += "while (" + cn + " < " + std::to_string(pick(1, 5)) + ") {\n";
      src += "  " + cn + " = " + cn + " + 1;\n";
    } else {
      src += "let " + cn + " = " + std::to_string(pick(1, 5)) + ";\n";
      std::string cond = cn + " > 0";
      if (pick(0, 4) < 2) { // compound condition: still terminates, the
        // counter decrements FIRST every iteration; the pure rider also
        // exercises short-circuit evaluation of the loop condition itself
        std::string x;
        gen_pure(x, 2);
        cond += " && " + x;
      }
      src += "while (" + cond + ") {\n";
      src += "  " + cn + " = " + cn + " - 1;\n";
    }
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
  { // let: 1/5 flat array, else scalar (2-D only when allow_mat: a
    // single handle-chain load kills program-wide lifting, so whole
    // programs opt in to 2-D coverage instead of every single let)
    int r2 = pick(0, 4);
    if (r2 == 0 && allow_mat) { // 2-D: outer slots filled with
      // equal-length inner arrays
      std::string n = "m" + std::to_string(counter_id++);
      int ol = pick(2, 3), il = pick(1, 4);
      src += "let " + n + " = array(" + std::to_string(ol) + ");\n";
      for (int k = 0; k < ol; ++k)
        src += n + "[" + std::to_string(k) + "] = array(" +
               std::to_string(il) + ");\n";
      arrays.push_back({n, ol, il});
      has_mat = true;
      return;
    }
    if (r2 <= 1) {
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

// generate 0-3 plain functions (params, body stmts, 1-2 returns) plus,
// with fixed shapes, a self-recursive and/or a mutually-recursive pair, a
// tail-recursive accumulator, a 3-member SCC and array-parameter helpers.
// Bodies run with a FRESH scope (params only); every call site uses small
// constant arguments so recursion depth stays far below the 512-frame trap.
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
    fns.push_back({name, np, -1, false});
  }
  if (pick(0, 3)) { // self recursion (never inlined: SCC self-loop)
    src += "fn rec(n) { if (n <= 0) { return 0; } "
           "return rec(n - 1) + n; }\n";
    fns.push_back({"rec", 1, -1, true});
  }
  if (pick(0, 4)) { // mutual recursion (a 2-member SCC, all-tail)
    src += "fn pe(n) { if (n <= 0) { return 1; } return po(n - 1); }\n";
    src += "fn po(n) { if (n <= 0) { return 0; } return pe(n - 1); }\n";
    fns.push_back({"pe", 1, -1, true, true});
    fns.push_back({"po", 1, -1, true, true});
  }
  if (pick(0, 1)) { // tail recursion with an accumulator: every internal
    // edge is a tail call, so scctco must collapse this into a plain loop
    src += "fn tsum(n, acc) { if (n <= 0) { return acc; } "
           "return tsum(n - 1, acc + n); }\n";
    fns.push_back({"tsum", 2, -1, true, true});
  }
  if (pick(0, 2)) { // gcd-style tail call whose FIRST argument is a raw
    // parameter: the collapsed loop must rotate it through the header phi
    // (feeding the original argument back forever is the classic bug)
    src += "fn rgcd(a, b) { if (b == 0) { return a; } "
           "return rgcd(b, a % b); }\n";
    fns.push_back({"rgcd", 2, -1, true, true});
  }
  if (pick(0, 3) == 0) { // flat argument swap across a tail call: BOTH
    // args are raw params, so both rotation phis are exercised
    src += "fn sw(a, b, n) { if (n <= 0) { return a; } "
           "return sw(b, a, n - 1); }\n";
    fns.push_back({"sw", 3, -1, true, true});
  }
  if (pick(0, 3) == 0) { // a 3-member SCC of tail calls
    src += "fn m3a(n) { if (n == 0) { return 7; } return m3b(n - 1); }\n";
    src += "fn m3b(n) { if (n == 0) { return 8; } return m3c(n - 1); }\n";
    src += "fn m3c(n) { if (n == 0) { return 9; } return m3a(n - 1); }\n";
    fns.push_back({"m3a", 1, -1, true});
    fns.push_back({"m3b", 1, -1, true});
    fns.push_back({"m3c", 1, -1, true});
  }
  if (pick(0, 4) == 0) { // array handles cross frames: aliasing bait for
    // the handle gates (Call args must block stack arrays, Load/Store of a
    // param must block lifting -- both are program-wide gates, so this
    // stays RARE to keep mem2reg coverage high elsewhere)
    src += "fn aget(a, i) { return a[i]; }\n";
    src += "fn aset(a, i, v) { a[i] = v; return v; }\n";
    fns.push_back({"aget", 2, 0, false});
    fns.push_back({"aset", 3, 0, false});
    has_aparam = true;
  }
}

static GenCtx gen_program(uint32_t seed) {
  GenCtx g(seed);
  g.use_input = (seed & 1) != 0; // half the programs read stdin
  g.allow_mat = ((seed >> 2) & 7) == 0; // 1/8 of programs grow 2-D arrays
  g.bait = ((seed >> 5) & 3) == 0; // 1/4 carry deliberate trap bait; the
  // rest run to completion so the deep optimizer paths stay covered
  g.allow_rti = ((seed >> 8) & 3) == 0; // 1/4 use runtime array indices; the
  // rest keep every index constant, so lifting / dead-array / const-call
  // coverage stays as strong as the narrower original vocabulary
  for (int i = 0; i < 16; ++i) g.ins.push_back(g.pick(-40, 40));
  g.gen_fn_defs(); // functions first (hoisting makes order irrelevant)
  g.gen_stmts(g.pick(2, 4), 4); // leading lets mostly
  g.gen_stmts(g.pick(4, 12), 4);
  return g;
}

static void run_fuzz(long n, const std::string &cc_in, long fuzz_native,
                     const std::string &out_dir, long from = 1) {
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
  for (long seed = from; seed < from + n; ++seed) {
    GenCtx g = gen_program((uint32_t)seed);
    const std::string &src = g.src;
    if (std::getenv("MJZ_FUZZ_DUMP") && seed >= from + n - 2)
      std::fprintf(stderr, "=== SEED %ld ===\n%s\n", seed, src.c_str());
    // rotate the pass configuration: every run exercises a different slice
    // of the pipeline (defaults are level 3 + register allocation)
    PassCfg cfg{};
    cfg.set_level(3);
    switch (seed % 8) {
    case 0: cfg.p_scctco = false; break;   // no SCC tail-call collapse
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
    if (g.has_div) F.div_progs++;
    if (g.has_mat) F.mat_progs++;
    if (g.has_aparam) F.aparam_progs++;
    {
      OptStats os = sum_ostats(P);
      F.inlined_total += os.inlines;
      F.unrolled_total += os.unrolls;
      F.lifted += os.mem2reg_lifts;
      F.dead_stores += os.mem2reg_dses;
      F.constcalls += os.constcalls;
      F.deadallocs += os.deadallocs;
      F.gensims += os.lcf_gensims;
      F.traps_killed += os.traps_killed;
      F.scctco_total += os.scctco;
      F.scctco_fold_total += os.scctco_folds;
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
              "const-called: %ld | dead allocs: %ld | "
              "general-sim folds: %ld | traps proven impossible: %ld | "
              "SCC-TCO collapses: %ld (folds: %ld) | "
              "native-checked (C+asm x -O0/-O2): %ld\n"
              "  vocabulary: w/ div/mod: %ld | w/ 2-D arrays: %ld | "
              "w/ array params: %ld\n",
              F.cases, mismatches, F.timeouts, F.traps, F.phis, F.dce_instrs,
              F.licm_hoists, F.arr_progs, F.in_progs, F.fn_progs, F.brk_progs,
              F.inlined_total, F.unrolled_total, F.lifted, F.dead_stores,
              F.constcalls, F.deadallocs,
              F.gensims, F.traps_killed, F.scctco_total, F.scctco_fold_total,
              F.native_checked, F.div_progs, F.mat_progs, F.aparam_progs);
  CHECK(mismatches == 0, "fuzz must be clean");
}

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
// constexpr. The static_asserts are enabled with -DMJZ_CX_ASSERT (needs
// generous -fconstexpr-depth / -fconstexpr-ops-limit; SEPARATE constant
// expressions because g++ retains each one's evaluation state — three fit
// in 4 GiB); --selftest always runs the FULL battery (all four programs)
// at runtime.
#ifdef MJZ_CX_ASSERT
static_assert(cx_arrays(),
              "MiniJZ pipeline (arena arrays + lib loop analyses) at compile time");
static_assert(cx_fns(),
              "MiniJZ pipeline (recursion/inline/const-call) at compile time");
static_assert(cx_series(),
              "MiniJZ pipeline (unroll/closed forms/divmagic/VM) at compile time");
#endif
