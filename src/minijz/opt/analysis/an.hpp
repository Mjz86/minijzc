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
#include "../../ir/ssa_generic.hpp"
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
// 8. SSA construction -- a thin adapter over the generic kernel
//    (ir/ssa_generic.hpp, mjz::ssa_ns): the parts that KNOW the IR stay
//    here (which Instr fields are uses/defs, how a phi record becomes a
//    2-slot Phi instr), the graph math lives in the kernel:
//    - 8b placement: ssa_ns::calculate_phi_placements (Cytron IDF,
//      ascending-var rows -- the loop order the old code hand-rolled)
//    - 8c rebuild: ssa_ns::calculate_reflow_plan (head inserts only)
//    - 8d renaming: ssa_ns::rename_ssa (the dominator-tree walk as a
//      pure table: reaching_def / def_ordinal / N-ary phi records),
//      then a REPLAY in visit_order that mints fresh vreg ids in exactly
//      the order the old interleaved walk did -- the rewiring is
//      byte-identical by construction (146-run parity, fuzz windows)
// ============================================================================
constexpr bool ssa_construct(Fn &fn, const An &A, SsaStats &st) {
  st = {};
  const uint32_t nb = fn.nblocks();
  // ---- 8a. def blocks per original vreg (the IR-side half of the
  // event model: this loop is the only place that knows dst = def)
  std::vector<std::vector<ulen>> defs(fn.nvars);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst != NONE && fn.code[i].k != K::Phi)
        defs[fn.code[i].dst].push_back(b);
  // parameters are "defined at entry" (block 0): uses before any assignment
  // see the incoming argument; re-assignments get ordinary phi placement
  for (uint32_t v = 0; v < fn.nparams; ++v)
    defs[v].push_back(0);
  // ---- 8b. phi placement at the IDF of each multi-def vreg's def set.
  // The pred rows must be POSITIONAL (fn.preds' ascending order): a phi
  // record's entry index is its edge identity, so the rows the kernel
  // sees have to be the same rows the lowering indexes into.
  const auto pred_rows = gns::make_basic_forest<V>(A.prs);
  const gns::treversal_result_t<V> placement =
      ssa_ns::calculate_phi_placements<V>(A.df, pred_rows, defs);
  auto placement_rows = placement.range();
  // ---- 8c. rebuild the code vector with phis at block starts
  //      (the generic reflow plan: head counts = the placement rows)
  std::vector<uint32_t> phi_ord_base(nb + 1, 0); // row-start ordinals
  for (uint32_t b = 0; b < nb; ++b)
    phi_ord_base[b + 1] = phi_ord_base[b] + (uint32_t)placement_rows[b].size();
  const uint32_t total_phis = phi_ord_base[nb];
  st.phis = total_phis;
  {
    const uint32_t n0 = (uint32_t)fn.code.size();
    std::vector<ulen> ob(nb), oe(nb), keep(n0, 1), head(nb, 0), after(n0, 0);
    for (uint32_t b = 0; b < nb; ++b) {
      ob[b] = fn.blocks[b].begin;
      oe[b] = fn.blocks[b].end;
      head[b] = (uint32_t)placement_rows[b].size();
    }
    const ssa_ns::reflow_plan_t<V> plan =
        ssa_ns::calculate_reflow_plan<V>(ob, oe, keep, head, after);
    std::vector<Instr> nc{};
    nc.reserve(plan.total + 16);
    for (uint32_t b = 0; b < nb; ++b) {
      for (ulen v : placement_rows[b])
        nc.push_back({.k = K::Phi, .a = NONE, .b2 = NONE, .c = (int64_t)v});
      for (uint32_t i = ob[b]; i < oe[b]; ++i)
        nc.push_back(fn.code[i]);
    }
    for (uint32_t b = 0; b < nb; ++b)
      fn.blocks[b] = {(uint32_t)plan.new_begin[b], (uint32_t)plan.new_end[b]};
    fn.code = std::move(nc);
  }
  // ---- 8d. renaming: gather events, run the generic walk, replay
  // The event stream per block, in program order: params first (block 0
  // head, pre-materialized sites [0, nparams) -- version 0 of a param is
  // the param itself, no fresh mint), then per instruction the uses
  // (a, b2, c2, args...) followed by the dst def; phi placeholders emit
  // nothing (the kernel synthesizes their defs from the placement).
  // Site ids are DENSE in this enumeration order; the gather, the
  // replay and the apply re-derive them from the same code by the same
  // rule (use/def events only advance the counter when emitted).
  const uint32_t nparams = fn.nparams;
  std::vector<std::vector<ssa_ns::ssa_event_t>> evs(nb);
  std::vector<uint32_t> site_begin(nb, 0); // first site of block b's events
  uint32_t n_sites = nparams;
  {
    uint32_t site = nparams;
    for (uint32_t b = 0; b < nb; ++b) {
      if (b == 0) // the entry defs: params, sites [0, nparams)
        for (uint32_t v = 0; v < nparams; ++v)
          evs[0].push_back({.kind = ssa_ns::ssa_def_v, .var = v, .site = v});
      site_begin[b] = site;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Phi)
          continue;
        auto use = [&](uint32_t v) {
          if (v != NONE && v < fn.nvars) {
            evs[b].push_back(
                {.kind = ssa_ns::ssa_use_v, .var = v, .site = site});
            ++site;
          }
        };
        use(I.a);
        use(I.b2);
        use(I.c2);
        for (uint32_t av : I.args)
          use(av);
        if (I.dst != NONE) {
          evs[b].push_back(
              {.kind = ssa_ns::ssa_def_v, .var = I.dst, .site = site});
          ++site;
        }
      }
    }
    n_sites = site;
  }
  const ssa_ns::ssa_renaming_t<V> R = ssa_ns::rename_ssa<V>(
      fn.nvars, n_sites, A.dom_tree, A.fs, pred_rows, placement, evs);
  // use before def: the old walk bailed at an empty version stack in a
  // VISITED block; the same condition, scanned as a table (unreachable
  // blocks were never walked and keep their raw pre-SSA ids -- unreach
  // deletes them later, exactly as before)
  {
    std::vector<char> visited(nb, 0);
    for (ulen b : R.visit_order)
      visited[(uint32_t)b] = 1;
    for (uint32_t b = 0; b < nb; ++b) {
      if (!visited[b])
        continue;
      for (const ssa_ns::ssa_event_t &ev : evs[b])
        if (ev.kind == ssa_ns::ssa_use_v &&
            R.reaching_def[ev.site] == ssa_ns::ssa_none_v)
          return false;
    }
  }
  // replay: mint fresh versions in the walk's order (phi dsts at block
  // heads first, then the instruction defs in program order) -- the same
  // sequence of fresh() calls the interleaved walk made, which is what
  // keeps vreg ids (and everything keyed on them) byte-identical.
  fn.version_base.clear();
  fn.version_no.clear();
  std::vector<uint32_t> site_vreg(n_sites + total_phis, NONE);
  for (uint32_t v = 0; v < nparams; ++v)
    site_vreg[v] = v; // params pre-materialized
  auto alloc_def = [&](uint32_t base, uint32_t site) {
    uint32_t v = fn.nvars + (uint32_t)fn.version_base.size();
    fn.version_base.push_back(base);
    fn.version_no.push_back((uint32_t)R.def_ordinal[site]);
    st.versions++;
    site_vreg[site] = v;
  };
  for (ulen bb : R.visit_order) {
    const uint32_t b = (uint32_t)bb;
    for (uint32_t pi = 0; pi < (uint32_t)placement_rows[b].size(); ++pi)
      alloc_def((uint32_t)placement_rows[b][pi],
                n_sites + phi_ord_base[b] + pi);
    for (const ssa_ns::ssa_event_t &ev : evs[b])
      if (ev.kind == ssa_ns::ssa_def_v && site_vreg[ev.site] == NONE)
        alloc_def((uint32_t)ev.var, ev.site);
  }
  // apply: rewrite uses against the table (sites re-derived by the same
  // enumeration), lower phi records into the 2-slot Phi instrs. The IR's
  // phis carry TWO (arg, pred) slots; a record entry at position i lands
  // in slot 0 (a/t) when i == 0, else slot 1 (b2/f) -- for the <= 2 pred
  // blocks pred_norm guarantees this is a bijection, and the rule also
  // reproduces the old last-match seeding verbatim on any arity.
  for (ulen bb : R.visit_order) {
    const uint32_t b = (uint32_t)bb;
    uint32_t pi = 0;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k != K::Phi)
        break;
      if (pi < (uint32_t)placement_rows[b].size()) {
        const uint32_t phi = phi_ord_base[b] + pi;
        const uint32_t e0 = R.phi_entry_begin[phi];
        I.dst = site_vreg[R.phi_def_site[phi]];
        for (uint32_t e = e0; e < R.phi_entry_begin[phi + 1]; ++e) {
          if (R.phi_entry_pred[e] == ssa_ns::ssa_none_v)
            continue; // never seeded (unreachable pred / dup slot)
          const uint32_t arg = R.phi_entry_def[e] == ssa_ns::ssa_none_v
                                   ? NONE
                                   : site_vreg[R.phi_entry_def[e]];
          // Minimal (non-pruned) SSA: an arg may be UNDEF on an edge
          // (variable not live-in there; scoping guarantees it is never
          // read on that path -- DCE drops the dead phi). The pred id is
          // recorded with the arg so later CFG edits can prune/rekey
          // args by pred identity, never by position.
          if (e == e0) {
            I.a = arg;
            I.t = (uint32_t)R.phi_entry_pred[e];
          } else {
            I.b2 = arg;
            I.f = (uint32_t)R.phi_entry_pred[e];
          }
        }
      }
      ++pi;
    }
    uint32_t site = site_begin[b];
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Phi)
        continue;
      auto use = [&](uint32_t &fld) {
        if (fld != NONE && fld < fn.nvars) {
          const uint32_t rd = (uint32_t)R.reaching_def[site];
          fld = rd == ssa_ns::ssa_none_v ? NONE : site_vreg[rd];
          ++site;
        }
      };
      use(I.a);
      use(I.b2);
      use(I.c2);
      for (uint32_t &av : I.args)
        use(av);
      if (I.dst != NONE) {
        I.dst = site_vreg[site];
        ++site;
      }
    }
  }
  return true;
}
