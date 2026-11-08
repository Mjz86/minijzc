#pragma once
// === callcse.hpp ========================================
// common-subexpression elimination of calls to provably-pure functions.

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/analysis/an.hpp"
#include "minijz/opt/loop/lcf_rt.hpp" // fn_pure_closure (the purity oracle)

// --- 9e3b. PURE-CALL CSE (interprocedural)
// ------------------------------------- A call to a PURE function
// (fn_pure_closure: its whole call closure is arithmetic + control flow only --
// no Print, no input(), no arena access, no handle value; the same predicate
// the const-call evaluator uses) is a pure expression of (callee, arguments):
// the same inputs always produce the same result, INCLUDING traps and the
// recursion-depth cap, and the calling frame is the same for both calls. So a
// call that a DOMINATING identical call already covers is redundant:
//     t1 = pure_f(a, b); ...; t2 = pure_f(a, b)   ->   t2 = Copy t1
// Trap soundness: the dominating call executes first on every path to the
// dominated one -- if it traps, the second never runs; if it does not,
// the identical computation cannot trap either. Depth soundness: both
// calls sit in the same caller frame, so a Depth-reading closure reads
// the same value. Deleting calls is NOT done here (a pure call may still
// trap, so a value-less call is observable); this pass only dedupes.
// Driven from optimize_prog AFTER the inliner rounds: whatever is left as
// a call is multi-use or recursive, which is exactly where duplicates
// appear (memoization shapes like gcd(a,b) asked twice).
constexpr bool opt_callcse(std::vector<Fn> &fns,
                           std::vector<OptStats> &ostats) {
  const uint32_t n = (uint32_t)fns.size();
  if (n < 2 || ostats.size() < n)
    return false;
  std::vector<uint8_t> pure(n, 0);
  for (uint32_t f = 0; f < n; ++f)
    pure[f] = fn_pure_closure(fns, f) ? 1 : 0;
  bool changed_any = false;
  for (uint32_t f = 0; f < n; ++f) {
    Fn &fn = fns[f];
    if (!fn.nblocks())
      continue;
    const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
    if (!nv)
      continue;
    An A = analyze(fn);
    if (A.n != fn.nblocks() || !A.n)
      continue;
    struct Ent {
      uint32_t callee{};
      std::vector<uint32_t> args{};
      uint32_t val{};
    };
    std::vector<Ent> table;
    auto rows = A.dom_tree.range();
    struct Fr {
      uint32_t b, ci, mark;
    };
    std::vector<Fr> st;
    st.push_back({0, 0, (uint32_t)table.size()});
    while (!st.empty()) {
      uint32_t b = st.back().b;
      uint32_t ci = st.back().ci;
      uint32_t mark = st.back().mark;
      if (ci == 0) {
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          Instr &I = fn.code[i];
          if (I.k != K::Call)
            continue;
          uint32_t g = (uint32_t)I.c;
          if (g >= n || !pure[g] || I.args.size() != fns[g].nparams)
            continue;
          bool hit = false;
          for (const Ent &e : table) { // tables stay tiny
            if (e.callee != g || e.args.size() != I.args.size())
              continue;
            bool same = true;
            for (size_t k = 0; k < e.args.size(); ++k)
              if (e.args[k] != I.args[k]) {
                same = false;
                break;
              }
            if (!same)
              continue;
            if (e.val != I.dst) { // SSA: always distinct vregs
              I.k = K::Copy;
              I.a = e.val;
              I.c2 = NONE;
              I.args.clear();
              I.nt = false;
              ostats[f].callcses++;
              changed_any = true;
            }
            hit = true;
            break;
          }
          if (!hit)
            table.push_back({g, I.args, I.dst});
        }
      }
      auto ch = rows[b];
      if (ci < (uint32_t)ch.size()) {
        st.back().ci = ci + 1;
        uint32_t nb = (uint32_t)ch[ci];
        st.push_back({nb, 0, (uint32_t)table.size()});
        continue;
      }
      table.resize(mark); // leaving b: its entries go out of scope
      st.pop_back();
    }
  }
  return changed_any;
}
