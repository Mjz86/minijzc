#pragma once
// === fuzz_gen.hpp ========================================
// the differential fuzzer's program generator (GenCtx, gen_program).

#include "minijz/prog/program.hpp"
#include "minijz/prog/runners.hpp"

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
    bool rec{false};   // recursive: call sites must keep tiny constants
    bool rt{false};    // recursive BUT trip-guard bounded: runtime args safe
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
inline void GenCtx::gen_pure(std::string &out, int depth) {
  bool sv = pure;
  pure = true;
  gen_expr(out, depth);
  pure = sv;
}

inline void GenCtx::gen_expr(std::string &out, int depth) {
  if (depth <= 0 || pick(0, 5) == 0) {
    int leaf = pick(0, 9);
    if (leaf <= 1 && !vars.empty()) {
      out += vars[pick(0, (int)vars.size() - 1)];
      return;
    }
    if (leaf == 2) { // flat array load: three index flavors
      std::vector<int> flat{};
      for (int i = 0; i < (int)arrays.size(); ++i)
        if (arrays[i].inner == 0)
          flat.push_back(i);
      if (!flat.empty()) {
        const Arr &A = arrays[flat[pick(0, (int)flat.size() - 1)]];
        int flavor = pick(0, 5);
        if (allow_rti && flavor == 1) { // runtime index: counters >= 0
          std::vector<int> cs{};
          for (int k = 0; k < (int)vars.size(); ++k)
            if (vars[k][0] == 'c')
              cs.push_back(k);
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
        if (bait && !pure && pick(0, 11) == 0)
          i = A.len + pick(0, 2);
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
        if (arrays[i].inner == 0)
          flat.push_back(i);
      std::vector<int> call{};
      for (int i = 0; i < (int)fns.size(); ++i)
        if (fns[i].arr_param < 0 || !flat.empty())
          call.push_back(i);
      if (!call.empty()) {
        const FnG &F = fns[call[pick(0, (int)call.size() - 1)]];
        // recursive fns take runtime arguments only when every internal
        // edge is a tail call (scctco's trip guard bounds the loop); plain
        // non-tail recursion with runtime args would routinely hit the
        // 512-frame trap and kill the rest of the program
        bool expr_args = F.rec ? F.rt : true;
        out += F.name + "(";
        for (int k = 0; k < F.nparams; ++k) {
          if (k)
            out += ", ";
          if (F.arr_param >= 0 && k == F.arr_param) { // the array handle
            const Arr &A = arrays[flat[pick(0, (int)flat.size() - 1)]];
            out += A.name;
          } else if (F.arr_param >= 0 && k == F.arr_param + 1) {
            const Arr &A = arrays[flat[pick(0, (int)flat.size() - 1)]];
            int i = pick(0, A.len - 1); // its index (bait: OOB)
            if (bait && pick(0, 11) == 0)
              i = A.len + pick(0, 2);
            out += std::to_string(i);
          } else if (expr_args && pick(0, 4) == 0) { // pure expression arg
            std::string e;
            gen_pure(e, 2);
            out += e;
          } else if (use_input && pick(0, 3) == 0) {
            // occasionally a RUNTIME argument: exercises tailrec -> loop ->
            // cdfold with a live budget guard against the oracle's real
            // recursion (and its depth cap)
            out += "input()";
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
        if (arrays[i].inner > 0)
          mats.push_back(i);
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
        out += "(" + v + (trap ? " == 0 && " : " != 0 && ") + K + " / " + v +
               cmp + rhs + ")";
      else
        out += "(" + v + (trap ? " != 0 || " : " == 0 || ") + K + " / " + v +
               cmp + rhs + ")";
      return;
    }
    int r = pick(0, 3); // constants: widen the range sometimes
    if (r == 0)
      out += std::to_string(pick(-99, 99));
    else if (r == 1)
      out += std::to_string(pick(-99999, 99999));
    else
      out += std::to_string(pick(-9, 9));
    return;
  }
  switch (pick(0, 11)) {
  case 0:
    out += "(";
    gen_expr(out, depth - 1);
    out += " + ";
    gen_expr(out, depth - 1);
    out += ")";
    break;
  case 1:
    out += "(";
    gen_expr(out, depth - 1);
    out += " - ";
    gen_expr(out, depth - 1);
    out += ")";
    break;
  case 2:
    out += "(";
    gen_expr(out, depth - 1);
    out += " * ";
    gen_expr(out, depth - 1);
    out += ")";
    break;
  case 3:
    out += "(";
    gen_expr(out, depth - 1);
    out += pick(0, 1) ? " < " : " > "; // both directions: canon bait
    gen_expr(out, depth - 1);
    out += ")";
    break;
  case 4:
    out += "(";
    gen_expr(out, depth - 1);
    out += pick(0, 3) == 0 ? " == " : // canon: >= -> <=, != -> !x == y
               pick(0, 2) == 0 ? " >= "
           : pick(0, 1)        ? " <= "
                               : " != ";
    gen_expr(out, depth - 1);
    out += ")";
    break;
  case 5:
    out += pick(0, 1) ? "-(" : "!("; // negation or logical not
    gen_expr(out, depth - 1);
    out += ")";
    break;
  case 6:
    out += "(";
    gen_expr(out, depth - 1);
    out += " && ";
    gen_expr(out, depth - 1);
    out += ")";
    break;
  case 7:
    out += "(";
    gen_expr(out, depth - 1);
    out += " || ";
    gen_expr(out, depth - 1);
    out += ")";
    break;
  case 8: { // divide by a constant (bait: deliberate /0 trap)
    int d = pick(2, 12);
    if (pick(0, 1))
      d = -d; // negative divisors: signed divmagic bait
    if (bait && !pure && pick(0, 11) == 0)
      d = 0;
    has_div = true;
    out += "(";
    gen_expr(out, depth - 1);
    out += " / " + std::to_string(d) + ")";
    break;
  }
  case 9: { // modulo by a constant (ditto)
    int d = pick(2, 12);
    if (pick(0, 1))
      d = -d;
    if (bait && !pure && pick(0, 11) == 0)
      d = 0;
    has_div = true;
    out += "(";
    gen_expr(out, depth - 1);
    out += " % " + std::to_string(d) + ")";
    break;
  }
  case 10:
    out += "(";
    gen_expr(out, depth - 1);
    out += pick(0, 1) ? " + " : " - ";
    gen_expr(out, depth - 1);
    out += ")";
    break;
  case 11:
    out += "(";
    gen_expr(out, depth - 1);
    out += pick(0, 1) ? " < " : " > ";
    gen_expr(out, depth - 1);
    out += ")";
    break;
  }
}

// `depth` is a hard budget for compound-statement nesting: ifs and whiles
// consume one unit for their bodies, so the program tree is finite and small
// (if-arms pick(1,2) stmts: branching factor stays subcritical).
inline void GenCtx::gen_stmt(int depth) {
  // break/continue bait: inside loops only, always guarded by an `if` so
  // the statements after it stay reachable (the generated loops decrement
  // their counter FIRST, so a guarded `continue` cannot spin forever)
  if (loop_depth > 0 && pick(0, 11) == 0) {
    std::string c;
    gen_expr(c, 2);
    src += "if (" + c + ") { " + (pick(0, 1) ? "break" : "continue") + "; }\n";
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
        if (arrays[i].inner == 0)
          flat.push_back(i);
      if (!flat.empty()) {
        const Arr &A = arrays[flat[pick(0, (int)flat.size() - 1)]];
        std::string idx{};
        int flavor = pick(0, 5);
        if (allow_rti && flavor == 1) { // runtime index: counters >= 0
          std::vector<int> cs{};
          for (int k = 0; k < (int)vars.size(); ++k)
            if (vars[k][0] == 'c')
              cs.push_back(k);
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
        if (idx.empty())
          idx = std::to_string(i);
        std::string e;
        gen_expr(e, 3);
        src += A.name + "[" + idx + "] = " + e + ";\n";
        return;
      }
    }
    if (leaf == 2) { // 2-D store (constant in-bounds indices)
      std::vector<int> mats{};
      for (int i = 0; i < (int)arrays.size(); ++i)
        if (arrays[i].inner > 0)
          mats.push_back(i);
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
        if (vars[i][0] != 'c')
          idx.push_back(i);
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
        src += n + "[" + std::to_string(k) + "] = array(" + std::to_string(il) +
               ");\n";
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
    std::string n =
        std::string(VAR_NAMES[pick(0, 7)]) + std::to_string(counter_id++);
    std::string e;
    if (vars.empty())
      e = std::to_string(pick(-9, 9));
    else
      gen_expr(e, 3);
    src += "let " + n + " = " + e + ";\n";
    vars.push_back(n);
  }
}
inline void GenCtx::gen_stmts(int count, int depth) {
  for (int i = 0; i < count; ++i)
    gen_stmt(depth);
}

// generate 0-3 plain functions (params, body stmts, 1-2 returns) plus,
// with fixed shapes, a self-recursive and/or a mutually-recursive pair, a
// tail-recursive accumulator, a 3-member SCC and array-parameter helpers.
// Bodies run with a FRESH scope (params only); every call site uses small
// constant arguments so recursion depth stays far below the 512-frame trap.
inline void GenCtx::gen_fn_defs() {
  int n = pick(0, 2);
  if (pick(0, 1))
    ++n;
  for (int f = 0; f < n; ++f) {
    int np = pick(0, 3);
    std::string name = "h" + std::to_string(counter_id++);
    src += "fn " + name + "(";
    std::vector<std::string> saved_vars = vars;
    std::vector<Arr> saved_arrs = arrays;
    vars.clear();
    arrays.clear();
    for (int p = 0; p < np; ++p) {
      if (p)
        src += ", ";
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
  g.use_input = (seed & 1) != 0;        // half the programs read stdin
  g.allow_mat = ((seed >> 2) & 7) == 0; // 1/8 of programs grow 2-D arrays
  g.bait = ((seed >> 5) & 3) == 0;      // 1/4 carry deliberate trap bait; the
  // rest run to completion so the deep optimizer paths stay covered
  g.allow_rti = ((seed >> 8) & 3) == 0; // 1/4 use runtime array indices; the
  // rest keep every index constant, so lifting / dead-array / const-call
  // coverage stays as strong as the narrower original vocabulary
  for (int i = 0; i < 16; ++i)
    g.ins.push_back(g.pick(-40, 40));
  g.gen_fn_defs(); // functions first (hoisting makes order irrelevant)
  g.gen_stmts(g.pick(2, 4), 4); // leading lets mostly
  g.gen_stmts(g.pick(4, 12), 4);
  return g;
}
