#pragma once
// === fuzz_kernels.hpp ========================================
// the COMPONENT fuzzer: every generic kernel -- graph_ns's dominators,
// frontiers and phi placements, ssa_ns's placement wrapper / renaming /
// reflow / merge chains, every opt_ns kernel, and (fuzz_graphs.hpp,
// included below) the graph library's own algorithm layer: the inverse
// universe family, SCC/topological orders, BFS/DFS/Dijkstra/A*, the
// flow/cut family, the spanning/matching family, the biconnectivity
// and Euler families, and the minimum-arc-set/distances family --
// hammered on
// random inputs, each against an independent REFERENCE. The
// differential fuzzer (fuzz_run.hpp) proves the PIPELINE agrees with
// the oracle end to end; this one proves the ALGORITHMS agree with
// their math, one component at a time, including input shapes the
// compiler's own IR can never spell (3-pred joins, arity-1 phis at a
// self-cycling entry, junk operands, unreachable islands, duplicate
// pred slots).
//
// THE METHOD -- three check kinds, per component:
//   * DIFFERENTIAL. A reference coded from the SPEC, never from the
//     kernel's source: bitset round-robin dominators (vs the
//     semi-dominator algorithm), the Cytron frontier definition, the
//     worklist iterated-dominance-frontier, a chaotic-iteration SCCP
//     (vs the two-worklist engine), the dom-path FOLD for the memory
//     table (vs the watermark machinery), greedy replicas for the
//     order-sensitive sweeps. Bit-for-bit agreement required.
//   * SEMANTIC. Random walks whose meaning the check defines on its
//     own: rename_ssa's tables vs the VALUES a random execution path
//     carries (the SSA verifier), pcopy vs a parallel-assignment
//     simulation, ai_ns's intervals vs the concrete values of feasible
//     paths, the merge-chain plan vs actually walking the rebuilt CFG.
//   * INVARIANT. Structural contracts for ANY input: the policy
//     wrapper is exactly the pure relation minus its two filters,
//     born route sites carry distinct incoming routes, splice pairs
//     preserve reachability, no pcopy register is written twice.
//
// SEED SPACE: --fuzz-kernels has its own seeds (--fuzz-kernels-from),
// completely separate from the program fuzzer's --fuzz-from (fuzz_gen's
// fixtures ride THOSE seeds; this fuzzer perturbs nothing committed).
// Reproduce any failure with:
//   ./minijz --fuzz-kernels=1 --fuzz-kernels-from=<seed> --fuzz-kernel=<name>
// (names in the report table; --fuzz-kernel= is a substring filter).

#include "fuzz_base.hpp"

// (the name table, the stats, the checker, the RNG trio -- everything
// both body files share -- live in fuzz_base.hpp; this file keeps the
// CFG generators and its own kernel bodies)

// the random CFG: valid succ ids only (the forests index by entry id),
// biased toward compiler shapes -- back edges, duplicate slots (cbr X X),
// self loops, dead blocks. Preds derived positionally ascending (the
// adapters' convention), one entry per valid succ slot.
struct KzCfg {
  ulen nb{};
  std::vector<std::vector<ulen>> succs{}, preds{};
  gns::treversal_result_t<V> fs{}, fp_inv{}, pf{}, dom_tree{}, df{};
  std::vector<ulen> idom{};
  std::vector<char> reach{};
};
static KzCfg kz_cfg(std::mt19937_64 &rng, ulen nb) {
  KzCfg C{};
  C.nb = nb;
  C.succs.assign(nb, {});
  for (ulen b = 0; b < nb; ++b) {
    int deg = kz_chance(rng, 20) ? 0 : kz_chance(rng, 50) ? 1
          : kz_chance(rng, 60)   ? 2
                                 : 3;
    for (int j = 0; j < deg; ++j) {
      ulen t; // 40% back-ish (target <= b), else anywhere
      if (kz_chance(rng, 40))
        t = kz_pick(rng, b + 1);
      else
        t = kz_pick(rng, nb);
      if (deg == 2 && j == 1 && kz_chance(rng, 25))
        t = C.succs[b][0]; // cbr(c, X, X)
      C.succs[b].push_back(t);
    }
  }
  C.preds.assign(nb, {});
  for (ulen b = 0; b < nb; ++b)
    for (ulen s : C.succs[b])
      C.preds[s].push_back(b);
  C.fs = gns::make_basic_forest<V>(C.succs);
  C.fp_inv = gns::make_basic_inv_forest<V>(C.succs);
  C.pf = gns::make_basic_forest<V>(C.preds);
  C.idom = gns::calculate_dominators_from_entry<V>(0, C.fs, C.fp_inv);
  C.dom_tree = gns::make_dominator_forest_given_dominators<V>(C.idom);
  C.df = gns::calculate_dominance_frontiers_given_dominators<V>(C.idom,
                                                                C.fp_inv);
  C.reach.assign(nb, 0);
  {
    std::vector<ulen> wl{0};
    C.reach[0] = 1;
    while (!wl.empty()) {
      ulen b = wl.back();
      wl.pop_back();
      for (ulen s : C.succs[b])
        if (!C.reach[s]) {
          C.reach[s] = 1;
          wl.push_back(s);
        }
    }
  }
  return C;
}

// ---- the reference graph layer (coded from the definitions) ---------------
// reachability, dominator sets by round-robin intersection over the
// reachable subgraph, idom = the strict dominator whose own dom-set is
// as large as the strict set, frontiers straight off Cytron's definition.
static std::vector<char> kz_ref_reach(
    const std::vector<std::vector<ulen>> &succs, ulen entry) {
  const ulen nb = ulen(succs.size());
  std::vector<char> r(nb, 0);
  if (!nb || entry >= nb)
    return r;
  std::vector<ulen> wl{entry};
  r[entry] = 1;
  while (!wl.empty()) {
    ulen b = wl.back();
    wl.pop_back();
    for (ulen s : succs[b])
      if (s < nb && !r[s]) {
        r[s] = 1;
        wl.push_back(s);
      }
  }
  return r;
}
static std::vector<std::vector<char>>
kz_ref_domsets(const std::vector<std::vector<ulen>> &succs,
               const std::vector<std::vector<ulen>> &preds,
               const std::vector<char> &reach) {
  const ulen nb = ulen(succs.size());
  std::vector<std::vector<char>> dom(nb, std::vector<char>(nb, 0));
  for (ulen b = 0; b < nb; ++b)
    if (reach[b])
      for (ulen x = 0; x < nb; ++x)
        dom[b][x] = reach[x] ? 1 : 0;
  dom[0][0] = 1;
  for (ulen x = 0; x < nb; ++x)
    if (x != 0)
      dom[0][x] = 0;
  bool ch = true;
  while (ch) {
    ch = false;
    for (ulen b = 1; b < nb; ++b) {
      if (!reach[b])
        continue;
      std::vector<char> nd(nb, 1);
      nd[b] = 1;
      bool any = false;
      for (ulen p : preds[b]) {
        if (p >= nb || !reach[p])
          continue;
        any = true;
        for (ulen x = 0; x < nb; ++x)
          nd[x] = char(nd[x] && dom[p][x]);
      }
      if (!any)
        continue;
      nd[b] = 1;
      for (ulen x = 0; x < nb; ++x)
        if (nd[x] != dom[b][x]) {
          dom[b] = nd;
          ch = true;
          break;
        }
    }
  }
  return dom;
}
static std::vector<ulen> kz_ref_idom(const std::vector<std::vector<char>> &dom,
                                     const std::vector<char> &reach) {
  const ulen nb = ulen(dom.size());
  std::vector<ulen> idom(nb, kz_none());
  for (ulen b = 0; b < nb; ++b) {
    if (!reach[b] || b == 0)
      continue;
    ulen nstrict = 0, best = kz_none();
    for (ulen x = 0; x < nb; ++x)
      if (x != b && dom[b][x]) {
        ++nstrict;
        best = x; // any strict dominator; refine below
      }
    for (ulen x = 0; x < nb; ++x) {
      if (x == b || !dom[b][x])
        continue;
      ulen sz = 0;
      for (ulen y = 0; y < nb; ++y)
        sz += dom[x][y] ? 1u : 0u;
      if (sz == nstrict) {
        best = x;
        break;
      }
    }
    idom[b] = best;
  }
  return idom;
}
static std::vector<std::vector<ulen>>
kz_ref_df(const std::vector<std::vector<ulen>> &preds,
          const std::vector<std::vector<char>> &dom,
          const std::vector<char> &reach) {
  const ulen nb = ulen(preds.size());
  std::vector<std::vector<ulen>> df(nb);
  for (ulen a = 0; a < nb; ++a) {
    if (!reach[a])
      continue;
    std::vector<char> in(nb, 0);
    for (ulen b = 0; b < nb; ++b) {
      if (!reach[b])
        continue;
      bool has = false;
      for (ulen p : preds[b]) {
        if (p >= nb || !reach[p])
          continue;
        if (dom[p][a]) {
          has = true;
          break;
        }
      }
      if (!has)
        continue;
      // b in DF(a) iff a dominates a pred of b and (a == b or a does
      // not STRICTLY dominate b)
      if (a != b && dom[b][a])
        continue;
      in[b] = 1;
    }
    for (ulen b = 0; b < nb; ++b)
      if (in[b])
        df[a].push_back(b);
  }
  return df;
}
// the iterated dominance frontier, straight from Cytron's worklist
static std::vector<ulen> kz_ref_idf(
    const std::vector<std::vector<ulen>> &df_sets,
    const std::vector<ulen> &def_blocks) {
  std::vector<char> in_defs(df_sets.size(), 0);
  std::vector<ulen> W{};
  for (ulen d : def_blocks) {
    if (d >= df_sets.size() || in_defs[d])
      continue;
    in_defs[d] = 1;
    W.push_back(d);
  }
  std::vector<char> placed(df_sets.size(), 0);
  std::vector<ulen> idf{};
  while (!W.empty()) {
    const ulen x = W.back();
    W.pop_back();
    for (ulen y : df_sets[x]) {
      if (y >= df_sets.size() || placed[y])
        continue;
      placed[y] = 1;
      idf.push_back(y);
      if (!in_defs[y]) {
        in_defs[y] = 1;
        W.push_back(y);
      }
    }
  }
  return idf;
}

// forest rows as a sorted vector (set comparison; order-free checks)
static std::vector<ulen> kz_row_sorted(const auto &forest, ulen b) {
  auto r = forest.range()[b];
  std::vector<ulen> v(r.begin(), r.end());
  for (size_t i = 1; i < v.size(); ++i) // tiny insertion sort
    for (size_t j = i; j > 0 && v[j - 1] > v[j]; --j)
      std::swap(v[j - 1], v[j]);
  return v;
}

// ---------------------------------------------------------------------------
// 0. gns.forests: the CSR pair + the bipartite universe transpose
// ---------------------------------------------------------------------------
static void fz_forests(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 7);
  std::vector<std::vector<ulen>> rows(n);
  for (ulen b = 0; b < n; ++b) {
    const int k = int(kz_pick(rng, 5));
    for (int j = 0; j < k; ++j)
      rows[b].push_back(kz_pick(rng, n)); // duplicates on purpose
  }
  // the forest preserves the rows exactly
  auto f = gns::make_basic_forest<V>(rows);
  for (ulen b = 0; b < n; ++b) {
    auto r = f.range()[b];
    R.ok(ulen(r.size()) == rows[b].size(), "forest: row size preserved");
    bool eq = ulen(r.size()) == rows[b].size();
    for (ulen j = 0; j < rows[b].size() && eq; ++j)
      eq = (r[j] == rows[b][j]);
    R.ok(eq, "forest: row contents preserved");
  }
  // the SQUARE transpose: entry e's row lists its source rows -- the
  // square sibling fills cursors BACKWARD, so the order is DESCENDING
  std::vector<std::vector<ulen>> want(n);
  for (ulen b = 0; b < n; ++b)
    for (ulen e : rows[b])
      want[e].push_back(b);
  auto inv = gns::make_basic_inv_forest<V>(rows);
  for (ulen e = 0; e < n; ++e) {
    std::vector<ulen> w = want[e];
    std::reverse(w.begin(), w.end()); // descending source order
    auto r = inv.range()[e];
    bool eq = ulen(r.size()) == w.size();
    for (ulen j = 0; j < w.size() && eq; ++j)
      eq = (r[j] == w[j]);
    R.ok(eq, "inv forest: transpose rows, descending source order");
  }
  // the UNIVERSE transpose: non-square relations, ascending source order
  const ulen uni = 1 + kz_pick(rng, 9);
  std::vector<std::vector<ulen>> rws;
  for (ulen v = 0; v < n; ++v) {
    std::vector<ulen> row{};
    const int k = int(kz_pick(rng, 3));
    for (int j = 0; j < k; ++j)
      row.push_back(kz_pick(rng, uni)); // entries may pass the row count
    rws.push_back(row);
  }
  std::vector<std::vector<ulen>> wuni(uni);
  for (ulen v = 0; v < n; ++v)
    for (ulen e : rws[v])
      wuni[e].push_back(v); // ascending by construction
  auto u = gns::make_basic_inv_forest_universe<V>(rws, uni);
  for (ulen e = 0; e < uni; ++e) {
    auto r = u.range()[e];
    bool eq = ulen(r.size()) == wuni[e].size();
    for (ulen j = 0; j < wuni[e].size() && eq; ++j)
      eq = (r[j] == wuni[e][j]);
    R.ok(eq, "universe: transpose over the entry space, ascending rows");
  }
}

// ---------------------------------------------------------------------------
// 1. gns.dom: dominators vs the bitset round-robin; the dom tree
// ---------------------------------------------------------------------------
static void fz_dom(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 1 + kz_pick(rng, 7);
  KzCfg C = kz_cfg(rng, nb);
  auto dom = kz_ref_domsets(C.succs, C.preds, C.reach);
  auto ridom = kz_ref_idom(dom, C.reach);
  for (ulen b = 1; b < nb; ++b) {
    if (!C.reach[b])
      continue; // unreachable behavior is not pinned by any client
    R.ok(C.idom[b] == ridom[b], "dom: idom matches the round-robin fixpoint");
  }
  R.ok(C.idom[0] == 0 || C.idom[0] == kz_none(),
       "dom: entry is its own idom or none");
  // the dominator forest: every reachable non-entry block appears
  // exactly once, under its reference idom
  std::vector<ulen> parent_of(nb, kz_none());
  for (ulen b = 0; b < nb; ++b) {
    auto ch = C.dom_tree.range()[b];
    std::vector<char> seen(nb, 0);
    for (ulen x : ch) {
      R.ok(x < nb && !seen[x], "dom tree: no duplicate children");
      if (x < nb)
        seen[x] = 1;
      if (x < nb && C.reach[x] && x != 0)
        parent_of[x] = b;
    }
  }
  for (ulen b = 1; b < nb; ++b) {
    if (!C.reach[b])
      continue;
    R.ok(parent_of[b] == ridom[b], "dom tree: child sits under its idom");
  }
}

// ---------------------------------------------------------------------------
// 2. gns.df: dominance frontiers vs Cytron's definition
// ---------------------------------------------------------------------------
static void fz_df(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 1 + kz_pick(rng, 7);
  KzCfg C = kz_cfg(rng, nb);
  auto dom = kz_ref_domsets(C.succs, C.preds, C.reach);
  auto rdf = kz_ref_df(C.preds, dom, C.reach);
  for (ulen a = 0; a < nb; ++a) {
    if (!C.reach[a])
      continue;
    R.ok(kz_row_sorted(C.df, a) == rdf[a],
         "df: frontier set matches the definition");
  }
}

// ---------------------------------------------------------------------------
// 3. gns.phis: the pure Cytron relation + the block-major transpose
// ---------------------------------------------------------------------------
static void fz_phis(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 1 + kz_pick(rng, 7);
  KzCfg C = kz_cfg(rng, nb);
  // the DF relation the placements consume is the KERNEL's forest (its
  // reachable rows are validated separately, in gns.df); the IDF
  // reference below runs over exactly those rows, so this component
  // tests the ITERATION, not the frontier underneath
  std::vector<std::vector<ulen>> df_sets(nb);
  for (ulen a = 0; a < nb; ++a)
    df_sets[a] = kz_row_sorted(C.df, a);
  const ulen nv = kz_pick(rng, 6);
  std::vector<std::vector<ulen>> defs(nv);
  for (ulen v = 0; v < nv; ++v) {
    const int k = int(kz_pick(rng, 5));
    for (int j = 0; j < k; ++j)
      defs[v].push_back(kz_pick(rng, nb)); // duplicates count, as seeds
  }
  const auto pure = gns::calculate_phi_placements<V>(C.df, defs);
  R.ok(ulen(pure.range().size()) == nv, "phis: one row per variable");
  for (ulen v = 0; v < nv; ++v) {
    std::vector<ulen> got = kz_row_sorted(pure, v);
    std::vector<ulen> want = kz_ref_idf(df_sets, defs[v]);
    for (size_t i = 1; i < want.size(); ++i)
      for (size_t j = i; j > 0 && want[j - 1] > want[j]; --j)
        std::swap(want[j - 1], want[j]);
    R.ok(got == want, "phis: row v = IDF(defs(v)), every def count");
  }
  // the transpose: block-major rows, ascending var order, universe = nb
  const auto inv = gns::calculate_inv_phi_placements<V>(C.df, defs);
  R.ok(ulen(inv.range().size()) == nb, "phis: transpose sized by blocks");
  std::vector<std::vector<ulen>> wrow(nb);
  for (ulen v = 0; v < nv; ++v) {
    std::vector<ulen> idf = kz_ref_idf(df_sets, defs[v]);
    for (ulen blk : idf)
      wrow[blk].push_back(v);
  }
  for (ulen b = 0; b < nb; ++b) {
    auto r = inv.range()[b];
    bool eq = ulen(r.size()) == wrow[b].size();
    for (ulen j = 0; j < wrow[b].size() && eq; ++j)
      eq = (r[j] == wrow[b][j]);
    R.ok(eq, "phis: transposed rows, ascending variable order");
  }
}

// ---------------------------------------------------------------------------
// 4. ssa.wrapper: the policy layer is exactly pure placement minus the
//    two IR filters (<2 def blocks, <2 pred slots)
// ---------------------------------------------------------------------------
static void fz_wrapper(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 1 + kz_pick(rng, 7);
  KzCfg C = kz_cfg(rng, nb);
  std::vector<std::vector<ulen>> df_sets(nb);
  for (ulen a = 0; a < nb; ++a)
    df_sets[a] = kz_row_sorted(C.df, a);
  const ulen nv = kz_pick(rng, 6);
  std::vector<std::vector<ulen>> defs(nv);
  for (ulen v = 0; v < nv; ++v) {
    const int k = int(kz_pick(rng, 5));
    for (int j = 0; j < k; ++j)
      defs[v].push_back(kz_pick(rng, nb));
  }
  auto pl = ssa_ns::calculate_phi_placements<V>(C.df, C.pf, defs);
  R.ok(ulen(pl.range().size()) == nb, "wrapper: one row per block");
  // reference: pure IDF per var (over the kernel's DF rows), then the
  // two policies, then the transpose
  std::vector<std::vector<ulen>> want(nb);
  for (ulen v = 0; v < nv; ++v) {
    if (defs[v].size() < 2)
      continue; // the <2-defs skip (raw row size, duplicates count)
    for (ulen blk : kz_ref_idf(df_sets, defs[v])) {
      if (blk >= nb)
        continue;
      if (C.preds[blk].size() < 2)
        continue; // the >=2-pred-slot rule (duplicates count)
      bool dup = false;
      for (ulen x : want[blk])
        dup |= (x == v);
      if (!dup)
        want[blk].push_back(v); // ascending: v ascends
    }
  }
  for (ulen b = 0; b < nb; ++b) {
    auto r = pl.range()[b];
    bool eq = ulen(r.size()) == want[b].size();
    for (ulen j = 0; j < want[b].size() && eq; ++j)
      eq = (r[j] == want[b][j]);
    R.ok(eq, "wrapper: pure placement minus the two IR policies");
  }
}

// ---------------------------------------------------------------------------
// 5. ssa.rename: the walk vs a recursive reference, and the tables vs the
//    VALUES a random execution path carries (the SSA verifier)
// ---------------------------------------------------------------------------
struct KzRenRef {
  std::vector<ulen> visit_order{}, reaching_def{}, def_ordinal{};
  std::vector<ulen> phi_block{}, phi_var{}, phi_def_site{},
      phi_entry_begin{}, phi_entry_pred{}, phi_entry_def{};
};
static KzRenRef kz_rename_ref(
    ulen nvars, ulen nsites, const std::vector<std::vector<ulen>> &dom_rows,
    const std::vector<std::vector<ulen>> &succ_rows,
    const std::vector<std::vector<ulen>> &pred_rows,
    const std::vector<std::vector<ulen>> &phi_rows,
    const std::vector<std::vector<ssa_ns::ssa_event_t>> &evs) {
  using namespace ssa_ns;
  const ulen nb = ulen(dom_rows.size());
  KzRenRef t{};
  ulen n_phis = 0, n_entries = 0;
  std::vector<ulen> ord_base(nb, 0);
  {
    ulen ord = 0, ent = 0;
    for (ulen b = 0; b < nb; ++b) {
      ord_base[b] = ord;
      for (ulen v : phi_rows[b]) {
        t.phi_block.push_back(b);
        t.phi_var.push_back(v);
        t.phi_def_site.push_back(nsites + ord);
        t.phi_entry_begin.push_back(ent);
        ent += ulen(pred_rows[b].size());
        ++ord;
      }
    }
    t.phi_entry_begin.push_back(ent);
    n_phis = ord;
    n_entries = ent;
  }
  t.reaching_def.assign(nsites + n_phis, ssa_none_v);
  t.def_ordinal.assign(nsites + n_phis, ssa_none_v);
  t.phi_entry_pred.assign(n_entries, ssa_none_v);
  t.phi_entry_def.assign(n_entries, ssa_none_v);
  std::vector<std::vector<ulen>> stacks(nvars);
  std::vector<ulen> undo{}; // the var of every push, for the pop-back
  std::function<void(ulen)> walk = [&](ulen b) {
    const size_t mark = undo.size();
    t.visit_order.push_back(b);
    for (ulen pi = 0; pi < phi_rows[b].size(); ++pi) {
      const ulen v = phi_rows[b][pi], site = t.phi_def_site[ord_base[b] + pi];
      t.def_ordinal[site] = ulen(stacks[v].size());
      stacks[v].push_back(site);
      undo.push_back(v);
    }
    for (const ssa_event_t &ev : evs[b]) {
      if (ev.kind == ssa_use_v) {
        t.reaching_def[ev.site] =
            stacks[ev.var].empty() ? ssa_none_v : stacks[ev.var].back();
      } else {
        t.def_ordinal[ev.site] = ulen(stacks[ev.var].size());
        stacks[ev.var].push_back(ev.site);
        undo.push_back(ev.var);
      }
    }
    for (ulen s : succ_rows[b]) {
      if (s >= nb)
        continue;
      for (ulen pi = 0; pi < phi_rows[s].size(); ++pi) {
        const ulen phi = ord_base[s] + pi, v = t.phi_var[phi];
        ulen entry = ssa_none_v;
        for (ulen j = 0; j < pred_rows[s].size(); ++j)
          if (pred_rows[s][j] == b)
            entry = j; // the LAST slot of a repeated pred wins
        if (entry == ssa_none_v)
          continue;
        const ulen at = t.phi_entry_begin[phi] + entry;
        t.phi_entry_pred[at] = b;
        t.phi_entry_def[at] =
            stacks[v].empty() ? ssa_none_v : stacks[v].back();
      }
    }
    for (ulen c : dom_rows[b])
      walk(c);
    while (undo.size() > mark) {
      stacks[undo.back()].pop_back();
      undo.pop_back();
    }
  };
  if (nb)
    walk(0);
  return t;
}

static bool kz_ren_eq(const ssa_ns::ssa_renaming_t<V> &k, const KzRenRef &r) {
  return k.visit_order == r.visit_order && k.reaching_def == r.reaching_def &&
         k.def_ordinal == r.def_ordinal && k.phi_block == r.phi_block &&
         k.phi_var == r.phi_var && k.phi_def_site == r.phi_def_site &&
         k.phi_entry_begin == r.phi_entry_begin &&
         k.phi_entry_pred == r.phi_entry_pred &&
         k.phi_entry_def == r.phi_entry_def;
}

static void fz_rename(std::mt19937_64 &rng, KzRec &R) {
  using ssa_ns::ssa_def_v;
  using ssa_ns::ssa_event_t;
  using ssa_ns::ssa_use_v;
  const ulen nb = 1 + kz_pick(rng, 8);
  KzCfg C = kz_cfg(rng, nb);
  const ulen nvars = 1 + kz_pick(rng, 5);
  // random def/use streams; defs double as the placement seeds (the
  // compiler derives def blocks from its own events exactly this way)
  std::vector<std::vector<ssa_event_t>> evs(nb);
  std::vector<std::vector<ulen>> defs(nvars);
  ulen nsites = 0;
  for (ulen b = 0; b < nb; ++b) {
    const int k = int(kz_pick(rng, 4));
    for (int j = 0; j < k; ++j) {
      const ulen v = kz_pick(rng, nvars), site = nsites++;
      if (kz_chance(rng, 55)) {
        evs[b].push_back({ssa_use_v, v, site});
      } else {
        evs[b].push_back({ssa_def_v, v, site});
        defs[v].push_back(b);
      }
    }
  }
  // the two placements: the compiler's policy wrapper, and the PURE
  // relation (whose supersets include the single-def phis the wrapper
  // drops -- rename must handle both, any placement is just data)
  auto wrap = ssa_ns::calculate_phi_placements<V>(C.df, C.pf, defs);
  auto pureinv = gns::calculate_inv_phi_placements<V>(C.df, defs);
  std::vector<std::vector<ulen>> dom_rows(nb), phi_w(nb), phi_p(nb);
  for (ulen b = 0; b < nb; ++b) {
    auto dch = C.dom_tree.range()[b];
    dom_rows[b].assign(dch.begin(), dch.end());
    auto w = wrap.range()[b];
    phi_w[b].assign(w.begin(), w.end());
    auto p = pureinv.range()[b];
    phi_p[b].assign(p.begin(), p.end());
  }
  // (a) the kernel vs the recursive reference, bit-for-bit, both ways
  auto K1 = ssa_ns::rename_ssa<V>(nvars, nsites, C.dom_tree, C.fs, C.pf, wrap,
                                   evs);
  KzRenRef r1 = kz_rename_ref(nvars, nsites, dom_rows, C.succs, C.preds,
                              phi_w, evs);
  R.ok(kz_ren_eq(K1, r1), "rename: tables match the recursive walk (wrap)");
  auto K2 = ssa_ns::rename_ssa<V>(nvars, nsites, C.dom_tree, C.fs, C.pf,
                                   pureinv, evs);
  KzRenRef r2 = kz_rename_ref(nvars, nsites, dom_rows, C.succs, C.preds,
                              phi_p, evs);
  R.ok(kz_ren_eq(K2, r2), "rename: tables match the recursive walk (pure)");
  // (b) the SEMANTIC verifier: walk random paths, carry the value of
  // every var (the site of its last def along the path, phis included),
  // and demand the kernel's fixed tables agree everywhere:
  //   * a use's reaching_def is the value the path carries
  //   * crossing edge p->b, the phi entries at b for pred p carry the
  //     END-OF-p value (the last matching slot; earlier dup slots stay
  //     unseeded -- the hand-rolled rule, pinned)
  // Minimal-superset placement (the pure relation) makes this a theorem;
  // the walk is the proof-by-random-paths half.
  const int paths = 6;
  for (int pw = 0; pw < paths; ++pw) {
    std::vector<ulen> val(nvars, ssa_ns::ssa_none_v);
    ulen b = 0;
    for (int step = 0; step < int(4 * nb + 16) && b < nb; ++step) {
      // phis are the block head: on this path they define their var
      for (ulen p2 = 0; p2 < K2.phi_block.size(); ++p2)
        if (K2.phi_block[p2] == b)
          val[K2.phi_var[p2]] = K2.phi_def_site[p2];
      for (const ssa_event_t &ev : evs[b]) {
        if (ev.kind == ssa_use_v)
          R.ok(K2.reaching_def[ev.site] == val[ev.var],
               "rename: use reads the value the path carries");
        else
          val[ev.var] = ev.site;
      }
      std::vector<ulen> end = val;
      if (C.succs[b].empty())
        break;
      const ulen s = C.succs[b][kz_pick(rng, ulen(C.succs[b].size()))];
      if (s >= nb)
        break;
      // edge checks: every phi at s that reads a var v must have the
      // end-of-b value in the LAST slot naming b, none in earlier dups
      for (ulen p2 = 0; p2 < K2.phi_block.size(); ++p2) {
        if (K2.phi_block[p2] != s)
          continue;
        const ulen v = K2.phi_var[p2];
        ulen last = ssa_ns::ssa_none_v;
        for (ulen j = 0; j < C.preds[s].size(); ++j)
          if (C.preds[s][j] == b)
            last = j;
        if (last == ssa_ns::ssa_none_v)
          continue; // inconsistent input: cannot happen (preds derived)
        for (ulen j = 0; j < C.preds[s].size(); ++j)
          if (C.preds[s][j] == b && j != last) {
            const ulen at = K2.phi_entry_begin[p2] + j;
            R.ok(K2.phi_entry_pred[at] == ssa_ns::ssa_none_v &&
                     K2.phi_entry_def[at] == ssa_ns::ssa_none_v,
                 "rename: dup-pred slots before the last stay unseeded");
          }
        const ulen at = K2.phi_entry_begin[p2] + last;
        R.ok(K2.phi_entry_pred[at] == b &&
                 K2.phi_entry_def[at] == end[v],
             "rename: the phi arg carries the end-of-pred value");
      }
      val = end;
      b = s;
    }
  }
}

// ---------------------------------------------------------------------------
// 6. ssa.reflow: the rebuild-with-removal index math vs a direct emit
// ---------------------------------------------------------------------------
static void fz_reflow(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 1 + kz_pick(rng, 6);
  ulen total = 0;
  std::vector<ulen> ob{}, oe{};
  for (ulen b = 0; b < nb; ++b) {
    const ulen w = kz_pick(rng, 5);
    ob.push_back(total);
    total += w;
    oe.push_back(total);
  }
  std::vector<ulen> keep(total, 0), after(total, 0), head(nb, 0);
  for (ulen i = 0; i < total; ++i) {
    keep[i] = kz_chance(rng, 80) ? 1 : 0;
    after[i] = kz_pick(rng, 3);
  }
  for (ulen b = 0; b < nb; ++b)
    head[b] = kz_pick(rng, 4);
  auto plan = ssa_ns::calculate_reflow_plan<V>(ob, oe, keep, head, after);
  // reference: emit the new layout block by block
  ulen pos = 0;
  std::vector<ulen> wbegin(nb), wend(nb), wmap(total, ssa_ns::ssa_none_v);
  for (ulen b = 0; b < nb; ++b) {
    wbegin[b] = pos;
    pos += head[b];
    for (ulen i = ob[b]; i < oe[b]; ++i) {
      if (!keep[i])
        continue;
      wmap[i] = pos;
      ++pos;
      pos += after[i];
    }
    wend[b] = pos;
  }
  R.ok(plan.total == pos, "reflow: total = heads + kept + afters");
  R.ok(plan.new_begin == wbegin && plan.new_end == wend,
       "reflow: new block ranges match the direct emit");
  R.ok(plan.old_to_new == wmap, "reflow: old->new map matches");
  for (ulen i = 0; i < total; ++i)
    R.ok((plan.old_to_new[i] == ssa_ns::ssa_none_v) == (keep[i] == 0),
         "reflow: dropped instrs map to NONE exactly");
}

// ---------------------------------------------------------------------------
// 7. ssa.chain: the N-pred -> binary merge chain, formula + structure
// ---------------------------------------------------------------------------
static void fz_chain(std::mt19937_64 &rng, KzRec &R) {
  const ulen k = kz_pick(rng, 8), block = kz_pick(rng, 7);
  std::vector<ulen> pr{};
  for (ulen j = 0; j < k; ++j)
    pr.push_back(kz_pick(rng, 10)); // any ids; the plan is pure index math
  auto plan = ssa_ns::calculate_merge_chain_plan<V>(block, pr);
  const ulen want_chains = k >= 2 ? k - 2 : 0;
  R.ok(plan.block == block && plan.chain_count == want_chains,
       "chain: chain_count = max(0, k-2)");
  R.ok(plan.redirect_pred.size() == (k > 2 ? k - 1 : 0),
       "chain: one redirect per extra pred (none for k <= 2)");
  for (ulen j = 1; k > 2 && j < k; ++j) {
    R.ok(plan.redirect_pred[j - 1] == pr[j] &&
             plan.redirect_chain[j - 1] == (j <= 2 ? 0 : j - 2),
         "chain: p_j retargets to chain 0/0/1/2.. (right-leaning)");
  }
  // structural: rebuild the CFG the plan describes and walk it -- every
  // original pred must still reach the block exactly once, and no block
  // in the chain may have more than two preds
  if (k >= 1) {
    const ulen nb2 = block + 1 + want_chains;
    std::vector<std::vector<ulen>> npreds(nb2);
    npreds[block].push_back(pr[0]);
    if (k >= 3) {
      npreds[nb2 - want_chains + 0].push_back(pr[1]);
      npreds[nb2 - want_chains + 0].push_back(pr[2]);
      for (ulen c = 1; c < want_chains; ++c) {
        const ulen cb = nb2 - want_chains + c;
        npreds[cb].push_back(cb - 1);
        if (2 + c < k)
          npreds[cb].push_back(pr[2 + c]);
      }
      npreds[block].push_back(nb2 - 1);
    } else if (k == 2) {
      npreds[block].push_back(pr[1]);
    }
    for (ulen b = 0; b < nb2; ++b) {
      R.ok(npreds[b].size() <= 2, "chain: every block keeps <= 2 preds");
      (void)b;
    }
    // walk each pred to the block through the chain
    const auto chain_of = [&](ulen j) { return j <= 2 ? 0 : j - 2; };
    for (ulen j = 0; j < k; ++j) {
      ulen at = j == 0 ? block : (k >= 3 ? nb2 - want_chains + chain_of(j)
                                         : block);
      int hops = 0;
      while (at != block && hops++ <= int(k) + 2) {
        // chain block c targets b iff c+1 == chain_count, else c+1
        const ulen c = at - (nb2 - want_chains);
        at = (c + 1 == want_chains) ? block : (nb2 - want_chains + c + 1);
      }
      R.ok(at == block, "chain: every pred still reaches the join");
    }
  }
}

// ---------------------------------------------------------------------------
// 8. opt.mark: mark-sweep liveness vs a reference BFS (junk tolerated)
// ---------------------------------------------------------------------------
static void fz_mark(std::mt19937_64 &rng, KzRec &R) {
  const ulen nv = kz_pick(rng, 7), ns = kz_pick(rng, 9);
  std::vector<uintlen_t> def_of(nv, opt_ns::none_v);
  for (ulen v = 0; v < nv; ++v) {
    const int r = int(kz_pick(rng, 100));
    if (r < 70)
      def_of[v] = kz_pick(rng, ns);
    else if (r < 85)
      def_of[v] = opt_ns::none_v;
    else
      def_of[v] = ns + kz_pick(rng, 3); // junk def site
  }
  std::vector<std::vector<uintlen_t>> args(ns);
  for (ulen s = 0; s < ns; ++s) {
    const int k = int(kz_pick(rng, 4));
    for (int j = 0; j < k; ++j) {
      const int r = int(kz_pick(rng, 100));
      if (r < 80)
        args[s].push_back(kz_pick(rng, nv));
      else if (r < 90)
        args[s].push_back(opt_ns::none_v);
      else
        args[s].push_back(nv + kz_pick(rng, 3)); // junk var
    }
  }
  std::vector<uintlen_t> roots{};
  const int rk = int(kz_pick(rng, 4));
  for (int j = 0; j < rk; ++j) {
    if (kz_chance(rng, 80))
      roots.push_back(kz_pick(rng, ns));
    else
      roots.push_back(ns + kz_pick(rng, 3)); // junk root
  }
  auto live = opt_ns::calculate_live_sites<V>(def_of, args, roots);
  // reference: BFS over site -> operand-def edges
  std::vector<char> want(ns, 0);
  std::vector<ulen> wl{};
  for (ulen r : roots)
    if (r < ns && !want[r]) {
      want[r] = 1;
      wl.push_back(r);
    }
  while (!wl.empty()) {
    const ulen s = wl.back();
    wl.pop_back();
    for (ulen v : args[s]) {
      if (v >= nv)
        continue;
      const ulen d = def_of[v];
      if (d != opt_ns::none_v && d < ns && !want[d]) {
        want[d] = 1;
        wl.push_back(d);
      }
    }
  }
  R.ok(live.size() == ns, "mark: one bit per site");
  for (ulen s = 0; s < ns; ++s)
    R.ok(!!live[s] == !!want[s], "mark: liveness matches the BFS closure");
}

// ---------------------------------------------------------------------------
// 9. opt.vn: value numbering, soundness by an independent evaluator --
//    every hit must be a true congruence under the final rename map, and
//    the hit target's block must dominate the hitting site's block
// ---------------------------------------------------------------------------
static void fz_vn(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 2 + kz_pick(rng, 6);
  KzCfg C = kz_cfg(rng, nb);
  const ulen nv = 3 + kz_pick(rng, 7);
  std::vector<char> cmask(nv, 0);
  std::vector<int64_t> cval(nv, 0);
  for (ulen v = 0; v < nv; ++v)
    if (kz_chance(rng, 40)) {
      cmask[v] = 1;
      cval[v] = int64_t(rng() % 2001) - 1000;
    }
  // one defining site per var (SSA), random tag, junk operands sprinkled
  std::vector<ulen> def_site(nv, opt_ns::none_v), site_block{};
  std::vector<opt_ns::vn_site_t<V>> sinfo{};
  std::vector<std::vector<uintlen_t>> sargs{}, spreds{};
  std::vector<std::vector<uintlen_t>> sob(nb);
  auto rnd_var = [&]() -> uintlen_t {
    const int r = int(kz_pick(rng, 100));
    if (r < 80)
      return kz_pick(rng, nv);
    if (r < 90)
      return opt_ns::none_v;
    return nv + kz_pick(rng, 3); // junk: resolve() keeps it as-is
  };
  for (ulen v = 0; v < nv; ++v) {
    if (!kz_chance(rng, 80))
      continue; // no def site
    const ulen b = kz_pick(rng, nb), s = ulen(sinfo.size());
    def_site[v] = s;
    site_block.push_back(b);
    sob[b].push_back(s);
    sargs.emplace_back(); // the new site's rows exist before filling
    spreds.emplace_back();
    opt_ns::vn_site_t<V> si{};
    si.dst = v;
    const int r = int(kz_pick(rng, 100));
    if (r < 20) {
      si.tag = opt_ns::vn_skip_v;
      si.dst = opt_ns::none_v;
    } else if (r < 40) {
      si.tag = opt_ns::vn_const_v;
      si.c = int64_t(rng() % 2001) - 1000;
    } else if (r < 65) {
      si.tag = opt_ns::vn_phi_v;
      const ulen k = 1 + kz_pick(rng, 3);
      for (ulen j = 0; j < k; ++j) {
        sargs[s].push_back(rnd_var());
        spreds[s].push_back(kz_pick(rng, nb + 1));
      }
    } else {
      si.tag = opt_ns::vn_bin_v;
      si.op = 100 + kz_pick(rng, 4);
      if (kz_chance(rng, 50))
        si.flags = opt_ns::vn_comm_v;
      sargs[s].push_back(rnd_var());
      sargs[s].push_back(rnd_var());
    }
    sinfo.push_back(si);
  }
  const ulen ns = ulen(sinfo.size());
  auto res = opt_ns::value_number<V>(nv, C.dom_tree, sob, sinfo, sargs, spreds,
                                     cmask, cval);
  R.ok(res.ren.size() == nv && res.keep.size() == ns,
       "vn: output shapes (ren per var, keep per site)");
  // the kernel keys operands through the map AS OF EACH SITE'S VISIT
  // (the growing map, per the contract: "a hit keys by the canonical
  // representative" -- the representative AT THE VISIT). Reproduce the
  // visit order exactly -- the dom-tree DFS preorder, children in row
  // order, a block's sites in row order -- and a rename ren[v] = w is
  // visible at site s iff v's defining site visited earlier (ren is
  // written exactly once, at that visit). On LEGAL inputs (operands
  // dominated at their use) visit-time and final resolution coincide;
  // the junk regime (self references, use-before-def) is where they
  // part, and only the visit-time keying is the contract.
  std::vector<ulen> visit_rank(ns, kz_none());
  {
    ulen ctr = 0;
    auto dom_rows = C.dom_tree.range();
    std::vector<ulen> stack{0};
    while (!stack.empty()) {
      const ulen b = stack.back();
      stack.pop_back();
      if (b < nb)
        for (ulen s : sob[b])
          visit_rank[s] = ctr++;
      for (ulen i = uintlen_t(std::ranges::size(dom_rows[b])); i-- > 0;)
        stack.push_back(uintlen_t(dom_rows[b][i]));
    }
  }
  // the evaluator: a site's VALUE under the visit-time rename map
  auto resolve_at = [&](uintlen_t v, ulen at) -> uintlen_t {
    uintlen_t steps = 0;
    while (v != opt_ns::none_v && v < nv && res.ren[v] != opt_ns::none_v &&
           def_site[v] != opt_ns::none_v &&
           visit_rank[def_site[v]] < visit_rank[at]) {
      v = res.ren[v];
      if (++steps > nv + 1)
        break;
    }
    return v;
  };
  auto op_key_at = [&](uintlen_t raw, ulen at) {
    // -> (is_const, val): const table first, absent -> const 0
    const uintlen_t r = resolve_at(raw, at);
    if (r != opt_ns::none_v && r < nv && cmask[r])
      return std::pair<bool, uint64_t>(true, uint64_t(cval[r]));
    if (r == opt_ns::none_v)
      return std::pair<bool, uint64_t>(true, 0);
    return std::pair<bool, uint64_t>(false, uint64_t(r));
  };
  using Val = std::vector<uint64_t>; // tag, op/block, then payload
  std::vector<Val> value(ns);
  for (ulen s = 0; s < ns; ++s) {
    const auto &si = sinfo[s];
    Val &w = value[s];
    if (si.tag == opt_ns::vn_const_v) {
      w = {1, uint64_t(si.c)};
    } else if (si.tag == opt_ns::vn_phi_v) {
      w = {2, uint64_t(site_block[s])};
      for (ulen j = 0; j < sargs[s].size(); ++j) {
        auto k = op_key_at(sargs[s][j], s);
        w.push_back((k.first ? (uint64_t(1) << 63) : 0) ^ k.second);
        w.push_back(uint64_t(spreds[s][j]));
      }
    } else if (si.tag == opt_ns::vn_bin_v) {
      w = {3, uint64_t(si.op)};
      auto a = op_key_at(sargs[s][0], s), b = op_key_at(sargs[s][1], s);
      uint64_t ka = (a.first ? (uint64_t(1) << 63) : 0) ^ a.second;
      uint64_t kb = (b.first ? (uint64_t(1) << 63) : 0) ^ b.second;
      if ((si.flags & opt_ns::vn_comm_v) && kb < ka)
        std::swap(ka, kb); // the commutative canonical form
      w.push_back(ka);
      w.push_back(kb);
    }
  }
  // every hit is a true congruence under the visit-time map + domination
  auto dominates_block = [&](ulen d, ulen x) {
    int steps = 0;
    while (x != kz_none() && steps++ <= int(nb)) {
      if (x == d)
        return true;
      x = x == 0 ? kz_none() : C.idom[x]; // idom[0] = 0/none: stop
    }
    return false;
  };
  ulen hits_seen = 0;
  for (ulen s = 0; s < ns; ++s) {
    const auto &si = sinfo[s];
    if (si.tag == opt_ns::vn_skip_v || si.dst == opt_ns::none_v ||
        si.dst >= nv)
      continue;
    const uintlen_t r = res.ren[si.dst];
    if (r != opt_ns::none_v) {
      ++hits_seen;
      R.ok(res.keep[s] == 0, "vn: a hit site is dropped");
      R.ok(r < nv && def_site[r] != opt_ns::none_v,
           "vn: ren targets a defined var");
      if (r < nv && def_site[r] != opt_ns::none_v) {
        const ulen st = def_site[r];
        R.ok(value[s] == value[st],
             "vn: congruence holds under the visit-time rename map");
        R.ok(dominates_block(site_block[st], site_block[s]),
             "vn: the hit target dominates the hitting site");
      }
    } else {
      R.ok(res.keep[s] != 0, "vn: a miss site is kept");
    }
  }
  R.ok(res.hits == hits_seen, "vn: hit count matches keep[]");
}

// ---------------------------------------------------------------------------
// 10. opt.sccp: the two-worklist engine vs chaotic iteration over the
//     same abstract semantics (the least fixpoint is unique -- an
//     independent iteration strategy is a real differential)
// ---------------------------------------------------------------------------
struct KzScOp {
  int kind{}; // 0 const, 1 copy, 2 add, 3 br-all, 4 br-var
  uintlen_t a{}, b{};
  int64_t c{};
};
static void fz_sccp(std::mt19937_64 &rng, KzRec &R) {
  using opt_ns::sccp_branch_v;
  using opt_ns::sccp_cn_v;
  using opt_ns::sccp_od_v;
  using opt_ns::sccp_phi_v;
  using opt_ns::sccp_top_v;
  using opt_ns::sccp_value_v;
  const ulen nb = 2 + kz_pick(rng, 5);
  const ulen nv = 2 + kz_pick(rng, 5);
  std::vector<std::vector<uintlen_t>> succs(nb);
  for (ulen b = 0; b < nb; ++b) {
    const int deg = int(kz_pick(rng, 4));
    for (int j = 0; j < deg; ++j)
      succs[b].push_back(kz_chance(rng, 75) ? kz_pick(rng, nb)
                                            : uintlen_t(opt_ns::none_v));
  }
  std::vector<std::vector<uintlen_t>> preds(nb);
  for (ulen b = 0; b < nb; ++b)
    for (ulen s : succs[b])
      if (s < nb)
        preds[s].push_back(b);
  // one defining site per var (SSA); ops read only vars
  std::vector<uintlen_t> defsite(nv, opt_ns::none_v);
  std::vector<KzScOp> ops{};
  std::vector<opt_ns::sccp_site_t<V>> sites{};
  std::vector<std::vector<uintlen_t>> sob(nb);
  std::vector<uintlen_t> phi_block{}, phi_dst{}, phi_ebegin{0};
  std::vector<uintlen_t> phi_epred{}, phi_earg{};
  std::vector<std::vector<uintlen_t>> uses(nv);
  for (ulen b = 0; b < nb; ++b) {
    const int k = int(kz_pick(rng, 4));
    for (int j = 0; j < k; ++j) {
      const ulen s = ulen(sites.size());
      opt_ns::sccp_site_t<V> si{};
      KzScOp op{};
      const int r = int(kz_pick(rng, 100));
      ulen freevar = opt_ns::none_v;
      for (ulen v = 0; v < nv; ++v)
        if (defsite[v] == opt_ns::none_v) {
          freevar = v;
          break;
        }
      const bool can_def = freevar != opt_ns::none_v && r < 70;
      if (can_def && r < 25) { // const
        si.kind = sccp_value_v;
        si.dst = freevar;
        defsite[freevar] = s;
        op.kind = 0;
        op.c = int64_t(rng() % 101) - 50;
      } else if (can_def && r < 50) { // copy
        si.kind = sccp_value_v;
        si.dst = freevar;
        defsite[freevar] = s;
        op.kind = 1;
        op.a = kz_pick(rng, nv);
        uses[op.a].push_back(s);
      } else if (can_def && r < 70) { // add
        si.kind = sccp_value_v;
        si.dst = freevar;
        defsite[freevar] = s;
        op.kind = 2;
        op.a = kz_pick(rng, nv);
        op.b = kz_pick(rng, nv);
        uses[op.a].push_back(s);
        uses[op.b].push_back(s);
      } else if (r < 85 && freevar != opt_ns::none_v && !preds[b].empty()) {
        // phi (N-ary, duplicate preds included)
        si.kind = sccp_phi_v;
        si.dst = freevar;
        defsite[freevar] = s;
        si.phi = ulen(phi_block.size());
        phi_block.push_back(b);
        phi_dst.push_back(freevar);
        for (ulen p : preds[b]) {
          phi_epred.push_back(p);
          const uintlen_t arg = kz_chance(rng, 75) ? kz_pick(rng, nv)
                                                   : uintlen_t(opt_ns::none_v);
          phi_earg.push_back(arg);
          if (arg != opt_ns::none_v && arg < nv)
            uses[arg].push_back(s);
        }
        phi_ebegin.push_back(ulen(phi_epred.size()));
      } else { // branch
        si.kind = sccp_branch_v;
        if (kz_chance(rng, 50))
          op.kind = 3; // push every valid slot
        else {
          op.kind = 4; // conditional on a var
          op.a = kz_pick(rng, nv);
          uses[op.a].push_back(s);
        }
      }
      ops.push_back(op);
      sites.push_back(si);
      sob[b].push_back(s);
    }
  }
  // initial lattice: all TOP, sometimes one var OVERDEF (a runtime input)
  std::vector<uint8_t> lat0(nv, sccp_top_v);
  std::vector<int64_t> cv0(nv, 0);
  if (nv && kz_chance(rng, 30)) {
    const ulen v = kz_pick(rng, nv);
    lat0[v] = sccp_od_v;
  }
  // ---- the engine ----
  opt_ns::sccp_engine_t<V> E{};
  E.succs = succs;
  E.sites_of_block = sob;
  E.sites = sites;
  E.phi_block = phi_block;
  E.phi_entry_dst = phi_dst;
  E.phi_entry_pred = phi_epred;
  E.phi_entry_arg = phi_earg;
  E.phi_entry_begin = phi_ebegin;
  E.uses = uses;
  E.entry_block = 0;
  E.lat = lat0;
  E.cval = cv0;
  const auto valid_slots = [&](ulen b) {
    std::vector<ulen> vs{};
    for (ulen j = 0; j < succs[b].size(); ++j)
      if (succs[b][j] < nb)
        vs.push_back(j);
    return vs;
  };
  bool ran = E.run(
      [&](uintlen_t s, uintlen_t b) {
        const KzScOp &op = ops[s];
        if (op.kind == 0)
          E.lower(sites[s].dst, sccp_cn_v, op.c);
        else if (op.kind == 1) {
          if (E.lat[op.a] != sccp_top_v)
            E.lower(sites[s].dst, E.lat[op.a], E.cval[op.a]);
        } else if (op.kind == 2) {
          const uint8_t la = E.lat[op.a], lb = E.lat[op.b];
          if (la == sccp_top_v || lb == sccp_top_v)
            return; // wait for operands
          if (la == sccp_od_v || lb == sccp_od_v)
            E.lower(sites[s].dst, sccp_od_v, 0);
          else
            E.lower(sites[s].dst, sccp_cn_v,
                    E.cval[op.a] + E.cval[op.b]); // wrapping int64
        } else if (op.kind == 3) {
          for (ulen j : valid_slots(b))
            E.push_edge(b, j);
        } else if (op.kind == 4) {
          if (E.lat[op.a] == sccp_cn_v) {
            const std::vector<ulen> vs = valid_slots(b);
            if (vs.size() >= 2)
              E.push_edge(b, E.cval[op.a] != 0 ? vs[0] : vs[1]);
          } else if (E.lat[op.a] == sccp_od_v) {
            for (ulen j : valid_slots(b))
              E.push_edge(b, j);
          }
        }
      },
      1000000);
  R.ok(ran, "sccp: the engine terminates under the guard");
  // ---- the reference: chaotic iteration to the (unique) fixpoint ----
  std::vector<uint8_t> lat = lat0;
  std::vector<int64_t> cv = cv0;
  std::vector<ulen> ebase(nb + 1, 0);
  for (ulen b = 0; b < nb; ++b)
    ebase[b + 1] = ebase[b] + succs[b].size();
  std::vector<char> eex(ebase[nb], 0), bex(nb, 0);
  if (nb)
    bex[0] = 1;
  // the reference's lower: the SAME enforced monotone descent (the
  // kernel's contract) -- a meet that computes TOP knows nothing, and
  // OVERDEF is final; without the refusal the two iteration ORDERS
  // (row-order rounds vs the engine's worklists) could each clobber
  // differently and the comparison would be meaningless
  auto lower2 = [&](ulen v, uint8_t tag, int64_t val) {
    if (v >= nv)
      return false;
    if (tag == lat[v] || lat[v] == sccp_od_v || tag == sccp_top_v)
      return false; // unchanged / bottom is final / top knows nothing
    lat[v] = tag;
    cv[v] = val;
    return true;
  };
  auto pred_exec2 = [&](ulen p, ulen home) {
    if (p >= nb)
      return false;
    for (ulen j = 0; j < succs[p].size(); ++j)
      if (succs[p][j] == home && eex[ebase[p] + j])
        return true;
    return false;
  };
  auto mark_edge = [&](ulen b, ulen j) {
    const ulen t = succs[b][j];
    if (t < nb && !bex[t])
      bex[t] = 1;
    eex[ebase[b] + j] = 1;
  };
  for (int round = 0; round < 800; ++round) {
    bool ch = false;
    for (ulen b = 0; b < nb; ++b) {
      if (!bex[b])
        continue;
      for (ulen s : sob[b]) {
        const opt_ns::sccp_site_t<V> &si = sites[s];
        if (si.kind == sccp_phi_v) {
          uint8_t r = sccp_top_v;
          int64_t rc = 0;
          const ulen home = phi_block[si.phi];
          for (ulen e = phi_ebegin[si.phi]; e < phi_ebegin[si.phi + 1]; ++e) {
            if (!pred_exec2(phi_epred[e], home))
              continue;
            const bool ok = phi_earg[e] != opt_ns::none_v &&
                            phi_earg[e] < nv;
            const uint8_t al = ok ? lat[phi_earg[e]] : uint8_t(sccp_od_v);
            const int64_t av = ok ? cv[phi_earg[e]] : 0;
            uint8_t mr{};
            int64_t mc{};
            opt_ns::sccp_meet(r, rc, al, av, mr, mc);
            r = mr;
            rc = mc;
          }
          ch |= lower2(phi_dst[si.phi], r, rc);
        } else if (si.kind == sccp_value_v) {
          const KzScOp &op = ops[s];
          if (op.kind == 0) {
            ch |= lower2(si.dst, sccp_cn_v, op.c);
          } else if (op.kind == 1) {
            if (lat[op.a] != sccp_top_v)
              ch |= lower2(si.dst, lat[op.a], cv[op.a]);
          } else if (op.kind == 2) {
            const uint8_t la = lat[op.a], lb = lat[op.b];
            if (la != sccp_top_v && lb != sccp_top_v) {
              const uint8_t tag =
                  (la == sccp_od_v || lb == sccp_od_v) ? uint8_t(sccp_od_v)
                                                       : uint8_t(sccp_cn_v);
              const int64_t val = cv[op.a] + cv[op.b];
              ch |= lower2(si.dst, tag, val);
            }
          }
        } else { // branch
          const KzScOp &op = ops[s];
          if (op.kind == 3) {
            for (ulen j = 0; j < succs[b].size(); ++j)
              if (succs[b][j] < nb && !eex[ebase[b] + j]) {
                mark_edge(b, j);
                ch = true;
              }
          } else if (op.kind == 4) {
            if (lat[op.a] == sccp_cn_v) {
              const std::vector<ulen> vs = valid_slots(b);
              if (vs.size() >= 2) {
                const ulen j = cv[op.a] != 0 ? vs[0] : vs[1];
                if (!eex[ebase[b] + j]) {
                  mark_edge(b, j);
                  ch = true;
                }
              }
            } else if (lat[op.a] == sccp_od_v) {
              for (ulen j = 0; j < succs[b].size(); ++j)
                if (succs[b][j] < nb && !eex[ebase[b] + j]) {
                  mark_edge(b, j);
                  ch = true;
                }
            }
          }
        }
      }
    }
    if (!ch)
      break;
  }
  for (ulen v = 0; v < nv; ++v)
    R.ok(E.lat[v] == lat[v] && (lat[v] != sccp_cn_v || E.cval[v] == cv[v]),
         "sccp: lattice matches chaotic iteration");
  for (ulen b = 0; b < nb; ++b)
    R.ok(!!E.bexec[b] == !!bex[b], "sccp: block executability matches");
  R.ok(E.eexec.size() == eex.size(), "sccp: edge table size");
  for (ulen e = 0; e < eex.size(); ++e)
    R.ok(!!E.eexec[e] == !!eex[e], "sccp: edge executability matches");
  // soundness spot-check: consts the engine proved must hold on every
  // executable path -- concretely walked
  // (the engine's CN answer is the meet over executable in-edges; the
  // chaotic reference IS that check, so this stays a table comparison)
}

// ---------------------------------------------------------------------------
// 11. opt.pcopy: sequentialization vs parallel-assignment simulation
// ---------------------------------------------------------------------------
static void fz_pcopy(std::mt19937_64 &rng, KzRec &R) {
  const ulen nreg = 3 + kz_pick(rng, 5);
  const int nrows = int(kz_pick(rng, 8));
  // unique dsts among non-self rows (parallel-copy contract); self
  // copies and undef sources ride along
  std::vector<ulen> pool{};
  for (ulen r = 0; r < nreg; ++r)
    pool.push_back(r);
  for (ulen i = 0; i + 1 < pool.size(); ++i) { // shuffle
    const ulen j = i + kz_pick(rng, ulen(pool.size() - i));
    std::swap(pool[i], pool[j]);
  }
  std::vector<std::pair<uintlen_t, uintlen_t>> pcs{};
  ulen next = 0;
  for (int i = 0; i < nrows; ++i) {
    if (next < pool.size()) {
      const ulen d = pool[next++];
      ulen s = kz_pick(rng, nreg);
      if (kz_chance(rng, 15))
        s = uintlen_t(opt_ns::none_v); // undef arg
      pcs.push_back({d, s});
    } else if (kz_pick(rng, nreg) < nreg) {
      const ulen d = kz_pick(rng, nreg); // a self-copy (no-op)
      pcs.push_back({d, d});
    }
  }
  // per-register concrete values, the parallel semantics, the temps
  std::vector<int64_t> rval(6000, 0);
  for (ulen r = 0; r < nreg; ++r)
    rval[r] = int64_t(rng() % 1000);
  std::vector<int64_t> want(6000, 0);
  for (ulen r = 0; r < nreg; ++r)
    want[r] = rval[r];
  ulen want_self = 0, want_zeros = 0;
  std::vector<ulen> kept_dsts{};
  for (const auto &p : pcs) {
    if (p.first == p.second && p.second != opt_ns::none_v) {
      ++want_self;
      continue; // no-op
    }
    if (p.second == opt_ns::none_v) {
      ++want_zeros;
      want[p.first] = 0;
    } else {
      want[p.first] = rval[p.second];
    }
    kept_dsts.push_back(p.first);
  }
  // cycles among kept rows: dst -> src over the kept set
  ulen want_breaks = 0;
  {
    std::vector<char> is_kept(6000, 0), seen(6000, 0);
    for (ulen d : kept_dsts)
      is_kept[d] = 1;
    for (ulen d : kept_dsts) {
      if (seen[d])
        continue;
      ulen x = d;
      std::vector<ulen> stack{};
      while (true) {
        if (seen[x]) {
          if (std::find(stack.begin(), stack.end(), x) != stack.end())
            ++want_breaks; // closed a fresh cycle
          break;
        }
        seen[x] = 1;
        stack.push_back(x);
        // the src of row x (kept rows have unique dsts)
        ulen src = opt_ns::none_v;
        for (const auto &p : pcs)
          if (p.first == x && !(p.first == p.second &&
                                p.second != opt_ns::none_v))
            src = p.second;
        if (src == opt_ns::none_v || src >= 6000 || !is_kept[src])
          break;
        x = src;
      }
    }
  }
  uintlen_t ntemp = 0;
  opt_ns::pcopy_stats_t st{};
  auto ops = opt_ns::sequentialize_pcopy(
      pcs, [&](uintlen_t) { return uintlen_t(5000 + ntemp++); }, st);
  R.ok(st.self_copies == want_self, "pcopy: self-copies stripped");
  R.ok(st.zeros == want_zeros, "pcopy: undef args materialize zero");
  R.ok(st.cycles_broken == want_breaks,
       "pcopy: one temp per permutation cycle");
  R.ok(ops.size() == st.copies + st.zeros, "pcopy: ops = copies + zeros");
  // simulate the sequence; no register written twice
  std::vector<int64_t> got(rval);
  std::vector<ulen> writes(6000, 0);
  for (const auto &op : ops) {
    R.ok(op.dst < 6000, "pcopy: dst in range");
    if (op.dst >= 6000)
      continue;
    ++writes[op.dst];
    got[op.dst] = op.zero ? 0 : (op.src == opt_ns::none_v ? 0 : got[op.src]);
  }
  for (ulen d : kept_dsts) {
    R.ok(writes[d] == 1, "pcopy: every dst written exactly once");
    R.ok(got[d] == want[d], "pcopy: the sequence computes the parallel set");
  }
  for (ulen t = 0; t < ntemp; ++t)
    R.ok(writes[5000 + t] == 1, "pcopy: every temp written exactly once");
}

// ---------------------------------------------------------------------------
// 12. opt.split: the critical-edge plan vs the spec (none/junk slots,
//     the both-arms-one-target rule, enumeration order, fresh ids)
// ---------------------------------------------------------------------------
static void fz_split(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 1 + kz_pick(rng, 7);
  std::vector<std::vector<uintlen_t>> rows(nb);
  for (ulen b = 0; b < nb; ++b) {
    const int deg = int(kz_pick(rng, 4));
    for (int j = 0; j < deg; ++j) {
      const int r = int(kz_pick(rng, 100));
      if (r < 70)
        rows[b].push_back(kz_pick(rng, nb));
      else if (r < 90)
        rows[b].push_back(uintlen_t(opt_ns::none_v));
      else
        rows[b].push_back(nb + 1 + kz_pick(rng, 2)); // junk target
    }
  }
  std::vector<char> has_phi(nb, 0);
  for (ulen b = 0; b < nb; ++b)
    has_phi[b] = kz_chance(rng, 40) ? 1 : 0;
  auto plan = opt_ns::calculate_edge_splits(rows, has_phi);
  std::vector<opt_ns::edge_split_t> want{};
  for (ulen p = 0; p < nb; ++p) {
    const auto &row = rows[p];
    ulen valid = 0, distinct = 0, first = opt_ns::none_v;
    for (const auto &t : row)
      if (t != opt_ns::none_v) {
        ++valid;
        if (first == opt_ns::none_v) {
          first = t;
          distinct = 1;
        } else if (t != first)
          distinct = 2;
      }
    if (valid < 2 || distinct < 2)
      continue;
    for (ulen j = 0; j < row.size(); ++j) {
      const ulen t = row[j];
      if (t == opt_ns::none_v || t >= nb)
        continue;
      if (!has_phi[t])
        continue;
      want.push_back({p, j, t, nb + ulen(want.size())});
    }
  }
  R.ok(plan.size() == want.size(), "split: plan size matches");
  for (size_t i = 0; i < plan.size() && i < want.size(); ++i) {
    R.ok(plan[i].pred == want[i].pred && plan[i].slot == want[i].slot &&
             plan[i].target == want[i].target && plan[i].fresh == want[i].fresh,
         "split: (pred, slot, target, fresh) in enumeration order");
  }
  // invariant: every split pred really fans out to >= 2 distinct targets
  for (const auto &sp : plan) {
    std::vector<char> seen(nb + 4, 0);
    ulen distinct = 0;
    for (const auto &t : rows[sp.pred])
      if (t != opt_ns::none_v && t < seen.size() && !seen[t]) {
        seen[t] = 1;
        ++distinct;
      }
    R.ok(distinct >= 2 && sp.target < nb && has_phi[sp.target],
         "split: only 2+-target preds with phiful targets split");
  }
}

// ---------------------------------------------------------------------------
// 14. opt.reach: the flood vs the reference BFS (none slots, junk entry)
// ---------------------------------------------------------------------------
static void fz_reach(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 1 + kz_pick(rng, 8);
  std::vector<std::vector<uintlen_t>> succs(nb);
  for (ulen b = 0; b < nb; ++b) {
    const int deg = int(kz_pick(rng, 4));
    for (int j = 0; j < deg; ++j) {
      if (kz_chance(rng, 75))
        succs[b].push_back(kz_pick(rng, nb));
      else
        succs[b].push_back(uintlen_t(opt_ns::none_v));
    }
  }
  const ulen entry = kz_pick(rng, nb + 1); // sometimes junk
  auto reach = opt_ns::calculate_reachable_mask(succs, entry);
  auto rreach = kz_ref_reach(succs, entry);
  for (ulen b = 0; b < nb; ++b)
    R.ok(!!reach[b] == !!rreach[b], "reach: flood matches the BFS");
}

// ---------------------------------------------------------------------------
// 15. opt.splice: the pairing sweep vs its greedy replica, plus
//     reachability preservation through the merge
// ---------------------------------------------------------------------------
static void fz_splice(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 1 + kz_pick(rng, 8);
  std::vector<std::vector<uintlen_t>> succs(nb); // valid rows only
  for (ulen b = 0; b < nb; ++b) {
    const int deg = int(kz_pick(rng, 4));
    for (int j = 0; j < deg; ++j)
      if (kz_chance(rng, 75))
        succs[b].push_back(kz_pick(rng, nb));
  }
  std::vector<std::vector<uintlen_t>> preds(nb);
  for (ulen b = 0; b < nb; ++b)
    for (ulen s : succs[b])
      preds[s].push_back(b);
  std::vector<char> has_phi(nb, 0);
  for (ulen b = 0; b < nb; ++b)
    has_phi[b] = kz_chance(rng, 30) ? 1 : 0;
  const ulen ent = kz_pick(rng, nb);
  auto plan = opt_ns::calculate_splice_plan(succs, preds, ent, has_phi);
  // replica of the greedy (same rekeying discipline)
  std::vector<ulen> wabs(nb, opt_ns::none_v), wmer(nb, opt_ns::none_v);
  auto preds2 = preds;
  for (ulen b = 0; b < nb; ++b) {
    if (wabs[b] != opt_ns::none_v || wmer[b] != opt_ns::none_v)
      continue;
    if (succs[b].size() != 1)
      continue;
    const ulen s = succs[b][0];
    if (s == b || s == ent || s >= nb)
      continue;
    if (wabs[s] != opt_ns::none_v || wmer[s] != opt_ns::none_v)
      continue;
    if (preds2[s].size() != 1 || preds2[s][0] != b)
      continue;
    if (has_phi[s])
      continue;
    wabs[s] = b;
    wmer[b] = s;
    for (const auto &t : succs[s]) {
      if (t >= nb)
        continue;
      for (ulen &p : preds2[t])
        if (p == s)
          p = b;
    }
  }
  R.ok(plan.absorbed_of == wabs && plan.merges_of == wmer,
       "splice: the pairing sweep matches its replica");
  // invariants + reachability preservation through the merge. THE
  // MERGE SEMANTICS (the pairing contract): the absorber b ENDS with
  // the absorbed block's terminator -- b's successor row after the
  // merge is s's row (retargeted through any absorption), not b's own
  // (b's own row was the single edge b->s, which the merge deletes)
  std::vector<char> reach0 = kz_ref_reach(succs, ent);
  std::vector<std::vector<uintlen_t>> nsuccs(nb);
  for (ulen b = 0; b < nb; ++b) {
    if (wabs[b] != opt_ns::none_v)
      continue; // absorbed blocks disappear
    const auto &row = (wmer[b] != opt_ns::none_v) ? succs[wmer[b]] : succs[b];
    for (ulen s : row) {
      if (s >= nb)
        continue;
      nsuccs[b].push_back(wabs[s] != opt_ns::none_v ? wabs[s] : s);
    }
  }
  std::vector<char> reach1 = kz_ref_reach(nsuccs, ent);
  for (ulen b = 0; b < nb; ++b) {
    if (wabs[b] != opt_ns::none_v) {
      R.ok(wmer[wabs[b]] == b, "splice: absorbed/merges are inverse");
      R.ok(!reach1[b], "splice: absorbed blocks leave the graph");
      continue;
    }
    R.ok(!!reach1[b] == !!reach0[b],
         "splice: reachability preserved for survivors");
    if (wmer[b] != opt_ns::none_v) {
      R.ok(wabs[wmer[b]] == b, "splice: the pair agrees both ways");
      R.ok(succs[b].size() == 1 && succs[b][0] == wmer[b] &&
               has_phi[wmer[b]] == 0 && wmer[b] != ent && wmer[b] != b,
           "splice: every pair meets the merge conditions");
    }
  }
}

// ---------------------------------------------------------------------------
// 15. opt.inv: the unique-outside-pred scan + the first-invariant-site
//     selection vs references (real DFS intervals from a random dom tree)
// ---------------------------------------------------------------------------
static void fz_inv(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 2 + kz_pick(rng, 7);
  // a random dom tree over 0..nb-1 (parents strictly smaller), real
  // DFS intervals -- begin 0 stays the unreachable/invalid marker
  std::vector<ulen> par(nb, opt_ns::none_v);
  std::vector<std::vector<ulen>> kids(nb);
  for (ulen b = 1; b < nb; ++b) {
    par[b] = kz_pick(rng, b);
    kids[par[b]].push_back(b);
  }
  std::vector<ulen> db(nb, 0), de(nb, 0);
  {
    ulen t = 0;
    std::function<void(ulen)> dfs = [&](ulen b) {
      db[b] = ++t;
      for (ulen c : kids[b])
        dfs(c);
      de[b] = t;
    };
    dfs(0);
    if (kz_chance(rng, 20) && nb > 1) { // an invalid marker is legal input
      const ulen b = 1 + kz_pick(rng, nb - 1);
      db[b] = 0;
      de[b] = 0;
    }
  }
  auto dominates = [&](ulen d, ulen x) {
    if (d >= nb || x >= nb)
      return false;
    if (db[d] == 0 || db[x] == 0)
      return false;
    return db[d] <= db[x] && de[d] >= de[x];
  };
  // ---- find_unique_outside_pred ----
  std::vector<char> in_loop(nb, 0);
  for (ulen b = 0; b < nb; ++b)
    in_loop[b] = kz_chance(rng, 45) ? 1 : 0;
  const ulen header = kz_pick(rng, nb);
  std::vector<uintlen_t> hpreds{};
  {
    const int k = int(kz_pick(rng, 6));
    for (int j = 0; j < k; ++j)
      hpreds.push_back(kz_pick(rng, nb)); // valid ids only (contract)
  }
  auto [pre, nout] = opt_ns::find_unique_outside_pred(hpreds, in_loop);
  {
    ulen wpre = opt_ns::none_v, wout = 0;
    for (ulen p : hpreds)
      if (!in_loop[p]) {
        wpre = p;
        ++wout;
      }
    R.ok(pre == wpre && nout == wout,
         "inv: unique-outside-pred scan matches");
  }
  // ---- calculate_first_invariant_site ----
  const ulen nv = 1 + kz_pick(rng, 6);
  std::vector<uintlen_t> def_block(nv, opt_ns::none_v);
  std::vector<char> cmask(nv, 0);
  for (ulen v = 0; v < nv; ++v) {
    if (kz_chance(rng, 80))
      def_block[v] = kz_pick(rng, nb);
    cmask[v] = kz_chance(rng, 30) ? 1 : 0;
  }
  const ulen ns = kz_pick(rng, 8);
  std::vector<std::vector<uintlen_t>> sob(nb), sargs(ns);
  std::vector<char> elig(ns, 0);
  for (ulen s = 0; s < ns; ++s) {
    const ulen b = kz_pick(rng, nb);
    sob[b].push_back(s);
    elig[s] = kz_chance(rng, 70) ? 1 : 0;
    const int k = int(kz_pick(rng, 3));
    for (int j = 0; j < k; ++j) {
      const int r = int(kz_pick(rng, 100));
      if (r < 80)
        sargs[s].push_back(kz_pick(rng, nv));
      else if (r < 90)
        sargs[s].push_back(opt_ns::none_v);
      else
        sargs[s].push_back(nv + kz_pick(rng, 3)); // junk: never invariant
    }
  }
  std::vector<uintlen_t> loop_row{};
  for (ulen b = 0; b < nb; ++b)
    if (in_loop[b] && b != header && kz_chance(rng, 80))
      loop_row.push_back(b);
  const ulen preh = kz_pick(rng, nb);
  auto pick = opt_ns::calculate_first_invariant_site<V>(
      loop_row, header, preh, in_loop, def_block, cmask, db, de, sob, sargs,
      elig);
  // reference: the header first, then the member row; eligible sites in
  // row order; invariant = absent / const / outside-def-dominating-pre
  auto inv_var = [&](ulen v) {
    if (v == opt_ns::none_v)
      return true;
    if (v >= nv)
      return false;
    const ulen d = def_block[v];
    if (d == opt_ns::none_v)
      return false;
    if (cmask[v])
      return true;
    if (d < in_loop.size() && in_loop[d])
      return false;
    return dominates(d, preh);
  };
  std::pair<uintlen_t, uintlen_t> want{opt_ns::none_v, opt_ns::none_v};
  for (ulen bi = 0; bi <= loop_row.size() && want.first == opt_ns::none_v;
       ++bi) {
    const ulen B = bi == 0 ? header : loop_row[bi - 1];
    if (B >= nb)
      continue;
    for (ulen s : sob[B]) {
      if (!elig[s])
        continue;
      bool ok = true;
      for (ulen v : sargs[s])
        ok &= inv_var(v);
      if (ok) {
        want = {B, s};
        break;
      }
    }
  }
  R.ok(pick.first == want.first && pick.second == want.second,
       "inv: the first hoistable site matches the reference scan");
}

// ---------------------------------------------------------------------------
// 16. opt.ren: the copy-rename builder vs its reference (+ no self-loops)
// ---------------------------------------------------------------------------
static void fz_ren(std::mt19937_64 &rng, KzRec &R) {
  const ulen nv = 1 + kz_pick(rng, 8);
  std::vector<std::pair<uintlen_t, uintlen_t>> pairs{};
  const int k = int(kz_pick(rng, 9));
  for (int j = 0; j < k; ++j) {
    ulen d = kz_pick(rng, nv), s = kz_pick(rng, nv);
    if (kz_chance(rng, 15))
      s = d; // a self pair: a no-op either way
    pairs.push_back({d, s});
  }
  auto ren = opt_ns::build_copy_rename(nv, pairs);
  std::vector<ulen> want(nv, opt_ns::none_v);
  auto resolve = [&](ulen v) {
    ulen steps = 0;
    while (v != opt_ns::none_v && v < nv && want[v] != opt_ns::none_v) {
      v = want[v];
      if (++steps > nv)
        break;
    }
    return v;
  };
  for (const auto &pr : pairs) {
    const ulen d = pr.first, s = pr.second;
    if (d >= nv || s >= nv)
      continue;
    const ulen r = resolve(s);
    if (r != d)
      want[d] = r; // r == d: the crossed closing pair stays unrecorded
  }
  R.ok(ren == want, "ren: the rename map matches the reference");
  for (ulen v = 0; v < nv; ++v) {
    R.ok(ren[v] != v, "ren: no self-loop entries");
    R.ok(opt_ns::resolve_capped(ren, v) == resolve(v),
         "ren: the capped resolve agrees");
  }
  R.ok(opt_ns::resolve_capped(ren, opt_ns::none_v) == opt_ns::none_v,
       "ren: none passes through the capped resolve");
}

// ---------------------------------------------------------------------------
// 17. opt.trivphi: the trivial-phi pick (N-ary, absent/self args)
// ---------------------------------------------------------------------------
static void fz_trivphi(std::mt19937_64 &rng, KzRec &R) {
  const ulen dst = kz_pick(rng, 8);
  std::vector<uintlen_t> args{};
  const int k = int(kz_pick(rng, 6));
  for (int j = 0; j < k; ++j) {
    const int r = int(kz_pick(rng, 100));
    if (r < 60)
      args.push_back(kz_pick(rng, 8));
    else if (r < 85)
      args.push_back(opt_ns::none_v);
    else
      args.push_back(dst); // self
  }
  const ulen got = opt_ns::calculate_trivial_phi_pick(dst, args);
  ulen want = opt_ns::none_v;
  for (ulen a : args) {
    if (a == opt_ns::none_v || a == dst)
      continue;
    if (want == opt_ns::none_v)
      want = a;
    else if (want != a) {
      want = opt_ns::none_v;
      break;
    }
  }
  R.ok(got == want, "trivphi: unique non-self arg or none");
}

// ---------------------------------------------------------------------------
// 18. opt.shape: header-pred classification + the test-first loop shape
// ---------------------------------------------------------------------------
static void fz_shape(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 2 + kz_pick(rng, 6);
  std::vector<char> inL(nb, 0);
  for (ulen b = 0; b < nb; ++b)
    inL[b] = kz_chance(rng, 50) ? 1 : 0;
  std::vector<uintlen_t> preds{};
  {
    const int k = int(kz_pick(rng, 5));
    for (int j = 0; j < k; ++j) {
      if (kz_chance(rng, 90))
        preds.push_back(kz_pick(rng, nb));
      else
        preds.push_back(nb + kz_pick(rng, 3)); // junk: outside by the guard
    }
  }
  auto hp = opt_ns::calculate_header_preds(preds, inL);
  {
    ulen lin = 0, lout = 0, wlatch = opt_ns::none_v, wpre = opt_ns::none_v;
    for (ulen p : preds) {
      if (p < inL.size() && inL[p]) {
        wlatch = p;
        ++lin;
      } else {
        wpre = p;
        ++lout;
      }
    }
    R.ok(hp.n_in == lin && hp.n_out == lout && hp.latch == wlatch &&
             hp.pre == wpre,
         "shape: header preds (last in = latch, last out = pre)");
  }
  const auto arm = [&]() -> uintlen_t {
    const int r = int(kz_pick(rng, 100));
    if (r < 20)
      return uintlen_t(opt_ns::none_v);
    if (r < 30)
      return nb + kz_pick(rng, 2); // junk arm: invalid
    return kz_pick(rng, nb);
  };
  const uintlen_t a0 = arm(), a1 = arm();
  auto sh = opt_ns::calculate_loop_shape(preds, inL, a0, a1);
  {
    // reference: the unique-pred test gates pre/latch (they are set
    // the moment the header has exactly one in and one out pred), the
    // arm test then gates ok -- a header whose arms fail still keeps
    // its pre/latch, only ok drops (the kernel's contract: clients
    // that only read pre/latch when ok may rely on either, but the
    // kernel states them as soon as the pred test passes)
    const bool okp = hp.n_in == 1 && hp.n_out == 1;
    bool ok = okp;
    uintlen_t wb = opt_ns::none_v, wx = opt_ns::none_v;
    if (ok) {
      const bool v0 = a0 < nb, v1 = a1 < nb;
      const bool i0 = v0 && inL[a0], i1 = v1 && inL[a1];
      if (i0 && !i1 && v1) {
        wb = a0;
        wx = a1;
      } else if (i1 && !i0 && v0) {
        wb = a1;
        wx = a0;
      } else
        ok = false;
    }
    R.ok(sh.ok == ok &&
             sh.pre == (okp ? hp.pre : opt_ns::none_v) &&
             sh.latch == (okp ? hp.latch : opt_ns::none_v) &&
             sh.body == wb && sh.X == wx,
         "shape: the test-first classification matches");
  }
}

// ---------------------------------------------------------------------------
// 19. opt.exits: the exit-edge inventory (skip / forbid / degenerate)
// ---------------------------------------------------------------------------
static void fz_exits(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 2 + kz_pick(rng, 6);
  std::vector<char> inL(nb, 0);
  for (ulen b = 0; b < nb; ++b)
    inL[b] = kz_chance(rng, 50) ? 1 : 0;
  std::vector<std::vector<uintlen_t>> succs(nb);
  for (ulen b = 0; b < nb; ++b) {
    const int deg = int(kz_pick(rng, 4));
    for (int j = 0; j < deg; ++j) {
      const int r = int(kz_pick(rng, 100));
      if (r < 70)
        succs[b].push_back(kz_pick(rng, nb));
      else if (r < 90)
        succs[b].push_back(uintlen_t(opt_ns::none_v));
      else
        succs[b].push_back(nb + 1 + kz_pick(rng, 2)); // junk
    }
  }
  const uintlen_t sf = kz_chance(rng, 60) ? kz_pick(rng, nb)
                                          : uintlen_t(opt_ns::none_v);
  const uintlen_t st = kz_chance(rng, 60) ? kz_pick(rng, nb)
                                          : uintlen_t(opt_ns::none_v);
  const uintlen_t f0 = kz_chance(rng, 40) ? kz_pick(rng, nb)
                                          : uintlen_t(opt_ns::none_v);
  const uintlen_t f1 = kz_chance(rng, 40) ? kz_pick(rng, nb)
                                          : uintlen_t(opt_ns::none_v);
  std::vector<opt_ns::exit_edge_t> ex{};
  const bool ran = opt_ns::collect_exit_edges(succs, inL, sf, st, f0, f1, ex);
  // reference
  std::vector<opt_ns::exit_edge_t> want{};
  bool wran = true;
  for (ulen b = 0; b < nb && wran; ++b) {
    if (!(b < inL.size() && inL[b]))
      continue;
    const auto &row = succs[b];
    ulen valid = 0, distinct = 0, first = opt_ns::none_v;
    for (const auto &s : row) {
      if (s == opt_ns::none_v || s >= nb)
        continue;
      ++valid;
      if (first == opt_ns::none_v) {
        first = s;
        distinct = 1;
      } else if (s != first)
        distinct = 2;
    }
    if (valid >= 2 && distinct == 1 && first < inL.size() && !inL[first]) {
      wran = false; // both arms on ONE outside target: degenerate
      break;
    }
    for (const auto &s : row) {
      if (s == opt_ns::none_v || s >= nb || inL[s])
        continue;
      if (b == sf && s == st)
        continue;
      want.push_back({b, s});
      if (s == f0 || s == f1) {
        wran = false;
        break;
      }
    }
  }
  R.ok(ran == wran, "exits: the accept/bail decision matches");
  if (wran) {
    R.ok(ex.size() == want.size(), "exits: inventory size");
    for (size_t i = 0; i < ex.size() && i < want.size(); ++i)
      R.ok(ex[i].from == want[i].from && ex[i].to == want[i].to,
           "exits: (from, to) in slot order");
  }
}

// ---------------------------------------------------------------------------
// 20. opt.route: the supplier-routing fixpoint vs its replica, plus the
//     born-site contracts (arity bound, distinct routes, final proutes)
// ---------------------------------------------------------------------------
static void fz_route(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 2 + kz_pick(rng, 7);
  std::vector<std::vector<uintlen_t>> preds(nb);
  for (ulen b = 0; b < nb; ++b) {
    const int k = int(kz_pick(rng, 5));
    for (int j = 0; j < k; ++j)
      preds[b].push_back(kz_pick(rng, nb)); // duplicates allowed
  }
  std::vector<char> dead(nb, 0), dom0(nb, 0), dom1(nb, 0);
  for (ulen b = 0; b < nb; ++b) {
    dead[b] = kz_chance(rng, 30) ? 1 : 0;
    dom0[b] = kz_chance(rng, 30) ? 1 : 0;
    dom1[b] = kz_chance(rng, 30) ? 1 : 0; // overlaps are legal input
  }
  const ulen bound = 2 + kz_pick(rng, 3);
  auto P = opt_ns::calculate_supplier_routes(preds, dead, dom0, dom1, bound);
  // the replica (same round structure, independently coded)
  std::vector<ulen> route(nb, opt_ns::route_pending_v);
  std::vector<opt_ns::route_site_t> sites{};
  std::vector<char> born_here(nb, 0);
  for (ulen b = 0; b < nb; ++b) {
    if (dead[b]) {
      route[b] = opt_ns::route_dead_v;
      continue;
    }
    if (dom0[b] && !dom1[b])
      route[b] = 0;
    else if (dom1[b] && !dom0[b])
      route[b] = 1;
  }
  auto step = [&](ulen b) {
    if (route[b] != opt_ns::route_pending_v)
      return false;
    const auto &ps = preds[b];
    bool any_pending = false, any_dead = false;
    for (ulen p : ps) {
      if (p >= nb || route[p] == opt_ns::route_dead_v)
        any_dead = true;
      else if (route[p] == opt_ns::route_pending_v)
        any_pending = true;
    }
    if (any_pending)
      return false;
    if (ps.empty() || any_dead || ps.size() > bound) {
      route[b] = opt_ns::route_dead_v;
      return true;
    }
    bool all_same = true;
    for (ulen k2 = 1; k2 < ps.size(); ++k2)
      all_same &= route[ps[0]] == route[ps[k2]];
    if (ps.size() == 1 || all_same) {
      route[b] = route[ps[0]];
      return true;
    }
    opt_ns::route_site_t S{};
    S.W = b;
    for (ulen p : ps) {
      S.preds.push_back(p);
      S.proutes.push_back(route[p]);
    }
    route[b] = opt_ns::route_born_base_v + ulen(sites.size());
    born_here[b] = 1; // THIS block minted the site (ids propagate!)
    sites.push_back(std::move(S));
    return true;
  };
  for (ulen round = 0; round <= nb; ++round) {
    bool prog = false;
    for (ulen b = 0; b < nb; ++b)
      prog |= step(b);
    if (!prog)
      break;
  }
  for (ulen b = 0; b < nb; ++b)
    if (route[b] == opt_ns::route_pending_v)
      route[b] = opt_ns::route_dead_v;
  R.ok(P.route == route, "route: the fixpoint matches its replica");
  R.ok(P.sites.size() == sites.size(), "route: born-site count matches");
  for (size_t i = 0; i < P.sites.size(); ++i) {
    R.ok(P.sites[i].W == sites[i].W && P.sites[i].preds == sites[i].preds &&
             P.sites[i].proutes == sites[i].proutes,
         "route: born site (block, preds, proutes) in creation order");
  }
  // contracts: dead stays dead, single-source masks read outright, born
  // sites sit under the arity bound with distinct incoming routes, and
  // proutes are the FINAL routes of their preds
  for (ulen b = 0; b < nb; ++b) {
    if (dead[b]) {
      R.ok(P.route[b] == opt_ns::route_dead_v, "route: dead blocks stay dead");
      continue;
    }
    if (dom0[b] && !dom1[b])
      R.ok(P.route[b] == 0, "route: a 0-dominated block reads source 0");
    if (dom1[b] && !dom0[b])
      R.ok(P.route[b] == 1, "route: a 1-dominated block reads source 1");
    if (P.route[b] >= opt_ns::route_born_base_v &&
        P.route[b] != opt_ns::route_dead_v) {
      const ulen k = P.route[b] - opt_ns::route_born_base_v;
      // a born id names the block that MINTED it; dominated blocks
      // INHERIT the id (the single-pred / all-same copy), so W==b is
      // only claimed where the replica itself minted the site
      R.ok(k < P.sites.size(), "route: born ids index the site table");
      if (born_here[b])
        R.ok(k < P.sites.size() && P.sites[k].W == b,
             "route: a born route names its own block");
      if (k < P.sites.size()) {
        R.ok(P.sites[k].preds.size() <= bound,
             "route: born phis respect the arity bound");
        bool distinct = false;
        for (ulen x : P.sites[k].proutes)
          for (ulen y : P.sites[k].proutes)
            distinct |= (x != y);
        R.ok(distinct, "route: born phis merge distinct incoming routes");
        for (size_t j = 0; j < P.sites[k].preds.size(); ++j) {
          const ulen p = P.sites[k].preds[j];
          R.ok(p < nb && P.sites[k].proutes[j] == P.route[p],
               "route: proutes are the preds' final routes");
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 21. opt.needs: the need closure vs its replica, over SYNTHETIC born
//     sites. CONTRACT NOTE (found by this very fuzzer): the closure's
//     DFS terminates because a routing plan's born sites are born in
//     dependency order -- a site's proutes only ever name EARLIER sites
//     (a pred is final before the join it feeds is born), so the
//     proute graph is a DAG. Synthetic sites below respect that shape
//     (proutes point strictly backwards); junk route ids ride along,
//     which the DFS must bail on harmlessly, exactly like the kernel.
// ---------------------------------------------------------------------------
static void fz_needs(std::mt19937_64 &rng, KzRec &R) {
  const ulen nsite = kz_pick(rng, 6), nb = 1 + kz_pick(rng, 6);
  std::vector<opt_ns::route_site_t> sites(nsite);
  for (ulen k = 0; k < nsite; ++k) {
    sites[k].W = kz_pick(rng, nb);
    const int deg = int(kz_pick(rng, 4));
    for (int j = 0; j < deg; ++j) {
      sites[k].preds.push_back(kz_pick(rng, nb));
      const int r = int(kz_pick(rng, 100));
      if (r < 40)
        sites[k].proutes.push_back(kz_pick(rng, 2)); // a source
      else if (r < 85 && k > 0)
        // an EARLIER born site (the DAG discipline; k == 0 has none)
        sites[k].proutes.push_back(opt_ns::route_born_base_v +
                                   kz_pick(rng, k));
      else if (r < 92)
        // out-of-range site id: the DFS bails on it harmlessly
        sites[k].proutes.push_back(opt_ns::route_born_base_v + nsite +
                                   kz_pick(rng, 3));
      else
        // SENTINEL junk (none_v / pending_v): also bails harmlessly.
        // NOT small in-range ids -- 2/3 would name sites 0/1, and a
        // proute of site 0/1 naming site 0/1 breaks the DAG discipline
        // the kernel documents as its termination contract (found the
        // hard way: seed 26's self-cycle ran the DFS out of memory)
        sites[k].proutes.push_back(kz_chance(rng, 50)
                                       ? uintlen_t(opt_ns::none_v)
                                       : uintlen_t(opt_ns::route_pending_v));
    }
  }
  std::vector<std::pair<uintlen_t, uintlen_t>> reads{};
  const int rk = int(kz_pick(rng, 5));
  for (int j = 0; j < rk; ++j) {
    const int r = int(kz_pick(rng, 100));
    uintlen_t rr;
    if (r < 30)
      rr = kz_pick(rng, 2);
    else if (r < 85)
      rr = opt_ns::route_born_base_v + kz_pick(rng, nsite);
    else
      rr = opt_ns::route_born_base_v + nsite + kz_pick(rng, 3); // junk
    reads.push_back({rr, kz_pick(rng, 6)});
  }
  std::vector<std::vector<char>> need{};
  std::vector<std::vector<ulen>> bvals{};
  opt_ns::calculate_route_needs(2, 8, sites, reads, need, bvals);
  // the replica: the same stack DFS, independently coded
  std::vector<std::vector<char>> wneed(2, std::vector<char>(8, 0));
  std::vector<std::vector<ulen>> wbvals(sites.size());
  for (const auto &rd : reads) {
    std::vector<ulen> st{rd.first};
    while (!st.empty()) {
      const ulen x = st.back();
      st.pop_back();
      if (x < 2) {
        wneed[x][rd.second] = 1;
        continue;
      }
      const ulen k = x - opt_ns::route_born_base_v;
      if (k >= sites.size())
        continue;
      bool have = false;
      for (ulen v : wbvals[k])
        have |= (v == rd.second);
      if (!have)
        wbvals[k].push_back(rd.second);
      for (ulen pr : sites[k].proutes)
        st.push_back(pr);
    }
  }
  R.ok(need == wneed, "needs: the closure matches its replica");
  R.ok(bvals == wbvals, "needs: born-site values in first-appearance order");
}

// ---------------------------------------------------------------------------
// 22. opt.alias: the copy/phi identity fixpoint vs a same-cap replica
// ---------------------------------------------------------------------------
static void fz_alias(std::mt19937_64 &rng, KzRec &R) {
  const ulen nv = 1 + kz_pick(rng, 9);
  std::vector<uintlen_t> single(nv, opt_ns::none_v);
  for (ulen v = 0; v < nv; ++v)
    if (kz_chance(rng, 30))
      single[v] = 5 + kz_pick(rng, 4); // pre-seeded allocations
  std::vector<opt_ns::alias_def_t> defs{};
  const int k = int(kz_pick(rng, 8));
  for (int j = 0; j < k; ++j) {
    opt_ns::alias_def_t d{};
    d.var = kz_pick(rng, nv);
    const int r = int(kz_pick(rng, 100));
    if (r < 40) {
      d.kind = opt_ns::alias_copy_v;
      d.src = kz_pick(rng, nv);
    } else if (r < 85) {
      d.kind = opt_ns::alias_phi_v;
      const ulen a = 1 + kz_pick(rng, 3); // N-ary, arity 1..3
      for (ulen j2 = 0; j2 < a; ++j2)
        d.args.push_back(kz_chance(rng, 85) ? kz_pick(rng, nv)
                                            : uintlen_t(opt_ns::none_v));
    } else {
      d.kind = opt_ns::alias_other_v;
    }
    defs.push_back(d);
  }
  const ulen cap = 1 + kz_pick(rng, 8); // sometimes shorter than the chain
  auto want = single;
  bool grew = true;
  for (ulen g = 0; grew && g < cap; ++g) {
    grew = false;
    for (const auto &d : defs) {
      if (d.var >= nv)
        continue;
      if (d.kind == opt_ns::alias_copy_v) {
        if (d.src < nv && want[d.var] != want[d.src]) {
          want[d.var] = want[d.src];
          grew = true;
        }
      } else if (d.kind == opt_ns::alias_phi_v) {
        bool ok = !d.args.empty();
        uintlen_t w = opt_ns::none_v;
        for (size_t j2 = 0; j2 < d.args.size() && ok; ++j2) {
          const ulen a = d.args[j2];
          if (a >= nv || want[a] == opt_ns::none_v) {
            ok = false;
            break;
          }
          if (j2 == 0)
            w = want[a];
          else
            ok &= (want[a] == w);
        }
        w = ok ? w : uintlen_t(opt_ns::none_v);
        if (want[d.var] != w) {
          want[d.var] = w;
          grew = true;
        }
      }
    }
  }
  opt_ns::resolve_single_ids<V>(defs, single, nv, cap);
  R.ok(single == want, "alias: the fixpoint matches the same-cap replica");
}

// ---------------------------------------------------------------------------
// 23. opt.mem: the dominator-scoped table vs the dom-path FOLD (a
//     direct semantic definition -- no watermarks, no seq tricks)
// ---------------------------------------------------------------------------
static void fz_mem(std::mt19937_64 &rng, KzRec &R) {
  const ulen nb = 1 + kz_pick(rng, 7);
  // a random dom tree (root 0, parents smaller)
  std::vector<std::vector<ulen>> kids(nb);
  std::vector<ulen> par(nb, opt_ns::none_v);
  for (ulen b = 1; b < nb; ++b) {
    par[b] = kz_pick(rng, b);
    kids[par[b]].push_back(b);
  }
  auto domf = gns::make_basic_forest<V>(kids);
  std::vector<char> drop(nb, 0);
  for (ulen b = 0; b < nb; ++b)
    drop[b] = kz_chance(rng, 25) ? 1 : 0;
  // events, keyed randomly; loads record their dst as the value
  struct KzEv {
    opt_ns::mem_event_t e{};
  };
  std::vector<std::vector<opt_ns::mem_event_t>> evs(nb);
  for (ulen b = 0; b < nb; ++b) {
    const int k = int(kz_pick(rng, 5));
    for (int j = 0; j < k; ++j) {
      opt_ns::mem_event_t e{};
      const int r = int(kz_pick(rng, 100));
      if (r < 35) {
        e.kind = opt_ns::mem_store_v;
        e.val = kz_chance(rng, 85) ? 10 + kz_pick(rng, 20)
                                   : uintlen_t(opt_ns::none_v);
      } else if (r < 90) {
        e.kind = opt_ns::mem_load_v;
        e.val = 50 + kz_pick(rng, 20); // the dst the load defines
      } else {
        e.kind = opt_ns::mem_clear_v;
      }
      e.group = kz_pick(rng, 3);
      e.key_tag = kz_pick(rng, 2);
      e.key_a = kz_pick(rng, 4);
      e.key_b = kz_pick(rng, 4);
      evs[b].push_back(e);
    }
  }
  // three may-kill policies, all exercised per seed
  const int pol = int(kz_pick(rng, 3));
  auto may_kill = [&](uintlen_t eg, uintlen_t et, uintlen_t eka,
                      uintlen_t ekb, const opt_ns::mem_event_t &ev) {
    if (pol == 0)
      return eg == ev.group && et == ev.key_tag &&
             (ev.key_tag ? ekb == ev.key_b : eka == ev.key_a);
    if (pol == 1)
      return eg == ev.group; // a whole-group clobber
    return false; // never kills
  };
  auto hits = opt_ns::run_dom_scoped_table<V>(domf, evs, drop, evs, may_kill);
  // THE REFERENCE -- an independent restatement of the kernel's LIVE
  // TABLE semantics (the dom-path FOLD was wrong here, found by this
  // very fuzzer): one table shared across the whole dominator-tree
  // DFS, no seq counters, no watermarks -- each entry instead carries
  // the subtree depth it was pushed AT (a block's own events push at
  // d+1, so they die with the block's subtree, exactly the kernel's
  // entry-watermark rule), and leaving a frame erases exactly the
  // entries with depth beyond its own. Two consequences the fold
  // missed, both load-bearing in the kernel (and both sound for its
  // client, where they only ever cause MISSES):
  //   * KILLS ARE PERMANENT: a store inside an earlier-visited sibling
  //     subtree removes a path entry for every LATER sibling too (the
  //     exit filter only drops pushed-inside entries, it cannot
  //     resurrect killed ones);
  //   * DROPS ARE SUBTREE-LOCAL: the drop_at clear wipes the live
  //     table at entry, and the subtree's pushed entries die on exit,
  //     so a join's dropped state never leaks to its right siblings
  //     (their table is whatever survived, which may be nothing).
  struct Ent {
    uintlen_t group{}, tag{}, ka{}, kb{}, val{}, depth{};
    bool from_store{};
  };
  std::vector<Ent> table{};
  std::vector<opt_ns::mem_hit_t> want{};
  {
    std::function<void(ulen, uintlen_t)> dfs = [&](ulen b, uintlen_t d) {
      if (drop[b])
        table.clear(); // join: the memory state merges
      for (size_t j = 0; j < evs[b].size(); ++j) {
        const opt_ns::mem_event_t &ev = evs[b][j];
        if (ev.kind == opt_ns::mem_store_v) {
          std::vector<Ent> keep{};
          for (const Ent &e : table)
            if (!may_kill(e.group, e.tag, e.ka, e.kb, ev))
              keep.push_back(e);
          table = std::move(keep);
          if (ev.val != opt_ns::none_v)
            table.push_back({ev.group, ev.key_tag, ev.key_a, ev.key_b,
                             ev.val, d + 1, true});
        } else if (ev.kind == opt_ns::mem_load_v) {
          opt_ns::mem_hit_t h{};
          for (const Ent &e : table)
            if (e.group == ev.group && e.tag == ev.key_tag &&
                (ev.key_tag ? e.kb == ev.key_b : e.ka == ev.key_a)) {
              h = {.found = true, .val = e.val, .from_store = e.from_store};
              break;
            }
          want.push_back(h);
          table.push_back({ev.group, ev.key_tag, ev.key_a, ev.key_b,
                           ev.val, d + 1, false});
        } else {
          table.clear(); // call / unknown clobber
        }
      }
      for (ulen c : kids[b])
        dfs(c, d + 1);
      // leaving b: everything pushed inside its subtree dies
      size_t w = 0;
      for (size_t r = 0; r < table.size(); ++r)
        if (table[r].depth <= d)
          table[w++] = table[r];
      table.resize(w);
    };
    if (nb)
      dfs(0, 0);
  }
  R.ok(hits.size() == want.size(), "mem: one hit per load, DFS order");
  for (size_t i = 0; i < hits.size() && i < want.size(); ++i)
    R.ok(hits[i].found == want[i].found && hits[i].val == want[i].val &&
             hits[i].from_store == want[i].from_store,
         "mem: the live-table replica agrees (scoping, kills, drops)");
}

// ---------------------------------------------------------------------------
// 24. opt.iv: the interval lattice vs its math (+ the algebra)
// ---------------------------------------------------------------------------
static void fz_iv(std::mt19937_64 &rng, KzRec &R) {
  using opt_ns::iv_ns::iv_t;
  auto rnd_iv = [&]() {
    iv_t x{};
    x.bot = kz_chance(rng, 15);
    int64_t a = int64_t(rng() % 2001) - 1000;
    int64_t b = int64_t(rng() % 2001) - 1000;
    if (kz_chance(rng, 10))
      std::swap(a, b); // inverted inputs: the lattice must stay sound
    x.lo = a < b ? a : b;
    x.hi = a < b ? b : a;
    if (!x.bot && kz_chance(rng, 10)) {
      x.lo = a;
      x.hi = b; // possibly lo > hi (defensive input)
    }
    return x;
  };
  const iv_t a = rnd_iv(), b = rnd_iv(), c = rnd_iv();
  // bot-tolerant equality for the algebra checks: both-bot answers
  // may carry any bounds (the kernel returns the RIGHT operand when
  // the left is bot -- an implementation choice the algebra must not
  // observe bit-for-bit)
  auto iv_same = [](const iv_t &x, const iv_t &y) {
    return x.bot == y.bot &&
           (x.bot || (x.lo == y.lo && x.hi == y.hi));
  };
  // join = hull
  {
    iv_t j = opt_ns::iv_ns::iv_join(a, b);
    // the kernel's exact shape: a.bot returns b, b.bot returns a (so
    // both-bot carries b's bounds), else the hull
    iv_t w = b;
    if (!a.bot && b.bot)
      w = a;
    else if (!a.bot && !b.bot)
      w = {a.lo < b.lo ? a.lo : b.lo, a.hi > b.hi ? a.hi : b.hi, false};
    R.ok(j.lo == w.lo && j.hi == w.hi && j.bot == w.bot,
         "iv: join is the hull with bot identity");
  }
  // join commutes up to both-bot representation; hulls associate ditto
  {
    iv_t ab = opt_ns::iv_ns::iv_join(a, b), ba = opt_ns::iv_ns::iv_join(b, a);
    R.ok(iv_same(ab, ba), "iv: join commutes");
    iv_t l = opt_ns::iv_ns::iv_join(opt_ns::iv_ns::iv_join(a, b), c);
    iv_t r2 = opt_ns::iv_ns::iv_join(a, opt_ns::iv_ns::iv_join(b, c));
    R.ok(iv_same(l, r2), "iv: join associates");
  }
  // narrow = the tighter sound intersect, kept only when non-empty
  {
    iv_t old = a, keep = a;
    opt_ns::iv_ns::iv_narrow(old, b);
    if (!a.bot && !b.bot) {
      const int64_t lo = b.lo > a.lo ? b.lo : a.lo;
      const int64_t hi = b.hi < a.hi ? b.hi : a.hi;
      if (lo <= hi)
        keep = {lo, hi, false};
    }
    R.ok(old.lo == keep.lo && old.hi == keep.hi && old.bot == keep.bot,
         "iv: narrowing intersects, keeping the old on empty");
  }
  // the trap predicates
  {
    const iv_t x = rnd_iv();
    R.ok(opt_ns::iv_ns::iv_excludes_zero(x) ==
             (x.bot || x.hi < 0 || x.lo > 0),
         "iv: excludes-zero");
    R.ok(opt_ns::iv_ns::iv_excludes_minus1(x) ==
             (x.bot || x.hi < -1 || x.lo > -1),
         "iv: excludes-minus-one");
    R.ok(opt_ns::iv_ns::iv_excludes_min(x) == (x.bot || x.lo > INT64_MIN),
         "iv: excludes-min");
  }
}

// ---------------------------------------------------------------------------
// 25. opt.ai: the abstract-interpretation driver -- convergence, plus
//     SOUNDNESS against concrete random paths (entry intervals must
//     contain every value a feasible path carries, post-narrowing too)
// ---------------------------------------------------------------------------
static void fz_ai(std::mt19937_64 &rng, KzRec &R) {
  using opt_ns::iv_ns::iv_t;
  const ulen nb = 2 + kz_pick(rng, 5);
  // a consistent CFG: succs then preds. The entry block is a PURE
  // entry (no in-edges): entry_top re-asserts the seed state at every
  // visit, which is only the concrete semantics when nothing loops
  // back into the entry -- the shape the compiler's own fns have
  std::vector<std::vector<ulen>> succs(nb);
  for (ulen b = 0; b < nb; ++b) {
    const int deg = int(kz_pick(rng, 3));
    for (int j = 0; j < deg; ++j)
      succs[b].push_back(1 + kz_pick(rng, nb - 1));
  }
  std::vector<std::vector<ulen>> preds(nb);
  for (ulen b = 0; b < nb; ++b)
    for (ulen s : succs[b])
      preds[s].push_back(b);
  // the per-block program: v0 += step (saturating at MAX), v1 joins a
  // block range; marked edges refine v0 to [rl, rh] (paths outside are
  // infeasible); entry: v0 = c, v1 = 0
  const int64_t c = int64_t(rng() % 101) - 50;
  std::vector<int64_t> step(nb), ja(nb), jb(nb);
  std::vector<int64_t> rl(nb), rh(nb);
  std::vector<std::vector<char>> marked(nb, std::vector<char>(nb, 0));
  for (ulen b = 0; b < nb; ++b) {
    step[b] = int64_t(kz_pick(rng, 4));
    int64_t a = int64_t(rng() % 201) - 100;
    int64_t z = a + int64_t(kz_pick(rng, 20));
    ja[b] = a;
    jb[b] = z;
  }
  for (ulen p = 0; p < nb; ++p)
    for (ulen s : succs[p])
      if (kz_chance(rng, 25)) {
        marked[p][s] = 1;
        const int64_t mid = int64_t(rng() % 401) - 200;
        rl[p] = mid - int64_t(rng() % 50);
        rh[p] = mid + int64_t(rng() % 50);
        if (rl[p] > rh[p])
          std::swap(rl[p], rh[p]);
      }
  const ulen plain_cap = 4 + kz_pick(rng, 5);
  const ulen widen_cap = 20 + kz_pick(rng, 20);
  const ulen narrow = 1 + kz_pick(rng, 3);
  struct CB {
    int64_t c;
    const std::vector<int64_t> *step, *ja, *jb, *rl, *rh;
    const std::vector<std::vector<char>> *marked;
    void entry_top(std::vector<iv_t> &st) const {
      st[0] = {c, c, false};
      st[1] = {0, 0, false};
    }
    void refine_edge(uintlen_t p, uintlen_t b, std::vector<iv_t> &st) const {
      if (!(*marked)[p][b] || st[0].bot)
        return;
      const int64_t lo = (*rl)[p] > st[0].lo ? (*rl)[p] : st[0].lo;
      const int64_t hi = (*rh)[p] < st[0].hi ? (*rh)[p] : st[0].hi;
      st[0] = lo <= hi ? iv_t{lo, hi, false} : iv_t{0, 0, true};
    }
    void phi_join(uintlen_t, std::vector<iv_t> &,
                  const iv_t *, uintlen_t) const {}
    void transfer(uintlen_t b, std::vector<iv_t> &st) const {
      if (!st[0].bot) {
        st[0].hi = st[0].hi == INT64_MAX ? INT64_MAX
                                         : st[0].hi + (*step)[b];
        if (st[0].hi < st[0].lo) // overflow paranoia: stay sound
          st[0].hi = INT64_MAX;
      }
      st[1] = opt_ns::iv_ns::iv_join(st[1], iv_t{(*ja)[b], (*jb)[b], false});
    }
  };
  // TWO solve passes per seed:
  //   * the RANDOM widening points -- where a growing cycle without a
  //     widening point is a LEGITIMATE cap give-up (converged=false, the
  //     client's documented fallback to sound TOP; the partial table is
  //     then not a soundness claim, so the walk only runs when it did
  //     converge),
  //   * widening EVERYWHERE -- must always converge, and the soundness
  //     walk must hold (widening only inflates; narrowing keeps the
  //     old state when the intersect empties, both sound moves)
  const int walks = 8;
  auto solve_and_walk = [&](const std::vector<char> &widen,
                            bool require_converged,
                            const std::vector<std::vector<char>> &marks,
                            const std::vector<int64_t> &step_v) {
    const CB cb{c, &step_v, &ja, &jb, &rl, &rh, &marks};
    auto Res = opt_ns::ai_ns::solve_forward<V>(nb, 2, preds, widen, 0,
                                               plain_cap, widen_cap, narrow,
                                               cb);
    R.ok(Res.entry.size() == nb * 2, "ai: entry table shape");
    if (require_converged)
      R.ok(Res.converged,
           "ai: the pure-join shape (no refine, no growth) must converge");
    if (!Res.converged)
      return;
    for (int w = 0; w < walks; ++w) {
      int64_t v0 = c, v1 = 0;
      ulen b = 0;
      for (int t = 0; t < 40; ++t) {
        const iv_t &e0 = Res.entry[b * 2 + 0];
        const iv_t &e1 = Res.entry[b * 2 + 1];
        R.ok(e0.bot || (e0.lo <= v0 && v0 <= e0.hi),
             "ai: entry[0] contains the concrete v0");
        R.ok(e1.bot || (e1.lo <= v1 && v1 <= e1.hi),
             "ai: entry[1] contains the concrete v1");
        if (succs[b].empty())
          break;
        const ulen s = succs[b][kz_pick(rng, ulen(succs[b].size()))];
        // the block's body ran: apply the transfer, THEN the edge's fact
        v0 += step_v[b];
        v1 = kz_chance(rng, 50) ? ja[b] : jb[b];
        if (marks[b][s] && (v0 < rl[b] || v0 > rh[b]))
          break; // the branch condition excludes this path: infeasible
        b = s;
      }
    }
  };
  const auto no_marks =
      std::vector<std::vector<char>>(nb, std::vector<char>(nb, 0));
  // THREE solve passes per seed (the convergence CONTRACT, pinned by
  // this fuzzer's own two findings -- seeds 130 and 407):
  //   * RANDOM widening points, the real (shrinking) refine, real
  //     growth -- NO convergence claim: a growing cycle without a
  //     widening point is a LEGITIMATE cap give-up (converged=false,
  //     the client's documented fallback to sound TOP; the partial
  //     table is then not a soundness claim, so the walk only runs
  //     when it did converge)
  //   * widening EVERYWHERE -- STILL no convergence claim. The
  //     kernel's widening grows the growing sides but is NOT sticky:
  //     entry := F-then-grow, not the classic entry := W(entry,
  //     F(entry)) that keeps the old bounds. Two ways to flip-flop,
  //     both traced: a SHRINKING refine can pull a widened entry back
  //     down (seed 130: b2 alternating [37,MAX] / [37,39]); and even
  //     with refine OFF the per-round STAGGER does it on any cycle
  //     (seed 407: the 1->4->3->2->1 loop -- a block widens to MAX at
  //     round r, and at r+1 its pred's lagging exit hulls it back to
  //     the small wave value, which then regrows and re-widens, lap
  //     after lap, until the cap). The give-up path IS the contract;
  //     the soundness walk still runs whenever it did converge.
  //   * the PURE-JOIN shape -- refine disabled AND step all zero, so
  //     every update is a monotone join of monotone joins (transfer
  //     keeps v0, joins a FIXED range into v1; hull over preds whose
  //     exits never shrink). There the iterate is a genuine chain, so
  //     it MUST converge, with any widening points -- and the
  //     soundness walk must hold (widening only inflates; narrowing
  //     keeps the old state when the intersect empties, both sound)
  std::vector<char> widen_at(nb, 0);
  for (ulen b = 0; b < nb; ++b)
    widen_at[b] = kz_chance(rng, 40) ? 1 : 0;
  solve_and_walk(widen_at, false, marked, step);
  std::vector<char> widen_all(nb, 1);
  solve_and_walk(widen_all, false, no_marks, step);
  solve_and_walk(widen_at, true, no_marks,
                 std::vector<int64_t>(nb, 0));
}

// ---------------------------------------------------------------------------
// 26. opt.key / 27. opt.leak / 28. opt.ipa / 29. opt.equi: the four
//     small kernels, each vs its reference
// ---------------------------------------------------------------------------
static void fz_key(std::mt19937_64 &rng, KzRec &R) {
  const ulen nv = 1 + kz_pick(rng, 6);
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (ulen v = 0; v < nv; ++v)
    if (kz_chance(rng, 50)) {
      kc[v] = 1;
      kv[v] = val_t(int64_t(rng() % 2001) - 1000);
    }
  for (int t = 0; t < 8; ++t) {
    uintlen_t v;
    const int r = int(kz_pick(rng, 100));
    if (r < 20)
      v = uintlen_t(opt_ns::none_v);
    else if (r < 40)
      v = nv + kz_pick(rng, 3); // junk: keys by its id
    else
      v = kz_pick(rng, nv);
    const uint64_t got = opt_ns::mixed_sort_key(nv, kc, kv, v);
    uint64_t want;
    if (v == uintlen_t(opt_ns::none_v))
      want = 0;
    else if (v < nv && kc[v])
      want = (uint64_t(1) << 63) ^ uint64_t(kv[v]);
    else
      want = uint64_t(v);
    R.ok(got == want, "key: absent 0, const by value after the tag bit, "
                      "else the id");
  }
}

static void fz_leak(std::mt19937_64 &rng, KzRec &R) {
  const ulen nv = kz_pick(rng, 6), ns = kz_pick(rng, 9);
  std::vector<uintlen_t> def_site(nv, opt_ns::none_v);
  for (ulen v = 0; v < nv; ++v) {
    const int r = int(kz_pick(rng, 100));
    if (r < 70)
      def_site[v] = kz_pick(rng, ns);
    else if (r >= 85)
      def_site[v] = ns + kz_pick(rng, 3);
  }
  std::vector<std::vector<uintlen_t>> args(ns);
  for (ulen s = 0; s < ns; ++s) {
    const int k = int(kz_pick(rng, 3));
    for (int j = 0; j < k; ++j) {
      if (kz_chance(rng, 85))
        args[s].push_back(kz_pick(rng, nv));
      else
        args[s].push_back(nv + kz_pick(rng, 3));
    }
  }
  ulen lo = kz_pick(rng, ns + 2), hi = kz_pick(rng, ns + 2);
  if (kz_chance(rng, 20))
    std::swap(lo, hi); // inverted region: still a well-defined question
  const bool got = opt_ns::region_uses_escape(def_site, args, lo, hi);
  bool want = false;
  for (ulen s = 0; s < ns && !want; ++s) {
    if (lo <= s && s < hi)
      continue;
    for (ulen v : args[s]) {
      if (v >= nv)
        continue;
      const ulen d = def_site[v];
      if (d != opt_ns::none_v && lo <= d && d < hi) {
        want = true;
        break;
      }
    }
  }
  R.ok(got == want, "leak: a region-local def read outside escapes");
}

static void fz_ipa(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 5);
  std::vector<char> elig(n, 0);
  std::vector<std::vector<char>> alive(n);
  std::vector<std::vector<std::vector<opt_ns::soft_edge_t>>> softs(n);
  for (ulen g = 0; g < n; ++g) {
    elig[g] = kz_chance(rng, 60) ? 1 : 0;
    const ulen np = kz_pick(rng, 4);
    alive[g].assign(np, 0);
    softs[g].assign(np, {});
    for (ulen p = 0; p < np; ++p) {
      alive[g][p] = kz_chance(rng, 30) ? 1 : 0; // hard roots
      const int k = int(kz_pick(rng, 3));
      for (int j = 0; j < k; ++j)
        softs[g][p].push_back({kz_pick(rng, n), kz_pick(rng, 4)});
    }
  }
  auto want = alive;
  for (bool ch = true; ch;) {
    ch = false;
    for (ulen g = 0; g < n; ++g) {
      if (!(g < elig.size() && elig[g]))
        continue;
      for (ulen p = 0; p < want[g].size(); ++p) {
        if (want[g][p])
          continue;
        for (const opt_ns::soft_edge_t &s : softs[g][p]) {
          const bool feeds = !(s.h < elig.size() && elig[s.h]) ||
                             (s.q < want[s.h].size() && want[s.h][s.q]);
          if (feeds) {
            want[g][p] = 1;
            ch = true;
            break;
          }
        }
      }
    }
  }
  opt_ns::calculate_live_params(elig, softs, alive);
  R.ok(alive == want, "ipa: the liveness fixpoint matches its replica");
}

static void fz_equi(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 8);
  std::vector<std::vector<uint64_t>> keys(n);
  std::vector<char> cand(n, 0);
  std::vector<uintlen_t> rep(n, 0);
  for (ulen f = 0; f < n; ++f) {
    const int k = int(kz_pick(rng, 3));
    for (int j = 0; j < k; ++j)
      keys[f].push_back(rng() % 4); // small key space: collisions abound
    cand[f] = kz_chance(rng, 70) ? 1 : 0;
    rep[f] = kz_pick(rng, f + 1); // a prior partition (any function)
  }
  auto want = rep;
  bool wch = false;
  {
    std::vector<uintlen_t> nrep(n);
    for (ulen f = 0; f < n; ++f)
      nrep[f] = f;
    for (ulen f = 0; f < n; ++f) {
      if (!(f < cand.size() && cand[f]))
        continue;
      for (ulen g = 0; g < f; ++g) {
        if (!(g < cand.size() && cand[g]))
          continue;
        if (keys[g] == keys[f]) {
          nrep[f] = nrep[g];
          break;
        }
      }
      wch |= (nrep[f] != rep[f]);
    }
    want = nrep;
  }
  const bool got = opt_ns::refine_by_keys(keys.data(), cand, rep);
  R.ok(got == wch, "equi: changed flag matches");
  R.ok(rep == want, "equi: rep-of-first-equal matches the reference");
}

// ---------------------------------------------------------------------------
// the driver: component selection, seeds, workers, the report
// ---------------------------------------------------------------------------
// the graph library's component bodies (generators, references, the
// 26 gns.* fuzzers above the opt/ssa ones in the table)
#include "fuzz_graphs.hpp"

using KzFn = void (*)(std::mt19937_64 &, KzRec &);
static const KzFn KZ_FNS[] = {
    fz_forests, fz_dom,     fz_df,        fz_phis,    fz_wrapper, fz_rename,
    fz_reflow,  fz_chain,   fz_mark,      fz_vn,      fz_sccp,    fz_pcopy,
    fz_split,   fz_reach,   fz_splice,    fz_inv,     fz_ren,     fz_trivphi,
    fz_shape,   fz_exits,   fz_route,     fz_needs,   fz_alias,   fz_mem,
    fz_iv,      fz_ai,      fz_key,       fz_leak,    fz_ipa,     fz_equi,
    fz_invfam,  fz_scc,     fz_seq,       fz_dsu,     fz_search,  fz_dij,
    fz_flow,    fz_stcut,   fz_mcf,       fz_gcut,    fz_dgcut,   fz_maxcut,
    fz_gh,      fz_mst,     fz_match,     fz_blossom, fz_bic,     fz_kcore,
    fz_euler,   fz_tr,        fz_rmas,      fz_dist,
    fz_meg,     fz_maxclosure, fz_pathcover};
static_assert(sizeof(KZ_FNS) / sizeof(KZ_FNS[0]) == KZ_NCOMP,
              "component table mismatch");

static void kz_one(long seed, const std::vector<int> &comps, KzStats &st,
                   std::mutex *mu) {
  // every component gets its OWN rng stream, derived from (seed,
  // component index): a component's inputs never depend on which other
  // components ran before it, so --fuzz-kernel=NAME --fuzz-kernels-from=S
  // reproduces a failure EXACTLY, and adding components later cannot
  // shift older windows
  for (int c : comps) {
    std::mt19937_64 rng(uint64_t(seed) * 0x9E3779B97F4A7C15ull +
                        uint64_t(c) * 0xBF58476D1CE4E5B9ull +
                        0xD1B54A32D192ED03ull);
    KzRec rec{c, seed, &st, mu};
    KZ_FNS[c](rng, rec);
  }
}

static bool kz_name_has(const char *name, const std::string &filter) {
  if (filter.empty())
    return true;
  for (size_t i = 0; name[i]; ++i) {
    size_t j = 0;
    while (name[i + j] && filter[j] &&
           std::tolower(name[i + j]) == std::tolower(filter[j]))
      ++j;
    if (j == filter.size() && !filter.empty())
      return true;
  }
  return false;
}

void run_kernel_fuzz(long n, long from, long jobs, const std::string &filter) {
  const auto t0 = std::chrono::steady_clock::now();
  // component selection (substring, case-insensitive)
  std::vector<int> comps{};
  for (int i = 0; i < KZ_NCOMP; ++i)
    if (kz_name_has(KZ_NAMES[i], filter))
      comps.push_back(i);
  if (comps.empty()) {
    std::printf("kernel fuzz: no component matches '%s' (components:",
                filter.c_str());
    for (int i = 0; i < KZ_NCOMP; ++i)
      std::printf(" %s", KZ_NAMES[i]);
    std::printf(")\n");
    ++g_checks;
    ++g_fails;
    return;
  }
  std::printf("=== kernel fuzz: %d component%s x %ld seeds (from %ld) vs "
              "independent references ===\n",
              int(comps.size()), comps.size() == 1 ? "" : "s", n, from);
  std::fflush(stdout);
  if (n <= 0) {
    ++g_checks; // nothing asked, nothing to check
    return;
  }
  if (jobs < 1)
    jobs = 1;
  long report_every = n / 20;
  if (report_every < 25)
    report_every = 25;
  KzStats total{};
  long done = 0;
  if (jobs == 1) {
    for (long seed = from; seed < from + n; ++seed) {
      kz_one(seed, comps, total, nullptr);
      if (++done % report_every == 0 || done == n) {
        double el = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
        std::fprintf(stderr,
                     "[kfuzz %ld/%ld] %.0fs elapsed, eta %.0fs, fails %ld\n",
                     done, n, el, el / done * (n - done),
                     total.total_fails());
        std::fflush(stderr);
      }
    }
  } else {
    std::mutex mu{};
    std::atomic<long> next_seed{from};
    std::atomic<long> adone{0};
    std::vector<KzStats> per(jobs);
    std::vector<std::thread> workers{};
    for (long w = 0; w < jobs; ++w) {
      workers.emplace_back([&, w]() {
        for (;;) {
          const long seed = next_seed.fetch_add(1);
          if (seed >= from + n)
            break;
          kz_one(seed, comps, per[w], &mu);
          const long d = adone.fetch_add(1) + 1;
          if (w == 0 && (d % report_every == 0 || d == n)) {
            double el = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
            std::fprintf(stderr,
                         "[kfuzz %ld/%ld] %.0fs elapsed, eta %.0fs\n", d, n,
                         el, el / d * (n - d));
            std::fflush(stderr);
          }
        }
      });
    }
    for (std::thread &t : workers)
      t.join();
    for (const KzStats &p : per)
      total.add(p);
    done = n;
  }
  (void)done;
  // the report: one line per component, then the verdict
  long checks = 0;
  for (int i = 0; i < KZ_NCOMP; ++i)
    checks += total.checks[i];
  std::printf("kernel fuzz: %ld seeds | components run: %d | checks: %ld | "
              "failures: %ld\n",
              n, int(comps.size()), checks, total.total_fails());
  for (int i = 0; i < KZ_NCOMP; ++i) {
    if (!total.checks[i] && !total.fails[i])
      continue; // not selected this run
    std::printf("  %-16s %10ld checks  %3ld fails\n", KZ_NAMES[i],
                total.checks[i], total.fails[i]);
  }
  double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  std::printf("  wall: %.1fs%s\n", secs, jobs == 1 ? "" :
              (" on " + std::to_string(jobs) + " jobs").c_str());
  CHECK(total.total_fails() == 0, "kernel fuzz must be clean");
  std::fflush(stdout);
}
