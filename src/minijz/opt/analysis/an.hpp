#pragma once
// === an.hpp ========================================
// An: the analysis bundle -- dominator tree, loops, SSA construction
// helpers (phi placement + renaming). Uses mjz_lib graphs.
//
// ALGORITHM (what analyze() computes, in one shot per CFG):
//   CSR successor/predecessor graphs -> immediate dominators
//   (Lengauer-Tarjan) -> dominator forest -> dominance intervals
//   -> natural loops per header -> nesting ranks -> dominance
//   frontiers. ssa_construct() then builds SSA from the def sets.
//
// DATA STRUCTURES (An fields and what each answers):
//   adjs/prs     adjacency lists (succ / pred per block)
//   fs/fp        the same as mjz_lib CSR forests (row access)
//   idom[b]      b's immediate (unique) dominator
//   dom_tree     children rows of the dominator tree (b's range())
//   ib/ie        DFS interval of b: d dominates s iff
//                ib[d] <= ib[s] && ie[d] >= ie[s] -- O(1) tests
//   loops        natural-loop MEMBER rows, keyed by header
//   inner[b]     innermost loop containing b (b == inner[b]
//                <=> b is a header); outer/nest = same-loop
//   df           dominance-frontier sets, keyed by block
//
// THEORY (the dominator machinery, and why SSA hangs off it):
//   * DOMINANCE. d dominates s iff every entry->s path passes
//     through d. idom is the unique closest such d; the dominator
//     TREE (edges b->idom[b]) is the CFG's skeleton: every SSA and
//     loop algorithm in this compiler walks it. Lengauer-Tarjan
//     finds idom in near-linear time (mjz_lib's
//     calculate_dominators_from_entry).
//   * DOMINANCE INTERVALS. The DFS numbering pair (ib,ie) makes
//     every dominance query an integer containment test --
//     dominates(d,s) above is O(1) with no tree walk. The natural
//     loop finder uses it to test "h dominates u" per back edge,
//     and the passes use it for reaching-def arguments.
//   * NATURAL LOOPS. A back edge is u->h with h dominating u; the
//     loop is h plus everything that reaches u without passing h
//     (the member row loops.range()[h]). nest ranks nesting depth;
//     the loop passes walk headers NEST-DESCENDING so inner loops
//     transform first (their results then feed the parents).
//   * DOMINANCE FRONTIER. DF(d) = the join blocks first reachable
//     from d without passing through d -- exactly where d's defs
//     stop dominating. DF+ (iterated, transitively closed) is
//     computed per def set by the lib.
//   * PHI PLACEMENT (Cytron et al.). For variable v with def-site
//     set S, block Y needs a phi for v iff Y is in DF+(S) and has
//     >= 2 preds. This is MINIMAL (non-pruned) SSA: a phi arg may
//     be UNDEF (NONE) on an edge where v is not live-in; the
//     language's scoping guarantees it is never read there, so
//     semantics hold and DCE later drops the dead phi.
//   * RENAMING (Cytron's algorithm, phase 8d). A DFS over the
//     dominator tree with a version STACK per source variable: a
//     def pushes its fresh name, a use reads the stack top (the
//     nearest enclosing definition in dominator order), and the
//     epilogue pops exactly what the prologue pushed (the `pushed`
//     counter) -- so sibling subtrees never see each other's
//     names, which is precisely why each use binds to its reaching
//     def. Phi args are seeded WHILE VISITING THE PREDECESSOR
//     (the edge's value is the pred's stack top at its end), and
//     the pred's block id is recorded in t/f so later CFG edits
//     can prune/rekey args by identity, never by position.
//
// PIPELINE POSITION. analyze() runs on the normalized CFG; every
// pass guards on A.n == fn.nblocks() (the fixpoint driver in
// opt/driver.hpp re-analyzes between rounds, so a stale An with a
// different block count is rejected, not trusted).

#include "../../graph_lib.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/stats.hpp"

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
    if (d >= n || s >= n)
      return false;
    if (ib[d] == 0 || ib[s] == 0)
      return false;
    return ib[d] <= ib[s] && ie[d] >= ie[s];
  }
};

constexpr An analyze(const Fn &fn) {
  An A{};
  A.n = fn.nblocks();
  A.adjs.assign(A.n, {});
  A.prs.assign(A.n, {});
  for (uint32_t b = 0; b < A.n; ++b) {
    if (fn.succ0(b) != NONE)
      A.adjs[b].push_back(fn.succ0(b));
    if (fn.succ1(b) != NONE)
      A.adjs[b].push_back(fn.succ1(b));
    for (uint32_t p : fn.preds[b])
      A.prs[b].push_back(p);
  }
  // -- everything below is mjz_lib doing the heavy lifting --
  A.fs = gns::make_basic_forest<V>(A.adjs);     // CSR successor graph
  A.fp = gns::make_basic_inv_forest<V>(A.adjs); // CSR predecessor graph
  A.idom =
      gns::calculate_dominators_from_entry<V>(0, A.fs, A.fp); // Lengauer–Tarjan
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
  for (uint32_t v = 0; v < fn.nparams; ++v)
    defs[v].push_back(0);
  // ---- 8b. phi insertion at the IDF of each multi-def vreg's def set
  std::vector<std::vector<uint32_t>> phis_at(nb);
  for (uint32_t v = 0; v < fn.nvars; ++v) {
    if (defs[v].size() < 2)
      continue;
    std::vector<ulen> idf =
        gns::calculate_iterated_dominance_frontiers_given_dominance_frontiers<
            V>(A.df, defs[v]);
    for (ulen blk : idf) {
      uint32_t b = (uint32_t)blk;
      if (b >= nb || fn.preds[b].size() < 2)
        continue;
      bool dup = false;
      for (uint32_t x : phis_at[b])
        dup |= (x == v);
      if (!dup)
        phis_at[b].push_back(v);
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
        if (s.empty())
          return false; // use before def (should not happen)
        I.a = s.back();
      }
      if (I.b2 != NONE && I.b2 < fn.nvars) {
        auto &s = stacks[I.b2];
        if (s.empty())
          return false;
        I.b2 = s.back();
      }
      if (I.c2 != NONE && I.c2 < fn.nvars) { // Store: the stored value
        auto &s = stacks[I.c2];
        if (s.empty())
          return false;
        I.c2 = s.back();
      }
      for (uint32_t &av : I.args) { // Call arguments
        if (av != NONE && av < fn.nvars) {
          auto &s = stacks[av];
          if (s.empty())
            return false;
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
      if (s == NONE)
        continue;
      for (uint32_t i = fn.blocks[s].begin; i < fn.blocks[s].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k != K::Phi)
          break;
        uint32_t base = (uint32_t)I.c;
        const auto &st_ = stacks[base];
        // find slot of b in preds[s]
        uint32_t slot = NONE;
        for (uint32_t j = 0; j < fn.preds[s].size(); ++j)
          if (fn.preds[s][j] == b)
            slot = j;
        if (slot == NONE)
          return false;
        // Minimal (non-pruned) SSA: a phi arg may be UNDEFINED on an edge
        // (variable not live-in there). Scoping guarantees it is never read
        // on that path, so record the pred with arg NONE; DCE drops the phi.
        uint32_t arg = st_.empty() ? NONE : st_.back();
        // record the pred block id with the arg (t/f unused by Phi) so that
        // later CFG edits can prune phi args by pred identity
        if (slot == 0) {
          I.a = arg;
          I.t = b;
        } else {
          I.b2 = arg;
          I.f = b;
        }
      }
    }
    return true;
  };
  auto visit_epilogue = [&](uint32_t b, uint32_t pushed) {
    // pop exactly `pushed` entries (one per def the prologue pushed; the
    // count equals the block's defining instrs — decrement ONLY on pops)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end && pushed; ++i) {
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
    if (!visit_prologue(0, pushed0))
      return false;
    stack.push_back({0, 0, pushed0});
    auto children_of = [&](uint32_t b) { return A.dom_tree.range()[b]; };
    while (!stack.empty()) {
      Frame &fr = stack.back();
      auto ch = children_of(fr.block);
      if (fr.child_idx < (uint32_t)ch.size()) {
        uint32_t next = (uint32_t)ch[fr.child_idx++];
        uint32_t pushed = 0;
        if (!visit_prologue(next, pushed))
          return false;
        stack.push_back({next, 0, pushed});
      } else {
        visit_epilogue(fr.block, fr.pushed);
        stack.pop_back();
      }
    }
  }
  return true;
}
