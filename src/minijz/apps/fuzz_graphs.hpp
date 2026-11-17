#pragma once
// === fuzz_graphs.hpp ========================================
// the graph-library half of the component fuzzer: every algorithmic
// component of mjz_lib's graph_ns -- the inverse universe family, the
// order layers (SCC, topological waves), the search engines (BFS/DFS,
// Dijkstra/A*), the flow/cut family (Dinic, s-t cut, min-cost flow,
// Stoer-Wagner, directed global cut, max cut, Gomory-Hu), the
// spanning/matching family (Kruskal/Prim/Boruvka, Hopcroft-Karp/Kuhn/
// Konig, blossoms), the connectivity family (bridges/articulation/
// biconnected components, k-cores, Euler tours), the minimum arc set
// family (the weighted feedback arc set, the transitive reduction,
// the minimum equivalent arc set), the selection family (the
// maximum-weight closure, the minimum path cover), and the distance
// engines -- each
// hammered on random
// inputs against a reference coded from the DEFINITION, never from
// the library's source. fuzz_kernels.hpp owns the driver and the
// component table (both via fuzz_base.hpp); this file only contributes
// bodies -- self-contained: it includes its own plumbing (fuzz_base.hpp;
// #pragma once makes that a no-op when fuzz_kernels.hpp includes this
// file after its own bodies, so the table below it sees ours).
//
// THE GENERATORS respect the CSR contract the library's own kernels
// assume: targets in range, self loops and parallel arcs legal (the
// one documented exception is the bipartite family, which tolerates
// junk right ids). Undirected input is the pair representation: each
// undirected edge is two arcs, its weight on each.
//
// THE REFERENCES, by component:
//   * invfam: the definitional transpose (per-row entry lists,
//     payloads, linear source indices), every fill order and universe.
//   * scc: mutual reachability classes (BFS per node).
//   * seq: Kahn layers over the condensation, re-coded -- waves,
//     queue order, the self-sequential sign, the topo permutation.
//   * dsu: naive labels under a random op stream.
//   * search: plain BFS and plain DFS in row order (times, parents,
//     edge classes by interval containment).
//   * dij: min weight of a SIMPLE path (Held-Karp over node masks --
//     no relaxation anywhere); A* under three consistent heuristics.
//   * flow/stcut: the 2^(n-2) s-t cut enumeration + the saturated
//     cut certificate (complementary slackness).
//   * mcf: conservation + capacity + the no-negative-residual-cycle
//     optimality certificate (a min-cost flow's residual cannot hide
//     an improving cycle -- Bellman-Ford over the rebuilt residual).
//   * gcut/dgcut/maxcut/gh: subset enumeration (the directed cut
//     counts every crossing arc; the undirected cut counts each edge
//     once; Gomory-Hu is asked every pair).
//   * mst: an independent Prim over the undirected edge list (the
//     total is the unique minimum; all three algorithms must agree).
//   * match: Hopcroft-Karp vs Kuhn (two engines, one answer) + the
//     Konig duality + cover validity; blossom vs bitmask DP.
//   * bic: simple-cycle enumeration (the definitional co-cycle
//     classes), removal tests for bridges/articulation, the per-k
//     iterative removal for k-cores.
//   * euler: the degree/piece conditions re-derived, then the tour
//     itself walked (consecutive arcs, each edge exactly once, the
//     promised endpoints).
//   * tr/meg: the length->=2-path redundancy rule; reachability
//     preservation + the exact-on-DAGs guarantee + the 2(n-1)
//     honesty bound.
//   * rmas: subset enumeration for the minimum (the definition:
//     the cheapest arc subset whose removal leaves a DAG), Kahn for
//     the FAS-ness, the order certificate.
//   * closure: every closed subset enumerated (successor-closed is
//     the definition), the maximum summed weight; the returned set
//     re-checked closed, summing, never below the empty set.
//   * pcover: the Dilworth count by branch-and-bound split-graph
//     matching; the returned paths walked (disjoint, covering,
//     every hop a real arc).
//   * dist: Held-Karp simple-path minima, negative-cycle poison by
//     enumeration, an independent topo relaxation, BFS eccentricity.

// ---------------------------------------------------------------------------
// generators + shared reference layer
// ---------------------------------------------------------------------------
#include "fuzz_base.hpp"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <utility>

// a random digraph: valid targets only (the CSR contract), self loops
// and duplicate slots legal
static std::vector<std::vector<ulen>> kzg_digraph(std::mt19937_64 &rng, ulen n,
                                                  ulen deg) {
  std::vector<std::vector<ulen>> rows(n);
  for (ulen u = 0; u < n; ++u) {
    const int k = int(kz_pick(rng, deg + 1));
    for (int j = 0; j < k; ++j)
      rows[u].push_back(kz_pick(rng, n));
  }
  return rows;
}

// a random DAG: a hidden permutation orders the nodes; arcs only run
// from earlier to later (rows stay indexed by node id, so the CSR row
// order and the acyclic order disagree -- the interesting shape)
static std::vector<std::vector<ulen>> kzg_dag(std::mt19937_64 &rng, ulen n,
                                              ulen deg) {
  std::vector<ulen> perm(n);
  for (ulen i = 0; i < n; ++i)
    perm[i] = i;
  for (ulen i = n; i-- > 1;)
    std::swap(perm[i], perm[kz_pick(rng, i + 1)]);
  std::vector<ulen> pos(n);
  for (ulen i = 0; i < n; ++i)
    pos[perm[i]] = i;
  std::vector<std::vector<ulen>> rows(n);
  for (ulen a = 0; a < n; ++a)
    for (ulen b = 0; b < n; ++b)
      if (pos[a] < pos[b] && kz_chance(rng, 35)) {
        rows[a].push_back(b);
        if (kz_chance(rng, 20))
          rows[a].push_back(b); // parallel arcs
      }
  (void)deg;
  return rows;
}

// a random REDUCIBLE flow graph, by construction. The recipe keeps
// the dominators exactly the tree ancestors that receive back arcs:
//   * a spanning tree out of the root (the root reaches everything),
//   * a set of MARKED nodes M -- the back-arc targets. A marked
//     node h keeps h-dominates-everything-in-its-subtree as long as
//     no arc enters subtree(h) from outside without passing h, so
//   * back arcs only target marked strict tree ancestors, and
//   * forward arcs a->b (b a tree descendant of a) only when NO
//     marked node lies strictly between them -- the arc would
//     otherwise enter that node's subtree from above and unseat its
//     domination. Tree and forward arcs both run down the tree, so
//     stripping the dominator back arcs leaves a DAG: reducible.
// inject_cross asks for one ancestor-unrelated pair a->b, b->a: the
// cycle a-b-a then owns no on-cycle dominator (neither can dominate
// the other without being a tree ancestor), the irreducibility
// witness. The second return says whether the pair was actually
// placed (a path-shaped tree may offer no unrelated pair).
static std::pair<std::vector<std::vector<ulen>>, bool>
kzg_reducible(std::mt19937_64 &rng, ulen n, bool inject_cross) {
  std::vector<ulen> par(n, kz_none());
  for (ulen i = 1; i < n; ++i)
    par[i] = kz_pick(rng, i);
  std::vector<char> marked(n, 0);
  for (ulen i = 0; i < n; ++i)
    marked[i] = kz_chance(rng, 35) ? 1 : 0;
  std::vector<std::vector<ulen>> rows(n);
  for (ulen i = 1; i < n; ++i)
    rows[par[i]].push_back(i);
  const auto rand_strict_ancestor = [&](ulen v) noexcept {
    // a random strict tree ancestor of v (kz_none(): a short hop)
    if (par[v] == kz_none() || !kz_chance(rng, 60))
      return kz_none();
    ulen u = par[v];
    while (par[u] != kz_none() && kz_chance(rng, 60))
      u = par[u];
    return u;
  };
  const ulen bck = kz_pick(rng, n + 1);
  for (ulen k = 0; k < bck && n > 1; ++k) {
    const ulen v = 1 + kz_pick(rng, n - 1);
    const ulen h = rand_strict_ancestor(v);
    if (h != kz_none() && marked[h])
      rows[v].push_back(h); // back: to a MARKED strict tree ancestor
  }
  const ulen fwd = kz_pick(rng, n + 1);
  for (ulen k = 0; k < fwd && n > 1; ++k) {
    const ulen v = 1 + kz_pick(rng, n - 1);
    const ulen a = rand_strict_ancestor(v);
    if (a == kz_none())
      continue;
    // no marked node strictly between a and v (the arc must not
    // enter any marked subtree from above)
    bool blocked = false;
    for (ulen z = par[v]; z != a && z != kz_none(); z = par[z])
      if (marked[z])
        blocked = true;
    if (!blocked)
      rows[a].push_back(v); // forward: down the tree, no marks crossed
  }
  if (kz_chance(rng, 20) && n) { // a self loop or two: always backward
    for (ulen k = 0, c = 1 + kz_pick(rng, 2); k < c; ++k) {
      const ulen u = kz_pick(rng, n);
      rows[u].push_back(u);
    }
  }
  bool witnessed = false;
  if (inject_cross && n > 2) {
    for (ulen tries = 0; tries < 12 && !witnessed; ++tries) {
      const ulen a = 1 + kz_pick(rng, n - 1);
      const ulen b = 1 + kz_pick(rng, n - 1);
      if (a == b)
        continue;
      auto is_anc = [&](ulen x, ulen y) noexcept { // x ancestor of y?
        for (ulen z = y; z != kz_none(); z = par[z])
          if (z == x)
            return true;
        return false;
      };
      if (is_anc(a, b) || is_anc(b, a))
        continue;
      rows[a].push_back(b);
      rows[b].push_back(a);
      witnessed = true;
    }
  }
  return {std::move(rows), witnessed};
}
struct KzgArc {
  ulen from{}, to{};
};
// the arc list in row-major order (the ids make_basic_forest assigns)
static std::vector<KzgArc>
kzg_arcs(const std::vector<std::vector<ulen>> &rows) {
  std::vector<KzgArc> arcs{};
  for (ulen u = 0; u < rows.size(); ++u)
    for (ulen v : rows[u])
      arcs.push_back({u, v});
  return arcs;
}

// the symmetric graph: undirected edge list (self loops legal), the
// pair-representation rows, and the per-slot edge index that maps an
// arc back to its undirected edge
struct KzgSym {
  ulen n{};
  std::vector<std::pair<ulen, ulen>> edges{};
  std::vector<std::vector<ulen>> rows{};
  std::vector<std::vector<ulen>> slot_edge{}; // row slot -> edge index
  std::vector<ulen> edge_of_arc{};            // flattened, arc-major
  gns::treversal_result_t<V> forest() const noexcept {
    return gns::make_basic_forest<V>(rows);
  }
};

static KzgSym kzg_symmetric(std::mt19937_64 &rng, ulen n, ulen medges,
                            ulen self_pct = 10) {
  KzgSym S{};
  S.n = n;
  S.rows.assign(n, {});
  S.slot_edge.assign(n, {});
  const ulen m = kz_pick(rng, medges + 1);
  for (ulen k = 0; k < m; ++k) {
    ulen a = kz_pick(rng, n), b = kz_pick(rng, n);
    if (a == b && !kz_chance(rng, int(self_pct)))
      b = kz_pick(rng, n); // self loops sometimes, not always
    S.edges.push_back({a, b});
    const ulen ei = ulen(S.edges.size() - 1);
    if (a == b) {
      S.rows[a].push_back(a); // the pair representation: two self arcs
      S.slot_edge[a].push_back(ei);
      S.rows[a].push_back(a);
      S.slot_edge[a].push_back(ei);
    } else {
      S.rows[a].push_back(b);
      S.slot_edge[a].push_back(ei);
      S.rows[b].push_back(a);
      S.slot_edge[b].push_back(ei);
    }
  }
  for (ulen u = 0; u < n; ++u)
    for (ulen ei : S.slot_edge[u])
      S.edge_of_arc.push_back(ei);
  return S;
}

// BFS reachability: reach[u][v] = a walk of >= 0 arcs (diagonal true)
static std::vector<std::vector<char>>
kzr_reach0(const std::vector<std::vector<ulen>> &succs) {
  const ulen n = ulen(succs.size());
  std::vector<std::vector<char>> reach(n, std::vector<char>(n, 0));
  for (ulen s = 0; s < n; ++s) {
    std::vector<ulen> wl{s};
    reach[s][s] = 1;
    while (!wl.empty()) {
      ulen u = wl.back();
      wl.pop_back();
      for (ulen v : succs[u])
        if (v < n && !reach[s][v]) {
          reach[s][v] = 1;
          wl.push_back(v);
        }
    }
  }
  return reach;
}

// reach1: a walk of >= 1 arc (the arc-set layer's convention: the
// diagonal means "on a cycle" -- a self loop counts, so the diagonal
// is recomputed from the UNZEROED reach0)
static std::vector<std::vector<char>>
kzr_reach1(const std::vector<std::vector<ulen>> &succs) {
  const auto r0 = kzr_reach0(succs);
  auto r1 = r0;
  const ulen n = ulen(succs.size());
  for (ulen u = 0; u < n; ++u)
    r1[u][u] = 0;
  for (ulen u = 0; u < n; ++u)
    for (ulen v : succs[u])
      if (v < n)
        r1[u][u] = char(r1[u][u] || r0[v][u]); // u -> v ->* u
  return r1;
}

// SCC classes by mutual reachability; comp_of filled per node
static void kzr_scc_classes(const std::vector<std::vector<ulen>> &succs,
                            std::vector<ulen> &comp_of,
                            std::vector<std::vector<ulen>> &classes) {
  const ulen n = ulen(succs.size());
  auto reach = kzr_reach0(succs);
  comp_of.assign(n, kz_none());
  classes.clear();
  for (ulen u = 0; u < n; ++u) {
    if (comp_of[u] != kz_none())
      continue;
    classes.push_back({});
    for (ulen v = u; v < n; ++v)
      if (comp_of[v] == kz_none() && reach[u][v] && reach[v][u]) {
        comp_of[v] = ulen(classes.size() - 1);
        classes.back().push_back(v);
      }
  }
}

// the directed simple cycles as node walks, each enumerated once from
// its smallest node (edge identities are the caller's business: for
// weighted verdicts pair the walk with specific arcs)
static void kzr_dfs_cycles_(const std::vector<std::vector<ulen>> &succs, ulen s,
                            ulen cur, std::vector<char> &vis,
                            std::vector<ulen> &walk,
                            std::vector<std::vector<ulen>> &out) {
  for (ulen v : succs[cur]) {
    if (v >= succs.size())
      continue;
    if (v == s) {
      if (walk.size() >= 1) // >= 2 arcs (self loops are not cycles here)
        out.push_back(walk);
      continue;
    }
    if (v < s || vis[v]) // s stays the smallest; nodes visited once
      continue;
    vis[v] = 1;
    walk.push_back(v);
    kzr_dfs_cycles_(succs, s, v, vis, walk, out);
    walk.pop_back();
    vis[v] = 0;
  }
}
static std::vector<std::vector<ulen>>
kzr_directed_simple_cycles(const std::vector<std::vector<ulen>> &succs) {
  const ulen n = ulen(succs.size());
  std::vector<std::vector<ulen>> out{};
  std::vector<char> vis(n, 0);
  std::vector<ulen> walk{};
  for (ulen s = 0; s < n; ++s) {
    walk.push_back(s);
    kzr_dfs_cycles_(succs, s, s, vis, walk, out);
    walk.pop_back();
  }
  return out;
}

// min weight of a SIMPLE path entry->v, by node mask (Held-Karp; no
// relaxation -- a definition with a loop bound). unreachable = none
static std::vector<int64_t>
kzr_simple_path_min(const std::vector<std::vector<ulen>> &succs,
                    const std::vector<KzgArc> &arcs,
                    const std::vector<int64_t> &w, ulen entry) {
  const ulen n = ulen(succs.size());
  constexpr int64_t none_ = 0x7fffffffffffffff;
  std::vector<int64_t> best(n, none_);
  if (entry >= n)
    return best;
  // out arcs per node, in arc order
  std::vector<std::vector<ulen>> out_arcs(n);
  for (ulen e = 0; e < arcs.size(); ++e)
    out_arcs[arcs[e].from].push_back(e);
  const ulen full = (ulen(1) << n);
  // best[mask][v]: the min weight of an entry->v simple path whose
  // node set is exactly mask (bit v set, bit entry set)
  std::vector<int64_t> dp(full * n, none_);
  dp[(ulen(1) << entry) * n + entry] = 0;
  for (ulen mask = 0; mask < full; ++mask) {
    if (!(mask & (ulen(1) << entry)))
      continue;
    for (ulen v = 0; v < n; ++v) {
      const int64_t d = dp[mask * n + v];
      if (d == none_ || !(mask & (ulen(1) << v)))
        continue;
      if (d < best[v])
        best[v] = d;
      for (ulen e : out_arcs[v]) {
        const ulen t = arcs[e].to;
        if (mask & (ulen(1) << t))
          continue;
        const int64_t nd = d + w[e];
        if (nd < dp[(mask | (ulen(1) << t)) * n + t])
          dp[(mask | (ulen(1) << t)) * n + t] = nd;
      }
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// 1. gns.invfam: the sixteen inverse universe members
// ---------------------------------------------------------------------------
static void fz_invfam(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 7);
  const ulen uni = kz_chance(rng, 50) ? n : 1 + kz_pick(rng, 9);
  // the square-shaped relation (entries < n) and the general universe
  // relation; payloads are per entry
  std::vector<std::vector<ulen>> sq(n), rows(n), sqd(n), rwd(n);
  for (ulen u = 0; u < n; ++u) {
    const int k = int(kz_pick(rng, 4));
    for (int j = 0; j < k; ++j) {
      const ulen e1 = kz_pick(rng, n);
      const ulen e2 = kz_pick(rng, uni);
      const ulen d1 = kz_pick(rng, 1000);
      const ulen d2 = 1000 + kz_pick(rng, 1000);
      sq[u].push_back(e1);
      sqd[u].push_back(d1);
      rows[u].push_back(e2);
      rwd[u].push_back(d2);
    }
  }
  // the definitional transpose: per entry, the source rows ascending,
  // their payloads, and the linear source-entry indices
  auto transpose_ref = [](const std::vector<std::vector<ulen>> &rel,
                          const std::vector<std::vector<ulen>> &dat,
                          ulen universe) {
    struct Ref {
      std::vector<std::vector<ulen>> src{}, pay{}, lin{};
    };
    Ref ref{};
    ref.src.assign(universe, {});
    ref.pay.assign(universe, {});
    ref.lin.assign(universe, {});
    ulen lin = 0;
    for (ulen u = 0; u < rel.size(); ++u)
      for (ulen j = 0; j < rel[u].size(); ++j, ++lin) {
        ref.src[rel[u][j]].push_back(u);
        ref.pay[rel[u][j]].push_back(dat[u][j]);
        ref.lin[rel[u][j]].push_back(lin);
      }
    return ref;
  };
  // one member check: rows in the fill order, payloads and the
  // bijection alongside (backward fills land DESCENDING, so the
  // payload at slot j is the reference's slot m-1-j)
  auto check_member = [&](const char *what, const gns::treversal_result_t<V> &f,
                          const std::vector<ulen> *pay,
                          const std::vector<ulen> *bij, const auto &ref,
                          bool forward) {
    for (ulen e = 0; e < ref.src.size(); ++e) {
      std::vector<ulen> w = ref.src[e];
      if (!forward)
        std::reverse(w.begin(), w.end());
      auto r = f.range()[e];
      bool eq = ulen(r.size()) == w.size();
      for (ulen j = 0; j < w.size() && eq; ++j)
        eq = (r[j] == w[j]);
      R.ok(eq, what);
      const ulen m_ = ref.src[e].size();
      if (pay) {
        eq = pay->size() >= m_;
        for (ulen j = 0; j < m_ && eq; ++j)
          eq = (*pay)[ulen(f.nodes_index[e]) + j] ==
               ref.pay[e][forward ? j : m_ - 1 - j];
        R.ok(eq, what);
      }
      if (bij) {
        eq = true;
        for (ulen j = 0; j < m_ && eq; ++j)
          eq = (*bij)[ulen(f.nodes_index[e]) + j] ==
               ref.lin[e][forward ? j : m_ - 1 - j];
        R.ok(eq, what);
      }
    }
  };
  {
    auto ref = transpose_ref(sq, sqd, n);
    auto [f0, b0] = gns::map_make_basic_inv_forest_edges_bijection<V>(sq);
    check_member("invfam: square backward plain+bij", f0, nullptr, &b0, ref,
                 false);
    auto [f1, d1_] =
        gns::map_make_basic_inv_forest_edges_data<V, ulen>(sq, sqd);
    check_member("invfam: square backward data", f1, &d1_, nullptr, ref, false);
    auto [f2, o2] =
        gns::map_make_basic_inv_forest_edges_data_bijection<V, ulen>(sq, sqd);
    check_member("invfam: square backward both", f2, &o2.first, &o2.second, ref,
                 false);
    auto fb = gns::make_basic_inv_forest<V>(sq);
    check_member("invfam: square backward plain", fb, nullptr, nullptr, ref,
                 false);
    auto ff = gns::make_basic_inv_forest_forward<V>(sq);
    check_member("invfam: square forward plain", ff, nullptr, nullptr, ref,
                 true);
    auto [ff1, dff] =
        gns::map_make_basic_inv_forest_edges_data_forward<V, ulen>(sq, sqd);
    check_member("invfam: square forward data", ff1, &dff, nullptr, ref, true);
    auto [ff2, bff] =
        gns::map_make_basic_inv_forest_edges_bijection_forward<V>(sq);
    check_member("invfam: square forward bij", ff2, nullptr, &bff, ref, true);
    auto [ff3, o3] =
        gns::map_make_basic_inv_forest_edges_data_bijection_forward<V, ulen>(
            sq, sqd);
    check_member("invfam: square forward both", ff3, &o3.first, &o3.second, ref,
                 true);
    // the square/universe equivalences: same fill order, same answer
    auto ub = gns::make_basic_inv_forest_universe_backward<V>(sq, n);
    check_member("invfam: universe backward == square backward", ub, nullptr,
                 nullptr, ref, false);
    auto uf = gns::make_basic_inv_forest_universe<V>(sq, n);
    check_member("invfam: universe forward == square forward", uf, nullptr,
                 nullptr, ref, true);
    // the double transpose is the identity relation (row multisets)
    auto rt = gns::make_basic_inv_forest_forward<V>(
        gns::make_basic_inv_forest_forward<V>(sq).range());
    for (ulen u = 0; u < n; ++u) {
      auto r = rt.range()[u];
      std::vector<ulen> a(r.begin(), r.end());
      std::vector<ulen> b = sq[u];
      std::sort(a.begin(), a.end());
      std::sort(b.begin(), b.end());
      R.ok(a == b, "invfam: double transpose restores the relation");
    }
  }
  {
    auto ref = transpose_ref(rows, rwd, uni);
    auto uf = gns::make_basic_inv_forest_universe<V>(rows, uni);
    check_member("invfam: universe forward plain", uf, nullptr, nullptr, ref,
                 true);
    auto ub = gns::make_basic_inv_forest_universe_backward<V>(rows, uni);
    check_member("invfam: universe backward plain", ub, nullptr, nullptr, ref,
                 false);
    auto [f1, d1_] =
        gns::map_make_basic_inv_forest_edges_data_universe<V, ulen>(rows, rwd,
                                                                    uni);
    check_member("invfam: universe forward data", f1, &d1_, nullptr, ref, true);
    auto [f2, d2_] =
        gns::map_make_basic_inv_forest_edges_data_universe_backward<V, ulen>(
            rows, rwd, uni);
    check_member("invfam: universe backward data", f2, &d2_, nullptr, ref,
                 false);
    auto [f3, b3] =
        gns::map_make_basic_inv_forest_edges_bijection_universe<V>(rows, uni);
    check_member("invfam: universe forward bij", f3, nullptr, &b3, ref, true);
    auto [f4, b4] =
        gns::map_make_basic_inv_forest_edges_bijection_universe_backward<V>(
            rows, uni);
    check_member("invfam: universe backward bij", f4, nullptr, &b4, ref, false);
    auto [f5, o5] =
        gns::map_make_basic_inv_forest_edges_data_bijection_universe<V, ulen>(
            rows, rwd, uni);
    check_member("invfam: universe forward both", f5, &o5.first, &o5.second,
                 ref, true);
    auto [f6, o6] =
        gns::map_make_basic_inv_forest_edges_data_bijection_universe_backward<
            V, ulen>(rows, rwd, uni);
    check_member("invfam: universe backward both", f6, &o6.first, &o6.second,
                 ref, false);
  }
}

// ---------------------------------------------------------------------------
// 2. gns.scc: the components vs mutual-reachability classes
// ---------------------------------------------------------------------------
static void fz_scc(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 8);
  auto succs = kzg_digraph(rng, n, 4);
  std::vector<ulen> comp_of{};
  std::vector<std::vector<ulen>> classes{};
  kzr_scc_classes(succs, comp_of, classes);
  auto g = gns::make_basic_forest<V>(succs);
  auto scc = gns::calculate_strongly_connected_components<V>(g);
  // the partition itself: derive comp ids from the library's rows (the
  // library numbers components its own way -- the partition, not the
  // numbering, is the contract)
  std::vector<ulen> got(n, kz_none());
  ulen ci = 0;
  bool sizes_ok = true;
  for (auto &&row : scc.range()) {
    for (ulen u : row) {
      sizes_ok = sizes_ok && u < n && got[u] == kz_none();
      if (u < n)
        got[u] = ci;
    }
    ++ci;
  }
  R.ok(sizes_ok && ci == classes.size(),
       "scc: every node claimed once, the class count matches");
  bool same = sizes_ok && ci == classes.size();
  for (ulen u = 0; u < n && same; ++u)
    for (ulen v = 0; v < n && same; ++v)
      same = (got[u] == got[v]) == (comp_of[u] == comp_of[v]);
  R.ok(same, "scc: the classes are the mutual-reachability partition");
  // each class is exactly one library row (the row's member set is
  // the class -- the ids are the library's, the class index is ours)
  same = true;
  for (auto &&row : scc.range()) {
    std::vector<ulen> a(row.begin(), row.end());
    if (a.empty()) {
      same = false;
      continue;
    }
    std::vector<ulen> b = classes[comp_of[a[0]]];
    std::sort(a.begin(), a.end());
    if (a != b)
      same = false;
  }
  R.ok(same, "scc: each component is one full class, no strays");
}

// ---------------------------------------------------------------------------
// 3. gns.seq: Kahn waves over the condensation, the topo orders
// ---------------------------------------------------------------------------
static void fz_seq(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 8);
  // ---- the DAG half: layered waves + the topological permutation
  {
    auto succs = kzg_dag(rng, n, 3);
    auto g = gns::make_basic_forest<V>(succs);
    // the reference: Kahn layering (in-degree 0 starts wave 1)
    std::vector<ulen> indeg(n, 0);
    for (ulen u = 0; u < n; ++u)
      for (ulen v : succs[u])
        indeg[v]++;
    std::vector<ulen> wave(n, 0), que{};
    for (ulen u = 0; u < n; ++u)
      if (!indeg[u])
        que.push_back(u);
    ulen tail = 0, wcnt = 0;
    while (tail < que.size()) {
      ++wcnt;
      const ulen head = ulen(que.size());
      for (ulen qi = tail; qi < head; ++qi) {
        const ulen u = que[qi];
        wave[u] = wcnt;
        for (ulen v : succs[u])
          if (!--indeg[v])
            que.push_back(v);
      }
      tail = head;
    }
    auto [waves, wc] = gns::calculate_acyclic_graph_sequenced_components<V>(g);
    bool eq = ulen(waves.size()) >= n;
    for (ulen u = 0; u < n && eq; ++u)
      eq = waves[u] == wave[u];
    R.ok(eq && wc == wcnt,
         "seq: DAG waves are the Kahn layers (1-based, all nodes)");
    auto [topo, tc] =
        gns::calculate_acyclic_graph_topological_sort_components<V>(g);
    std::vector<char> seen(n, 0);
    bool perm = topo.size() == n;
    for (ulen i = 0; i < topo.size() && perm; ++i)
      perm = topo[i] < n && !std::exchange(seen[topo[i]], 1);
    R.ok(perm && tc == wcnt, "seq: the topological sort is a permutation");
    std::vector<ulen> pos(n, 0);
    for (ulen i = 0; i < topo.size(); ++i)
      pos[topo[i]] = i;
    bool ord = perm;
    for (ulen u = 0; u < n && ord; ++u)
      for (ulen v : succs[u])
        if (pos[u] >= pos[v])
          ord = false; // every arc runs forward in the order
    R.ok(ord, "seq: every DAG arc runs forward in the topological order");
  }
  // ---- the general half: the condensation's wave queue, node waves
  // with the self-sequential sign, the complemented comp ids
  {
    auto succs = kzg_digraph(rng, n, 3);
    auto g = gns::make_basic_forest<V>(succs);
    std::vector<ulen> comp_of{};
    std::vector<std::vector<ulen>> classes{};
    kzr_scc_classes(succs, comp_of, classes);
    auto scc = gns::calculate_strongly_connected_components<V>(g);
    // the library's comp ids (its rows, its numbering -- kept in the
    // library's member order: the within-wave queue order follows it)
    const ulen nc = ulen(classes.size());
    std::vector<ulen> got(n, kz_none());
    std::vector<std::vector<ulen>> lclasses{};
    bool id_ok = true;
    for (auto &&row : scc.range()) {
      lclasses.push_back({});
      for (ulen u : row) {
        id_ok = id_ok && u < n && got[u] == kz_none();
        if (u < n) {
          got[u] = ulen(lclasses.size() - 1);
          lclasses.back().push_back(u);
        }
      }
    }
    R.ok(id_ok && lclasses.size() == nc,
         "seq: the library's components are one class each");
    // internal arcs (self loops included) flag a component
    std::vector<char> self_seq(nc, 0);
    for (ulen u = 0; u < n; ++u)
      for (ulen v : succs[u])
        if (v < n && got[u] == got[v])
          self_seq[got[u]] = 1;
    // the condensation's in-degrees (per arc, parallels included)
    std::vector<ulen> indeg(nc, 0);
    for (ulen u = 0; u < n; ++u)
      for (ulen v : succs[u])
        if (v < n && got[u] != got[v])
          indeg[got[v]]++;
    std::vector<ulen> cwave(nc, 0), que{};
    for (ulen c = 0; c < nc; ++c)
      if (!indeg[c])
        que.push_back(c);
    ulen tail = 0, wcnt = 0;
    while (tail < que.size()) {
      ++wcnt;
      const ulen head = ulen(que.size());
      for (ulen qi = tail; qi < head; ++qi) {
        const ulen c = que[qi];
        cwave[c] = wcnt;
        for (ulen u : lclasses[c])
          for (ulen v : succs[u])
            if (v < n && got[u] != got[v] && !--indeg[got[v]])
              que.push_back(got[v]);
      }
      tail = head;
    }
    auto [seq, wc] = gns::calculate_graph_sequenced_components<V>(scc, g);
    R.ok(seq.size() == n * 2 + nc && wc == wcnt,
         "seq: the layout size and the wave count match");
    bool eq = seq.size() == n * 2 + nc;
    for (ulen u = 0; u < n && eq; ++u) {
      const intlen_t want =
          self_seq[got[u]] ? -intlen_t(cwave[got[u]]) : intlen_t(cwave[got[u]]);
      eq = seq[u] == want;
    }
    R.ok(eq, "seq: node waves are the comp's wave, negated when cyclic");
    eq = true;
    for (ulen u = 0; u < n && eq; ++u)
      eq = seq[n + u] == intlen_t(~got[u]);
    R.ok(eq, "seq: the [n..2n) tail is the complemented component ids");
    eq = true;
    for (ulen k = 0; k < nc && eq; ++k)
      eq = seq[2 * n + k] == intlen_t(que[k]);
    R.ok(eq, "seq: the wave queue is the condensation's Kahn order");
    // the topo-ordered scc forest: rows permuted into queue order (as
    // MEMBER SETS -- the row order inside a class is the library's)
    auto ordered = gns::calculate_graph_topo_scc_order<V>(scc, g);
    eq = true;
    ulen ci = 0;
    for (auto &&row : ordered.range()) {
      std::vector<ulen> a(row.begin(), row.end());
      std::vector<ulen> b = lclasses[que[ci]];
      std::sort(a.begin(), a.end());
      std::sort(b.begin(), b.end());
      if (a != b)
        eq = false;
      ++ci;
    }
    R.ok(eq && ci == nc,
         "seq: the topo scc order lists the classes in wave order");
  }
}

// ---------------------------------------------------------------------------
// 4. gns.dsu: union-find vs naive labels
// ---------------------------------------------------------------------------
static void fz_dsu(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 24);
  gns::disjoint_set_union_t<V, std::vector<uintlen_t>> dsu{};
  dsu.parent_then_rank.assign(n, 0);
  dsu.init(n);
  std::vector<ulen> label(n, 0);
  for (ulen i = 0; i < n; ++i)
    label[i] = i;
  for (int step = 0; step < 40; ++step) {
    const ulen a = kz_pick(rng, n), b = kz_pick(rng, n);
    const bool were_split = label[a] != label[b];
    R.ok(dsu.find_and_unite_root(a, b) == were_split,
         "dsu: unite reports the split it merged");
    const ulen lab_a = label[a], lab_b = label[b]; // both read BEFORE
    for (ulen i = 0; i < n; ++i)
      if (label[i] == lab_a || label[i] == lab_b)
        label[i] = lab_a; // the naive merge
    R.ok(dsu.is_united(a, b) == (label[a] == label[b]),
         "dsu: is_united matches the labels");
    const ulen c = kz_pick(rng, n);
    bool roots_agree = true;
    for (ulen i = 0; i < n; ++i)
      roots_agree =
          roots_agree && ((label[i] == label[c]) == dsu.is_united(i, c));
    R.ok(roots_agree, "dsu: the whole label state matches the labels");
  }
}

// ---------------------------------------------------------------------------
// 5. gns.search: BFS levels/parents + DFS intervals/edge classes
// ---------------------------------------------------------------------------
static void fz_search(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 8);
  auto succs = kzg_digraph(rng, n, 3);
  auto g = gns::make_basic_forest<V>(succs);
  // ---- BFS: levels 1-based, tree parents, {max level, visited}
  {
    std::vector<uintlen_t> level(n, 0), order(n, 0), par(n, kz_none());
    auto [mx, cnt] = gns::calculate_breadth_first_levels_and_parent<V>(
        level, order, par, g, std::views::iota(ulen(0), n),
        [](auto &&...) noexcept {}, [](auto &&...) noexcept {},
        [](auto &&...) noexcept {});
    // the reference: multi-root BFS in node order, row-order neighbors
    std::vector<ulen> rlevel(n, 0), rpar(n, kz_none());
    std::vector<ulen> que{};
    ulen rmax = 0;
    for (ulen s = 0; s < n; ++s) {
      if (rlevel[s])
        continue;
      rlevel[s] = 1;
      que.push_back(s);
      for (ulen qi = 0; qi < que.size(); ++qi) {
        const ulen u = que[qi];
        for (ulen v : succs[u])
          if (!rlevel[v]) {
            rlevel[v] = rlevel[u] + 1;
            rpar[v] = u;
            que.push_back(v);
          }
      }
    }
    for (ulen u = 0; u < n; ++u)
      rmax = rmax < rlevel[u] ? rlevel[u] : rmax;
    bool eq = mx == rmax && cnt == que.size();
    for (ulen u = 0; u < n && eq; ++u)
      eq = level[u] == rlevel[u] && par[u] == rpar[u];
    R.ok(eq, "search: BFS levels and parents match the plain BFS");
    // the BFS order: nondecreasing in level WITHIN each root's walk
    // (a later root restarts at level 1 after an earlier root's deeper
    // levels -- multi-root order is not globally monotone)
    eq = true;
    ulen prev_level = 0;
    for (ulen i = 0; i < cnt && eq; ++i) {
      const ulen u = order[i];
      eq = level[u] != 0;
      if (par[u] == kz_none())
        prev_level = 0; // a new root segment begins
      eq = eq && level[u] >= prev_level;
      prev_level = level[u];
    }
    R.ok(eq, "search: the BFS order climbs by level per root");
  }
  // ---- DFS: preorder/postorder times, parents, the four edge classes
  {
    std::vector<uintlen_t> begin(n, 0), end(n, 0), par(n, kz_none());
    std::vector<std::pair<ulen, ulen>> tree{}, back{}, fwd{}, cross{};
    auto push = [](std::vector<std::pair<ulen, ulen>> &v) {
      return [&v](ulen a, ulen b) noexcept { v.push_back({a, b}); };
    };
    auto dummy = [](auto &&...) noexcept {};
    gns::calculate_depth_first_intervals<V>(
        begin, end, par, g, std::views::iota(ulen(0), n), dummy, push(tree),
        push(back), push(cross), push(fwd), dummy);
    // the reference: explicit frame stack (node, cursor), roots in id
    // order, children in row order -- same times (1-based), same
    // parents, and the four classes judged AT SCAN TIME (a final-array
    // judgement would misread a parallel arc to a tree child)
    std::vector<ulen> rb(n, 0), re(n, 0), rp(n, kz_none());
    ulen timer = 0;
    std::vector<std::pair<ulen, ulen>> frames{};
    std::vector<std::pair<ulen, ulen>> rtree{}, rback{}, rfwd{}, rcross{};
    for (ulen s = 0; s < n; ++s) {
      if (rb[s])
        continue;
      rp[s] = kz_none();
      frames.push_back({s, 0});
      rb[s] = ++timer;
      while (!frames.empty()) {
        auto &[u, cur] = frames.back();
        if (cur < succs[u].size()) {
          const ulen v = succs[u][cur++];
          if (!rb[v]) {
            rp[v] = u;
            rb[v] = ++timer;
            rtree.push_back({u, v});
            frames.push_back({v, 0});
          } else if (!re[v]) {
            rback.push_back({u, v}); // v open: an ancestor on the stack
          } else if (rb[u] < rb[v]) {
            rfwd.push_back({u, v}); // finished descendant of u
          } else {
            rcross.push_back({u, v}); // entered and left before u
          }
          continue;
        }
        re[u] = ++timer;
        frames.pop_back();
      }
    }
    bool eq = true;
    for (ulen u = 0; u < n && eq; ++u)
      eq = begin[u] == rb[u] && end[u] == re[u] && par[u] == rp[u];
    R.ok(eq, "search: DFS intervals and parents match the plain DFS");
    for (auto *cls : {&rtree, &rback, &rfwd, &rcross})
      std::sort(cls->begin(), cls->end());
    std::sort(tree.begin(), tree.end());
    std::sort(back.begin(), back.end());
    std::sort(fwd.begin(), fwd.end());
    std::sort(cross.begin(), cross.end());
    R.ok(tree == rtree, "search: DFS tree arcs classified right");
    R.ok(back == rback, "search: DFS back arcs classified right");
    R.ok(fwd == rfwd, "search: DFS forward arcs classified right");
    R.ok(cross == rcross, "search: DFS cross arcs classified right");
  }
}

// ---------------------------------------------------------------------------
// 6. gns.dij: Dijkstra + A* vs the simple-path minima
// ---------------------------------------------------------------------------
static void fz_dij(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 7);
  auto succs = kzg_digraph(rng, n, 3);
  auto arcs = kzg_arcs(succs);
  std::vector<int64_t> w(arcs.size());
  for (auto &x : w)
    x = int64_t(kz_pick(rng, 41)); // non-negative (the engine's contract)
  const ulen entry = kz_pick(rng, n);
  auto want = kzr_simple_path_min(succs, arcs, w, entry);
  auto g = gns::make_basic_forest<V>(succs);
  std::vector<uint32_t> w32(w.begin(), w.end());
  auto [par, dist] =
      gns::calculate_shortest_positive_path_distanced<V>(entry, g, w32);
  bool eq = true;
  for (ulen v = 0; v < n && eq; ++v) {
    const bool unwant = want[v] == 0x7fffffffffffffff;
    eq = unwant ? dist[v] == gns::distance_inf<V, uint32_t>()
                : int64_t(dist[v]) == want[v];
  }
  R.ok(eq, "dij: distances are the simple-path minima");
  // the parents: a real arc that closes at the distance
  eq = true;
  for (ulen v = 0; v < n && eq; ++v) {
    if (v == entry || want[v] == 0x7fffffffffffffff) {
      eq = par[v] == kz_none(); // the entry and the unreachable: no parent
      continue;
    }
    const ulen p = par[v];
    bool found = false;
    for (ulen e = 0; e < arcs.size() && !found; ++e)
      found = arcs[e].from == p && arcs[e].to == v &&
              int64_t(dist[p]) + w[e] == int64_t(dist[v]);
    eq = found;
  }
  R.ok(eq, "dij: every parent arc closes the distance equation");
  // ---- A* under consistent heuristics: the returned EDGE path is a
  // shortest entry->exit walk
  const ulen exit = kz_pick(rng, n);
  // the true distance TO exit, per node: the simple-path minima on the
  // REVERSED graph -- with the reversed arcs' own weights (each reverse
  // arc rides its original's weight; the id mapping is carried along,
  // the two arc orders are not the same list)
  std::vector<std::vector<ulen>> rsuccs(n), rids(n);
  for (ulen e = 0; e < arcs.size(); ++e) {
    rsuccs[arcs[e].to].push_back(arcs[e].from);
    rids[arcs[e].to].push_back(e);
  }
  {
    auto r_arcs = kzg_arcs(rsuccs);
    std::vector<int64_t> rw(r_arcs.size());
    {
      ulen k = 0;
      for (ulen v = 0; v < n; ++v)
        for (ulen j = 0; j < rsuccs[v].size(); ++j, ++k)
          rw[k] = w[rids[v][j]];
    }
    auto d_exit = kzr_simple_path_min(rsuccs, r_arcs, rw, exit);
    ulen reach_exit = 1;
    for (ulen v = 0; v < n; ++v)
      if (v != exit && d_exit[v] == 0x7fffffffffffffff)
        reach_exit = 0;
    std::vector<uint32_t> h(n, 0);
    if (reach_exit && kz_chance(rng, 70)) {
      // h = the true remaining distance (or half of it) is CONSISTENT
      // (the triangle inequality halves well on non-negative weights);
      // h = 0 when some node cannot reach the exit -- the fallback that
      // is consistent by vacuity
      const bool half = kz_chance(rng, 50);
      for (ulen v = 0; v < n; ++v)
        if (d_exit[v] != 0x7fffffffffffffff)
          h[v] = half ? uint32_t(d_exit[v] / 2) : uint32_t(d_exit[v]);
    }
    auto path = gns::calculate_A_start_path<V>(entry, exit, g, w32, h);
    if (entry == exit) {
      R.ok(path.empty(), "dij: A* returns the empty path for a trivial ask");
    } else if (want[exit] == 0x7fffffffffffffff) {
      R.ok(path.empty(), "dij: A* returns empty when the exit is cut off");
    } else {
      // the walk: entry -> head(e1) -> ... -> exit, the minimum weight
      bool ok = !path.empty();
      int64_t total = 0;
      ulen cur = entry;
      for (ulen e : path) {
        ok = ok && e < arcs.size() && arcs[e].from == cur;
        if (!ok)
          break;
        total += w[e];
        cur = arcs[e].to;
      }
      R.ok(ok && cur == exit && total == want[exit],
           "dij: A*'s edge path is a shortest path");
    }
  }
}

// ---------------------------------------------------------------------------
// 7. gns.flow: Dinic vs the 2^(n-2) cut enumeration + the certificate
// ---------------------------------------------------------------------------
static void fz_flow(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 2 + kz_pick(rng, 5); // s = 0, t = n-1
  const ulen s = 0, t = n - 1;
  auto succs = kzg_digraph(rng, n, 3);
  auto arcs = kzg_arcs(succs);
  const ulen m = arcs.size();
  std::vector<uint32_t> caps(m);
  for (auto &c : caps)
    c = uint32_t(kz_pick(rng, 31));
  // the brute force: every S (s in, t out) -- the minimum cut
  uint32_t min_cut = ~uint32_t(0);
  for (ulen bits = 0; bits < (ulen(1) << (n - 2)); ++bits) {
    std::vector<char> in_s(n, 0);
    in_s[s] = 1;
    for (ulen u = 1; u + 1 < n; ++u)
      in_s[u] = (bits >> (u - 1)) & 1;
    uint32_t cut = 0;
    for (ulen e = 0; e < m; ++e)
      if (in_s[arcs[e].from] && !in_s[arcs[e].to])
        cut += caps[e];
    min_cut = cut < min_cut ? cut : min_cut;
  }
  auto g = gns::make_basic_forest<V>(succs);
  auto fl = gns::calculate_maximum_flow<V>(s, t, g, caps);
  R.ok(fl.flow == min_cut, "flow: the value is the minimum s-t cut");
  // capacity + conservation + the self-loop neutrality
  bool ok = fl.edge_flow.size() == m;
  for (ulen e = 0; e < m && ok; ++e) {
    ok = fl.edge_flow[e] <= caps[e];
    if (arcs[e].from == arcs[e].to)
      ok = ok && fl.edge_flow[e] == 0; // a self loop never carries
  }
  R.ok(ok, "flow: capacity respected, self loops idle");
  ok = true;
  for (ulen v = 0; v < n && ok; ++v) {
    if (v == s || v == t)
      continue;
    int64_t net = 0;
    for (ulen e = 0; e < m; ++e) {
      if (arcs[e].to == v)
        net += fl.edge_flow[e];
      if (arcs[e].from == v && arcs[e].to != v)
        net -= fl.edge_flow[e];
    }
    ok = net == 0;
  }
  R.ok(ok, "flow: conservation at every inner node");
  // the certificate: the reported side is a fully saturated cut
  ok = fl.cut_side.size() == n && fl.cut_side[s] && !fl.cut_side[t];
  for (ulen e = 0; e < m && ok; ++e) {
    if (fl.cut_side[arcs[e].from] && !fl.cut_side[arcs[e].to])
      ok = fl.edge_flow[e] == caps[e]; // forward arcs saturated
    else if (!fl.cut_side[arcs[e].from] && fl.cut_side[arcs[e].to])
      ok = fl.edge_flow[e] == 0; // backward arcs empty
  }
  R.ok(ok, "flow: the cut side is saturated (the optimality witness)");
}

// ---------------------------------------------------------------------------
// 8. gns.stcut: the s-t cut against the same enumeration
// ---------------------------------------------------------------------------
static void fz_stcut(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 2 + kz_pick(rng, 5);
  const ulen s = 0, t = n - 1;
  auto succs = kzg_digraph(rng, n, 3);
  auto arcs = kzg_arcs(succs);
  const ulen m = arcs.size();
  std::vector<uint32_t> caps(m);
  for (auto &c : caps)
    c = uint32_t(kz_pick(rng, 31));
  uint32_t min_cut = ~uint32_t(0);
  for (ulen bits = 0; bits < (ulen(1) << (n - 2)); ++bits) {
    std::vector<char> in_s(n, 0);
    in_s[s] = 1;
    for (ulen u = 1; u + 1 < n; ++u)
      in_s[u] = (bits >> (u - 1)) & 1;
    uint32_t cut = 0;
    for (ulen e = 0; e < m; ++e)
      if (in_s[arcs[e].from] && !in_s[arcs[e].to])
        cut += caps[e];
    min_cut = cut < min_cut ? cut : min_cut;
  }
  auto g = gns::make_basic_forest<V>(succs);
  auto cut = gns::calculate_st_min_cut<V>(s, t, g, caps);
  R.ok(cut.weight == min_cut, "stcut: the weight is the minimum cut");
  R.ok(cut.source_side.size() == n && cut.source_side[s] && !cut.source_side[t],
       "stcut: the side separates s from t");
  // the crossing arcs are exactly the cut edges, and they carry it all
  std::vector<ulen> want{};
  for (ulen e = 0; e < m; ++e)
    if (cut.source_side[arcs[e].from] && !cut.source_side[arcs[e].to])
      want.push_back(e);
  R.ok(cut.cut_edges == want, "stcut: the edge list is the crossing set");
  uint32_t sum = 0;
  for (ulen e : want)
    sum += caps[e];
  R.ok(sum == cut.weight, "stcut: the crossing arcs sum to the weight");
}

// ---------------------------------------------------------------------------
// 9. gns.mcf: min-cost flow -- feasibility, cost accounting, and the
// no-negative-residual-cycle optimality certificate
// ---------------------------------------------------------------------------
static void fz_mcf(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 2 + kz_pick(rng, 4);
  const ulen s = 0, t = n - 1;
  auto succs = kzg_digraph(rng, n, 3);
  auto arcs = kzg_arcs(succs);
  const ulen m = arcs.size();
  std::vector<uint32_t> caps(m);
  std::vector<int64_t> cost(m);
  for (ulen e = 0; e < m; ++e) {
    caps[e] = uint32_t(kz_pick(rng, 6));
    cost[e] = int64_t(kz_pick(rng, 19)) - 9; // negatives legal
  }
  // the contract: the INPUT costs must not form a negative cycle --
  // ANYWHERE, not merely near the source (a cycle the source cannot
  // reach still makes the true optimum unbounded: circulation around
  // it lowers cost forever, so the certificate below would rightly
  // complain about a flow no algorithm can price). A cycle is weighed
  // by the CHEAPEST arc per consecutive pair (a negative closed walk
  // exists iff some choice of parallel arcs sums negative); such
  // instances are sanitized, the SPFA has no answer
  auto cycles = kzr_directed_simple_cycles(succs);
  bool bad = false;
  for (auto &c : cycles) {
    int64_t wsum = 0;
    for (ulen i = 0; i < c.size(); ++i) {
      const ulen a = c[i], b = c[(i + 1) % c.size()];
      int64_t best = 0x7fffffffffffffff;
      for (ulen e = 0; e < m; ++e)
        if (arcs[e].from == a && arcs[e].to == b && cost[e] < best)
          best = cost[e];
      wsum += best;
    }
    if (wsum < 0)
      bad = true; // a negative cycle: out of contract
  }
  if (bad)
    for (auto &c : cost)
      c = c < 0 ? -c : c; // sanitize: keep the instance, drop the risk
  auto g = gns::make_basic_forest<V>(succs);
  // the flow value reference: the brute-force min cut
  uint32_t min_cut = ~uint32_t(0);
  for (ulen bits = 0; bits < (ulen(1) << (n - 2)); ++bits) {
    std::vector<char> in_s(n, 0);
    in_s[s] = 1;
    for (ulen u = 1; u + 1 < n; ++u)
      in_s[u] = (bits >> (u - 1)) & 1;
    uint32_t cut = 0;
    for (ulen e = 0; e < m; ++e)
      if (in_s[arcs[e].from] && !in_s[arcs[e].to])
        cut += caps[e];
    min_cut = cut < min_cut ? cut : min_cut;
  }
  auto mc = gns::calculate_min_cost_flow<V>(s, t, g, caps, cost);
  R.ok(mc.flow == min_cut, "mcf: the unlimited run carries the max flow");
  bool ok = mc.edge_flow.size() == m;
  for (ulen e = 0; e < m && ok; ++e)
    ok = mc.edge_flow[e] <= caps[e];
  R.ok(ok, "mcf: capacity respected");
  ok = true;
  for (ulen v = 0; v < n && ok; ++v) {
    if (v == s || v == t)
      continue;
    int64_t net = 0;
    for (ulen e = 0; e < m; ++e) {
      if (arcs[e].to == v)
        net += mc.edge_flow[e];
      if (arcs[e].from == v && arcs[e].to != v)
        net -= mc.edge_flow[e];
    }
    ok = net == 0;
  }
  R.ok(ok, "mcf: conservation at every inner node");
  int64_t csum = 0;
  for (ulen e = 0; e < m; ++e)
    csum += int64_t(mc.edge_flow[e]) * cost[e];
  R.ok(csum == mc.cost, "mcf: the reported cost is the flow's cost");
  // THE CERTIFICATE: rebuild the residual from edge_flow (nothing
  // internal), Bellman-Ford it from a virtual source -- a negative
  // cycle would be an improving rerouting, so there must be none
  {
    struct RArc {
      ulen from{}, to{};
      int64_t w{};
    };
    std::vector<RArc> res{};
    for (ulen e = 0; e < m; ++e) {
      const auto [a, b] = arcs[e];
      if (a == b)
        continue;
      if (mc.edge_flow[e] < caps[e])
        res.push_back({a, b, cost[e]}); // forward headroom
      if (mc.edge_flow[e] > 0)
        res.push_back({b, a, -cost[e]}); // cancellable flow
    }
    // Bellman-Ford from a virtual node feeding everything (a source
    // component reaches all cycles): n+1 rounds over res
    const ulen vn = n + 1;
    std::vector<int64_t> dist(vn, 0); // the virtual source: all zero
    bool neg = false;
    for (ulen round = 0; round < vn; ++round) {
      bool ch = false;
      for (const auto &ra : res)
        if (dist[ra.from] + ra.w < dist[ra.to]) {
          dist[ra.to] = dist[ra.from] + ra.w;
          ch = true;
        }
      if (!ch)
        break;
      if (round + 1 == vn)
        neg = true; // still moving after |V| rounds: a negative cycle
    }
    R.ok(!neg, "mcf: the residual hides no improving cycle (optimal)");
  }
  // the limited run: the same optimality at the requested value
  if (min_cut > 1) {
    const uint32_t lim = uint32_t(1 + kz_pick(rng, min_cut));
    auto ml = gns::calculate_min_cost_flow<V>(s, t, g, caps, cost, lim);
    R.ok(ml.flow == (lim < min_cut ? lim : min_cut),
         "mcf: the limited run stops at the limit");
    ok = true;
    for (ulen e = 0; e < m && ok; ++e)
      ok = ml.edge_flow[e] <= caps[e];
    R.ok(ok, "mcf: the limited run respects capacity");
    csum = 0;
    for (ulen e = 0; e < m; ++e)
      csum += int64_t(ml.edge_flow[e]) * cost[e];
    R.ok(csum == ml.cost, "mcf: the limited run's cost is its own");
    struct RArc {
      ulen from{}, to{};
      int64_t w{};
    };
    std::vector<RArc> res{};
    for (ulen e = 0; e < m; ++e) {
      const auto [a, b] = arcs[e];
      if (a == b)
        continue;
      if (ml.edge_flow[e] < caps[e])
        res.push_back({a, b, cost[e]});
      if (ml.edge_flow[e] > 0)
        res.push_back({b, a, -cost[e]});
    }
    const ulen vn = n + 1;
    std::vector<int64_t> dist(vn, 0);
    bool neg = false;
    for (ulen round = 0; round < vn; ++round) {
      bool ch = false;
      for (const auto &ra : res)
        if (dist[ra.from] + ra.w < dist[ra.to]) {
          dist[ra.to] = dist[ra.from] + ra.w;
          ch = true;
        }
      if (!ch)
        break;
      if (round + 1 == vn)
        neg = true;
    }
    R.ok(!neg, "mcf: the limited residual hides no improving cycle");
  }
}

// ---------------------------------------------------------------------------
// 10. gns.gcut: Stoer-Wagner vs the 2^(n-1) bipartition sweep
// ---------------------------------------------------------------------------
static void fz_gcut(std::mt19937_64 &rng, KzRec &R) {
  KzgSym S = kzg_symmetric(rng, 1 + kz_pick(rng, 6), 10, 15);
  const ulen n = S.n;
  std::vector<uint32_t> ew(S.edges.size());
  for (auto &w : ew)
    w = uint32_t(kz_pick(rng, 21));
  // the flattened arc weights (each edge's weight on both its arcs)
  std::vector<uint32_t> arcw{};
  for (ulen u = 0; u < n; ++u)
    for (ulen ei : S.slot_edge[u])
      arcw.push_back(ew[ei]);
  // the brute force: every nonempty proper side, each edge once
  uint32_t want = ~uint32_t(0);
  bool any = n > 1;
  for (ulen bits = 1; bits + 1 < (ulen(1) << n) && any; ++bits) {
    std::vector<char> side(n, 0);
    for (ulen u = 0; u < n; ++u)
      side[u] = (bits >> u) & 1;
    uint32_t cut = 0;
    for (ulen ei = 0; ei < S.edges.size(); ++ei)
      if (side[S.edges[ei].first] != side[S.edges[ei].second])
        cut += ew[ei];
    want = cut < want ? cut : want;
  }
  auto g = S.forest();
  auto got = gns::calculate_global_min_cut_undirected<V>(g, arcw);
  if (n <= 1) {
    R.ok(got.weight == 0 && got.side.size() == n,
         "gcut: a one-node graph has no cut");
    return;
  }
  R.ok(got.weight == want, "gcut: the weight is the global minimum");
  uint32_t cut = 0;
  for (ulen ei = 0; ei < S.edges.size(); ++ei)
    if (got.side[S.edges[ei].first] != got.side[S.edges[ei].second])
      cut += ew[ei];
  R.ok(cut == got.weight, "gcut: the side realizes the reported weight");
}

// ---------------------------------------------------------------------------
// 11. gns.dgcut: the directed global cut vs the 2^n sweep
// ---------------------------------------------------------------------------
static void fz_dgcut(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 5);
  auto succs = kzg_digraph(rng, n, 3);
  auto arcs = kzg_arcs(succs);
  const ulen m = arcs.size();
  std::vector<uint32_t> caps(m);
  for (auto &c : caps)
    c = uint32_t(kz_pick(rng, 21));
  uint32_t want = ~uint32_t(0);
  if (n > 1)
    for (ulen bits = 1; bits + 1 < (ulen(1) << n); ++bits) {
      std::vector<char> side(n, 0);
      for (ulen u = 0; u < n; ++u)
        side[u] = (bits >> u) & 1;
      uint32_t cut = 0;
      for (ulen e = 0; e < m; ++e)
        if (side[arcs[e].from] && !side[arcs[e].to])
          cut += caps[e];
      want = cut < want ? cut : want;
    }
  auto g = gns::make_basic_forest<V>(succs);
  auto got = gns::calculate_global_min_cut_directed<V>(g, caps);
  if (n <= 1) {
    R.ok(got.weight == 0, "dgcut: a one-node graph has no cut");
    return;
  }
  R.ok(got.weight == want, "dgcut: the weight is the global minimum");
  uint32_t cut = 0;
  for (ulen e = 0; e < m; ++e)
    if (got.side[arcs[e].from] && !got.side[arcs[e].to])
      cut += caps[e];
  R.ok(cut == got.weight, "dgcut: the side realizes the reported weight");
}

// ---------------------------------------------------------------------------
// 12. gns.maxcut: the enumeration, the greedy witness and its guarantee
// ---------------------------------------------------------------------------
static void fz_maxcut(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 6);
  auto succs = kzg_digraph(rng, n, 3);
  auto arcs = kzg_arcs(succs);
  const ulen m = arcs.size();
  std::vector<int64_t> wi(m);
  for (auto &x : wi)
    x = int64_t(kz_pick(rng, 21));
  // the reference: every assignment of sides, every crossing ARC
  int64_t want = 0;
  if (n > 1)
    for (ulen bits = 0; bits < (ulen(1) << n); ++bits) {
      int64_t val = 0;
      for (ulen e = 0; e < m; ++e)
        if (((bits >> arcs[e].from) & 1) != ((bits >> arcs[e].to) & 1))
          val += wi[e];
      want = val > want ? val : want;
    }
  auto g = gns::make_basic_forest<V>(succs);
  auto bf = gns::calculate_max_cut_bruteforce<V>(g, wi);
  R.ok(bf.weight == want, "maxcut: the enumeration finds the optimum");
  int64_t val = 0;
  for (ulen e = 0; e < m; ++e)
    if (bf.side[arcs[e].from] != bf.side[arcs[e].to])
      val += wi[e];
  R.ok(val == bf.weight, "maxcut: the brute side realizes its weight");
  auto gr = gns::calculate_max_cut_greedy<V>(g, wi);
  R.ok(gr.weight <= bf.weight, "maxcut: the greedy never beats the optimum");
  R.ok(2 * gr.weight >= bf.weight,
       "maxcut: the greedy holds at least half the optimum");
  val = 0;
  for (ulen e = 0; e < m; ++e)
    if (gr.side[arcs[e].from] != gr.side[arcs[e].to])
      val += wi[e];
  R.ok(val == gr.weight, "maxcut: the greedy side realizes its weight");
  // local maximality: no single flip improves (both directions tallied)
  bool local = true;
  for (ulen u = 0; u < n && local; ++u) {
    int64_t same = 0, cross = 0;
    for (ulen e = 0; e < m; ++e) {
      if (arcs[e].from == u || arcs[e].to == u) {
        const ulen o = arcs[e].from == u ? arcs[e].to : arcs[e].from;
        if (arcs[e].from == u && arcs[e].to == u)
          continue; // self loops never cross
        if (gr.side[o] == gr.side[u])
          same += wi[e];
        else
          cross += wi[e];
      }
    }
    local = same <= cross;
  }
  R.ok(local, "maxcut: the greedy cut is flip-maximal");
}

// ---------------------------------------------------------------------------
// 13. gns.gh: the Gomory-Hu tree asked every pair
// ---------------------------------------------------------------------------
static void fz_gh(std::mt19937_64 &rng, KzRec &R) {
  KzgSym S = kzg_symmetric(rng, 2 + kz_pick(rng, 4), 8, 10);
  const ulen n = S.n;
  std::vector<uint32_t> ew(S.edges.size());
  for (auto &w : ew)
    w = uint32_t(kz_pick(rng, 11));
  std::vector<uint32_t> arcw{};
  for (ulen u = 0; u < n; ++u)
    for (ulen ei : S.slot_edge[u])
      arcw.push_back(ew[ei]);
  auto g = S.forest();
  auto tree = gns::calculate_gomory_hu_tree<V>(g, arcw);
  R.ok(tree.parent.size() == n && tree.weight.size() == n,
       "gh: the tree is sized");
  bool struct_ok = tree.parent.size() == n && tree.parent[0] == 0;
  for (ulen u = 1; u < n && struct_ok; ++u)
    struct_ok = tree.parent[u] < u; // Gusfield's re-parents only lower
  R.ok(struct_ok, "gh: parent[i] < i (the Gusfield shape)");
  R.ok(gns::gomory_hu_query<V>(tree, 0, 0) == 0, "gh: the self query is zero");
  for (ulen u = 0; u < n; ++u)
    for (ulen v = u + 1; v < n; ++v) {
      // the brute force u-v cut: sides containing u, not v
      uint32_t want = ~uint32_t(0);
      for (ulen bits = 0; bits < (ulen(1) << (n - 2)); ++bits) {
        std::vector<char> side(n, 0);
        side[u] = 1;
        ulen shift = 0;
        for (ulen x = 0; x < n; ++x) {
          if (x == u || x == v)
            continue;
          side[x] = (bits >> shift++) & 1;
        }
        uint32_t cut = 0;
        for (ulen ei = 0; ei < S.edges.size(); ++ei)
          if (side[S.edges[ei].first] != side[S.edges[ei].second])
            cut += ew[ei];
        want = cut < want ? cut : want;
      }
      R.ok(gns::gomory_hu_query<V>(tree, u, v) == want,
           "gh: the pair query is the pair's minimum cut");
    }
}

// ---------------------------------------------------------------------------
// 14. gns.mst: three algorithms, one unique total, valid forests
// ---------------------------------------------------------------------------
static void fz_mst(std::mt19937_64 &rng, KzRec &R) {
  KzgSym S = kzg_symmetric(rng, 1 + kz_pick(rng, 6), 12, 10);
  const ulen n = S.n;
  std::vector<uint32_t> ew(S.edges.size());
  for (auto &w : ew)
    w = uint32_t(kz_pick(rng, 31));
  std::vector<uint32_t> arcw{};
  for (ulen u = 0; u < n; ++u)
    for (ulen ei : S.slot_edge[u])
      arcw.push_back(ew[ei]);
  // the reference: Prim over the undirected edge list (the total is
  // the unique minimum whatever the tie-breaks)
  uint32_t want = 0;
  std::vector<std::vector<ulen>> adj(n);
  for (ulen ei = 0; ei < S.edges.size(); ++ei) {
    if (S.edges[ei].first == S.edges[ei].second)
      continue;
    adj[S.edges[ei].first].push_back(ei);
    adj[S.edges[ei].second].push_back(ei);
  }
  {
    std::vector<char> intree(n, 0);
    for (ulen s = 0; s < n; ++s) {
      if (intree[s])
        continue;
      intree[s] = 1;
      for (;;) {
        uint32_t best = ~uint32_t(0);
        ulen pick = kz_none();
        for (ulen u = 0; u < n; ++u) {
          if (!intree[u])
            continue;
          for (ulen ei : adj[u]) {
            const ulen o =
                S.edges[ei].first == u ? S.edges[ei].second : S.edges[ei].first;
            if (intree[o])
              continue;
            if (ew[ei] < best) {
              best = ew[ei];
              pick = o;
            }
          }
        }
        if (pick == kz_none())
          break;
        intree[pick] = 1;
        want += best;
      }
    }
  }
  // the undirected component count (for the forest edge count)
  std::vector<ulen> comp(n, kz_none());
  ulen ncomp = 0;
  for (ulen s = 0; s < n; ++s) {
    if (comp[s] != kz_none())
      continue;
    std::vector<ulen> wl{s};
    comp[s] = ncomp;
    while (!wl.empty()) {
      const ulen u = wl.back();
      wl.pop_back();
      for (ulen ei : adj[u]) {
        const ulen o =
            S.edges[ei].first == u ? S.edges[ei].second : S.edges[ei].first;
        if (comp[o] == kz_none()) {
          comp[o] = ncomp;
          wl.push_back(o);
        }
      }
    }
    ++ncomp;
  }
  auto g = S.forest();
  auto check_one =
      [&](const char *what,
          const gns::minimum_spanning_forest_result_t<V, uint32_t> &r) {
        R.ok(r.total == want, what);
        R.ok(r.edges.size() == n - ncomp,
             "mst: the forest spans every component");
        // distinct undirected edges, no self loops, acyclic, weights add up
        bool ok = true;
        std::vector<std::pair<ulen, ulen>> pairs{};
        uint32_t sum = 0;
        gns::disjoint_set_union_t<V, std::vector<uintlen_t>> dsu{};
        dsu.parent_then_rank.assign(n, 0);
        dsu.init(n);
        for (ulen e : r.edges) {
          ok = ok && e < S.edge_of_arc.size();
          if (!ok)
            break;
          const ulen ei = S.edge_of_arc[e];
          const auto [a, b] = S.edges[ei];
          ok = ok && a != b; // self loops never enter
          pairs.push_back({a < b ? a : b, a < b ? b : a});
          sum += ew[ei];
          dsu.find_and_unite_root(a, b);
        }
        std::sort(pairs.begin(), pairs.end());
        for (ulen i = 1; i < pairs.size() && ok; ++i)
          ok = pairs[i] != pairs[i - 1]; // each undirected edge once
        R.ok(ok && sum == r.total, "mst: the edges are distinct and weigh in");
        ulen joined = 0;
        for (ulen e : r.edges) {
          const ulen ei = S.edge_of_arc[e];
          if (dsu.find_and_unite_root(S.edges[ei].first, S.edges[ei].second))
            ++joined;
        }
        R.ok(joined == 0, "mst: the chosen edges close no cycle");
      };
  check_one("mst: kruskal total is the unique minimum",
            gns::calculate_minimum_spanning_forest_kruskal<V>(g, arcw));
  check_one("mst: prim total is the unique minimum",
            gns::calculate_minimum_spanning_forest_prim<V>(g, arcw));
  check_one("mst: boruvka total is the unique minimum",
            gns::calculate_minimum_spanning_forest_boruvka<V>(g, arcw));
}

// ---------------------------------------------------------------------------
// 15. gns.match: Hopcroft-Karp vs Kuhn, the Konig duality
// ---------------------------------------------------------------------------
static void fz_match(std::mt19937_64 &rng, KzRec &R) {
  const ulen nl = 1 + kz_pick(rng, 6);
  const ulen nr = 1 + kz_pick(rng, 6);
  std::vector<std::vector<ulen>> rows(nl);
  for (ulen l = 0; l < nl; ++l) {
    const int k = int(kz_pick(rng, 4));
    for (int j = 0; j < k; ++j) {
      // right ids in range, with junk slots the family tolerates
      if (kz_chance(rng, 85))
        rows[l].push_back(kz_pick(rng, nr));
      else
        rows[l].push_back(nr + kz_pick(rng, 3));
    }
  }
  auto hk = gns::calculate_maximum_bipartite_matching<V>(rows, nr);
  auto ku = gns::calculate_maximum_bipartite_matching_kuhn<V>(rows, nr);
  R.ok(hk.size == ku.size, "match: Hopcroft-Karp and Kuhn agree on the max");
  // validity: matched pairs are edges, the tables are inverses
  auto check_valid = [&](const char *what,
                         const gns::bipartite_matching_result_t<V> &m) {
    bool ok = m.right_of_left.size() == nl && m.left_of_right.size() == nr;
    ulen cnt = 0;
    for (ulen l = 0; l < nl && ok; ++l) {
      const ulen r = m.right_of_left[l];
      if (r == kz_none())
        continue;
      ++cnt;
      ok = r < nr && m.left_of_right[r] == l;
      bool edge = false;
      for (ulen x : rows[l])
        edge = edge || x == r;
      ok = ok && edge;
    }
    R.ok(ok && cnt == m.size, what);
  };
  check_valid("match: the HK tables are inverse edges", hk);
  check_valid("match: the Kuhn tables are inverse edges", ku);
  // the size is MAXIMUM: no augmenting path remains (the brute check
  // -- a bigger matching would need a free pair reachable by
  // alternation; for these sizes: try to grow by one via DFS from
  // every free left over the CURRENT matching)
  {
    // the maximum by a third engine: greedy + augment attempts until
    // stuck (starting from the empty matching, Hopcroft-Karp already
    // proved maximal when it stops -- instead verify via Konig below,
    // whose size equality IS the maximality proof)
    auto cov = gns::calculate_bipartite_minimum_vertex_cover<V>(rows, nr, hk);
    ulen csz = 0;
    for (char c : cov)
      csz += c != 0;
    R.ok(csz == hk.size, "match: the Konig cover matches the matching size");
    bool covered = true;
    for (ulen l = 0; l < nl && covered; ++l)
      for (ulen r : rows[l]) {
        if (r >= nr)
          continue; // junk slots are not edges
        covered = covered && (cov[l] || cov[nl + r]);
      }
    R.ok(covered, "match: the cover touches every edge");
  }
}

// ---------------------------------------------------------------------------
// 16. gns.blossom: the general matching vs bitmask DP
// ---------------------------------------------------------------------------
static void fz_blossom(std::mt19937_64 &rng, KzRec &R) {
  KzgSym S = kzg_symmetric(rng, 1 + kz_pick(rng, 9), 14, 5);
  const ulen n = S.n;
  // the adjacency over undirected edges (self loops never match)
  std::vector<std::vector<char>> adj(n, std::vector<char>(n, 0));
  for (auto [a, b] : S.edges)
    if (a != b)
      adj[a][b] = adj[b][a] = 1;
  // the bitmask DP: dp[mask] = the max matching inside mask
  const ulen full = ulen(1) << n;
  std::vector<ulen> dp(full, 0);
  for (ulen mask = 1; mask < full; ++mask) {
    const ulen low = mask & (~mask + 1); // the lowest set bit
    const ulen lv = 63 - __builtin_clzll(low);
    ulen best = dp[mask ^ low]; // leave lv unmatched
    for (ulen v = 0; v < n; ++v)
      if (v != lv && (mask & (ulen(1) << v)) && adj[lv][v]) {
        const ulen pick = 1 + dp[mask ^ low ^ (ulen(1) << v)];
        best = best < pick ? pick : best;
      }
    dp[mask] = best;
  }
  auto g = S.forest();
  auto got = gns::calculate_maximum_matching<V>(g);
  R.ok(got.size == dp[full - 1], "blossom: the size is the DP maximum");
  // validity: symmetric mates over real edges
  bool ok = got.mate.size() == n;
  ulen cnt = 0;
  for (ulen u = 0; u < n && ok; ++u) {
    const ulen v = got.mate[u];
    if (v == kz_none())
      continue;
    ++cnt;
    ok = v < n && v != u && got.mate[v] == u && adj[u][v];
  }
  R.ok(ok && cnt == 2 * got.size, "blossom: the mates are symmetric edges");
}

// ---------------------------------------------------------------------------
// 17. gns.bic: bridges/articulation/bcc vs cycle enumeration
// ---------------------------------------------------------------------------
// the undirected simple cycles as edge-index walks (each once), the
// definitional co-cycle classes
static void
kzr_und_cycles_(const std::vector<std::vector<std::pair<ulen, ulen>>> &inc,
                ulen s, ulen cur, std::vector<char> &vis,
                std::vector<ulen> &walk, std::vector<std::vector<ulen>> &out) {
  // inc[u]: (other endpoint, edge index) -- self loops excluded by the
  // caller; a cycle closes when an edge back to s appears (that edge
  // is part of the cycle -- the walk carries it too)
  for (auto [v, ei] : inc[cur]) {
    if (v == s) {
      if (!walk.empty()) { // >= 2 edges (parallel pairs included)
        walk.push_back(ei);
        out.push_back(walk);
        walk.pop_back();
      }
      continue;
    }
    if (v < s || vis[v])
      continue;
    vis[v] = 1;
    walk.push_back(ei);
    kzr_und_cycles_(inc, s, v, vis, walk, out);
    walk.pop_back();
    vis[v] = 0;
  }
}
static void fz_bic(std::mt19937_64 &rng, KzRec &R) {
  KzgSym S = kzg_symmetric(rng, 1 + kz_pick(rng, 6), 10, 12);
  const ulen n = S.n;
  const ulen me = S.edges.size();
  // the undirected incidence (self loops excluded: no connectivity)
  std::vector<std::vector<std::pair<ulen, ulen>>> inc(n);
  for (ulen ei = 0; ei < me; ++ei) {
    const auto [a, b] = S.edges[ei];
    if (a == b)
      continue;
    inc[a].push_back({b, ei});
    inc[b].push_back({a, ei});
  }
  // ---- the simple cycles (node-simple, edge-distinct by that very
  // fact; each walk closes back to its smallest node s)
  std::vector<std::vector<ulen>> cycles{};
  {
    std::vector<char> vis(n, 0);
    std::vector<ulen> walk{};
    for (ulen s = 0; s < n; ++s) {
      walk.clear();
      kzr_und_cycles_(inc, s, s, vis, walk, cycles);
    }
    // canonicalize: sort each cycle's edge ids, drop duplicates (each
    // cycle walked once per direction from s)
    for (auto &c : cycles)
      std::sort(c.begin(), c.end());
    std::sort(cycles.begin(), cycles.end());
    cycles.erase(std::unique(cycles.begin(), cycles.end()), cycles.end());
  }
  // ---- the co-cycle classes: union every cycle's edges pairwise
  gns::disjoint_set_union_t<V, std::vector<uintlen_t>> dsu{};
  dsu.parent_then_rank.assign(me, 0);
  dsu.init(me);
  for (auto &c : cycles) {
    for (ulen i = 1; i < c.size(); ++i)
      dsu.find_and_unite_root(c[0], c[i]);
  }
  std::vector<ulen> cls(me, kz_none());
  ulen ncls = 0;
  for (ulen ei = 0; ei < me; ++ei) {
    if (S.edges[ei].first == S.edges[ei].second)
      continue; // self loops carry no connectivity
    const ulen r = dsu.find_and_cache_root(ei);
    if (cls[r] == kz_none())
      cls[r] = ncls++;
    cls[ei] = cls[r];
  }
  // ---- the removal tests: bridges and articulation points
  std::vector<char> is_bridge(me, 0), is_artic(n, 0);
  auto components = [&](const std::vector<char> &node_up,
                        const std::vector<char> &edge_up) {
    // the component count among the nodes still standing: a removed
    // node is a hole -- walks never pass THROUGH it (traversing it
    // would glue both its sides together and mute the very split the
    // removal test exists to see)
    std::vector<ulen> comp(n, kz_none());
    ulen cnt = 0;
    for (ulen s = 0; s < n; ++s) {
      if (node_up[s] || comp[s] != kz_none())
        continue;
      std::vector<ulen> wl{s};
      comp[s] = cnt;
      while (!wl.empty()) {
        const ulen u = wl.back();
        wl.pop_back();
        for (auto [v, ei] : inc[u]) {
          if (edge_up[ei] || node_up[v] || comp[v] != kz_none())
            continue;
          comp[v] = cnt;
          wl.push_back(v);
        }
      }
      ++cnt;
    }
    return cnt;
  };
  {
    std::vector<char> up_n(n, 0), up_e(me, 0);
    const ulen base = components(up_n, up_e);
    for (ulen ei = 0; ei < me; ++ei) {
      if (S.edges[ei].first == S.edges[ei].second)
        continue;
      up_e[ei] = 1;
      is_bridge[ei] = components(up_n, up_e) > base;
      up_e[ei] = 0;
    }
    for (ulen v = 0; v < n; ++v) {
      up_n[v] = 1;
      // components among the REMAINING nodes vs base: removing v
      // disconnects iff the count grows
      is_artic[v] = n > 1 && components(up_n, up_e) > base;
      up_n[v] = 0;
    }
  }
  // ---- the library
  auto g = S.forest();
  auto bi = gns::calculate_biconnectivity<V>(g);
  R.ok(bi.bcc_count == ncls, "bic: the biconnected component count");
  // per arc: bridge flags and class equality against the reference
  bool eq = bi.is_bridge_edge.size() == S.edge_of_arc.size() &&
            bi.bcc_of_edge.size() == S.edge_of_arc.size();
  for (ulen a = 0; a < S.edge_of_arc.size() && eq; ++a) {
    const ulen ei = S.edge_of_arc[a];
    if (S.edges[ei].first == S.edges[ei].second) {
      // self loop arcs: ignored outright (no bridge, no component)
      eq = !bi.is_bridge_edge[a] && bi.bcc_of_edge[a] == kz_none();
      continue;
    }
    eq = bi.is_bridge_edge[a] == is_bridge[ei];
  }
  R.ok(eq, "bic: the bridge arcs match the removal test");
  eq = true;
  for (ulen a = 0; a < S.edge_of_arc.size() && eq; ++a) {
    const ulen ei = S.edge_of_arc[a];
    if (S.edges[ei].first == S.edges[ei].second)
      continue;
    for (ulen b = a + 1; b < S.edge_of_arc.size() && eq; ++b) {
      const ulen ej = S.edge_of_arc[b];
      if (S.edges[ej].first == S.edges[ej].second)
        continue;
      const bool same_ref = cls[ei] == cls[ej];
      const bool same_got = bi.bcc_of_edge[a] == bi.bcc_of_edge[b] &&
                            bi.bcc_of_edge[a] != kz_none();
      eq = same_ref == same_got;
    }
  }
  R.ok(eq, "bic: two arcs share a bcc iff their edges co-cycle");
  eq = true;
  for (ulen v = 0; v < n && eq; ++v)
    eq = bi.is_articulation[v] == is_artic[v];
  R.ok(eq, "bic: the articulation points match the removal test");
  auto br = gns::calculate_bridges<V>(g);
  {
    std::vector<ulen> want{};
    for (ulen a = 0; a < S.edge_of_arc.size(); ++a)
      if (bi.is_bridge_edge[a])
        want.push_back(a);
    R.ok(br == want, "bic: the bridge list is the flagged arcs");
  }
  auto ap = gns::calculate_articulation_points<V>(g);
  R.ok(ap == bi.is_articulation, "bic: the standalone articulation sweep");
}

// ---------------------------------------------------------------------------
// 18. gns.kcore: the degeneracy peel vs the per-k removal definition
// ---------------------------------------------------------------------------
static void fz_kcore(std::mt19937_64 &rng, KzRec &R) {
  KzgSym S = kzg_symmetric(rng, 1 + kz_pick(rng, 7), 14, 10);
  const ulen n = S.n;
  // the undirected adjacency, self loops excluded, multiplicity kept
  std::vector<std::vector<ulen>> adj(n);
  for (auto [a, b] : S.edges)
    if (a != b) {
      adj[a].push_back(b);
      adj[b].push_back(a);
    }
  // the definitional core: the largest k whose k-core (the maximal
  // subgraph with min degree >= k, by iterative removal) keeps v. The
  // sweep runs k up to the EDGE count -- parallel edges push degrees
  // past the node count, and the loop must still find their level
  std::vector<ulen> want(n, 0);
  {
    ulen maxdeg = 0;
    for (ulen u = 0; u < n; ++u)
      maxdeg = maxdeg < adj[u].size() ? adj[u].size() : maxdeg;
    for (ulen k = 0; k <= maxdeg; ++k) {
      std::vector<char> alive(n, 1);
      for (bool ch = true; ch;) {
        ch = false;
        for (ulen u = 0; u < n; ++u) {
          if (!alive[u])
            continue;
          ulen d = 0;
          for (ulen v : adj[u])
            d += alive[v];
          if (d < k) {
            alive[u] = 0;
            ch = true;
          }
        }
      }
      for (ulen u = 0; u < n; ++u)
        if (alive[u])
          want[u] = k;
    }
  }
  auto g = S.forest();
  auto got = gns::calculate_k_cores<V>(g);
  R.ok(got == want, "kcore: the peeling matches the per-k removal");
}

// ---------------------------------------------------------------------------
// 19. gns.euler: the four walks, their conditions and their tours
// ---------------------------------------------------------------------------
static void fz_euler(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 7);
  // ---- the directed family
  {
    // a closed random walk is balanced and one-piece; a raw digraph
    // usually is neither -- both shapes are wanted
    std::vector<std::vector<ulen>> rows(n);
    if (kz_chance(rng, 60)) {
      ulen cur = 0;
      const ulen steps = 1 + kz_pick(rng, 10);
      for (ulen i = 0; i < steps; ++i) {
        const ulen next = kz_pick(rng, n);
        rows[cur].push_back(next);
        cur = next;
      }
      rows[cur].push_back(0); // close the walk
    } else {
      rows = kzg_digraph(rng, n, 3);
    }
    auto arcs = kzg_arcs(rows);
    const ulen m = arcs.size();
    std::vector<ulen> indeg(n, 0), outdeg(n, 0);
    for (auto &a : arcs) {
      outdeg[a.from]++;
      indeg[a.to]++;
    }
    // the one-piece test: weak connectivity over the nodes with arcs
    bool one_piece = true;
    {
      gns::disjoint_set_union_t<V, std::vector<uintlen_t>> dsu{};
      dsu.parent_then_rank.assign(n, 0);
      dsu.init(n);
      std::vector<char> has(n, 0);
      for (auto &a : arcs) {
        has[a.from] = has[a.to] = 1;
        dsu.find_and_unite_root(a.from, a.to);
      }
      ulen anchor = kz_none();
      for (ulen u = 0; u < n; ++u)
        if (has[u]) {
          if (anchor == kz_none())
            anchor = u;
          else
            one_piece = one_piece && dsu.is_united(anchor, u);
        }
    }
    bool balanced = true;
    for (ulen u = 0; u < n; ++u)
      balanced = balanced && indeg[u] == outdeg[u];
    ulen starts = 0, ends = 0;
    for (ulen u = 0; u < n; ++u) {
      if (indeg[u] + 1 == outdeg[u])
        ++starts;
      else if (outdeg[u] + 1 == indeg[u])
        ++ends;
      else if (indeg[u] != outdeg[u])
        starts += 100; // a miscount kills both verdicts below
    }
    auto g = gns::make_basic_forest<V>(rows);
    auto circ = gns::calculate_eulerian_circuit_directed<V>(g);
    R.ok(circ.ok == (balanced && one_piece),
         "euler: the directed circuit verdict matches the conditions");
    auto path = gns::calculate_eulerian_path_directed<V>(g);
    R.ok(path.ok == ((balanced || (starts == 1 && ends == 1)) && one_piece),
         "euler: the directed path verdict matches the conditions");
    // walk the tours the verdicts promised
    auto check_tour = [&](const char *what, const gns::euler_result_t<V> &r,
                          bool close, ulen end_want) {
      if (!r.ok) {
        R.ok(r.edges.empty(), what);
        return;
      }
      bool ok = r.edges.size() == m;
      ulen cur = r.first_node;
      std::vector<ulen> used{};
      for (ulen e : r.edges) {
        ok = ok && e < m && arcs[e].from == cur;
        if (!ok)
          break;
        cur = arcs[e].to;
        used.push_back(e);
      }
      std::sort(used.begin(), used.end());
      for (ulen e = 0; e < m && ok; ++e)
        ok = e < used.size() && used[e] == e; // every arc exactly once
      R.ok(ok, what);
      if (close)
        R.ok(cur == r.first_node, "euler: the circuit closes");
      else
        R.ok(cur == end_want, "euler: the path ends at the odd sink");
    };
    ulen canon_d = 0;
    for (ulen u = 0; u < n; ++u)
      if (outdeg[u]) {
        canon_d = u;
        break;
      }
    if (circ.ok)
      R.ok(circ.first_node == canon_d,
           "euler: the directed circuit starts at the lowest busy node");
    ulen start_want = kz_none();
    for (ulen u = 0; u < n; ++u)
      if (indeg[u] + 1 == outdeg[u])
        start_want = u;
    if (path.ok && !balanced)
      R.ok(path.first_node == start_want,
           "euler: the directed path starts at the odd source");
    ulen end_want = kz_none();
    for (ulen u = 0; u < n; ++u)
      if (outdeg[u] + 1 == indeg[u])
        end_want = u;
    check_tour("euler: the directed circuit walks every arc once", circ, true,
               kz_none());
    check_tour("euler: the directed path walks every arc once", path, false,
               balanced ? (circ.ok ? circ.first_node : kz_none()) : end_want);
  }
  // ---- the undirected family (the pair representation: closed walks
  // over undirected edges give even true degrees and one piece)
  {
    KzgSym S{};
    S.n = n;
    S.rows.assign(n, {});
    S.slot_edge.assign(n, {});
    if (kz_chance(rng, 60)) {
      ulen cur = 0;
      const ulen steps = 1 + kz_pick(rng, 8);
      for (ulen i = 0; i < steps; ++i) {
        const ulen next = kz_pick(rng, n);
        const ulen ei = ulen(S.edges.size());
        S.edges.push_back({cur, next});
        if (cur == next) {
          S.rows[cur].push_back(cur);
          S.slot_edge[cur].push_back(ei);
          S.rows[cur].push_back(cur);
          S.slot_edge[cur].push_back(ei);
        } else {
          S.rows[cur].push_back(next);
          S.slot_edge[cur].push_back(ei);
          S.rows[next].push_back(cur);
          S.slot_edge[next].push_back(ei);
        }
        cur = next;
      }
      const ulen ei = ulen(S.edges.size()); // close the walk
      S.edges.push_back({cur, 0});
      if (cur == 0) {
        // a closing self loop at 0 (still a valid edge)
        S.rows[0].push_back(0);
        S.slot_edge[0].push_back(ei);
        S.rows[0].push_back(0);
        S.slot_edge[0].push_back(ei);
      } else {
        S.rows[cur].push_back(0);
        S.slot_edge[cur].push_back(ei);
        S.rows[0].push_back(cur);
        S.slot_edge[0].push_back(ei);
      }
    } else {
      S = kzg_symmetric(rng, n, 8, 15);
    }
    for (ulen u = 0; u < S.n; ++u)
      for (ulen ei : S.slot_edge[u])
        S.edge_of_arc.push_back(ei);
    const ulen nn = S.n, me = S.edges.size();
    // the true degrees: u's own row, self loops excluded
    std::vector<ulen> deg(nn, 0);
    for (ulen u = 0; u < nn; ++u)
      for (ulen v : S.rows[u])
        if (v != u)
          deg[u]++;
    // the one-piece test: every node holding ANY edge (self loops
    // included -- they mark the node busy) must be weakly connected
    // through the non-loop edges
    bool one_piece = true;
    std::vector<char> has(nn, 0); // busy = holds ANY edge, loops included
    {
      gns::disjoint_set_union_t<V, std::vector<uintlen_t>> dsu{};
      dsu.parent_then_rank.assign(nn, 0);
      dsu.init(nn);
      for (auto [a, b] : S.edges) {
        has[a] = 1;
        if (a != b) {
          has[b] = 1;
          dsu.find_and_unite_root(a, b);
        }
      }
      ulen anchor = kz_none();
      for (ulen u = 0; u < nn; ++u)
        if (has[u]) {
          if (anchor == kz_none())
            anchor = u;
          else
            one_piece = one_piece && dsu.is_united(anchor, u);
        }
    }
    ulen odd = 0, odd_a = kz_none(), odd_b = kz_none();
    for (ulen u = 0; u < nn; ++u)
      if (deg[u] % 2) {
        ++odd;
        if (odd_a == kz_none())
          odd_a = u;
        else
          odd_b = u;
      }
    auto g = S.forest();
    auto circ = gns::calculate_eulerian_circuit_undirected<V>(g);
    R.ok(circ.ok == (odd == 0 && one_piece),
         "euler: the undirected circuit verdict matches the conditions");
    auto path = gns::calculate_eulerian_path_undirected<V>(g);
    R.ok(path.ok == ((odd == 0 || odd == 2) && one_piece),
         "euler: the undirected path verdict matches the conditions");
    auto check_tour = [&](const char *what, const gns::euler_result_t<V> &r,
                          bool close, ulen end_want) {
      if (!r.ok) {
        R.ok(r.edges.empty(), what);
        return;
      }
      // walk: consecutive arcs from the start; one arc per undirected
      // edge (both directions of a pair together == the multiplicity)
      bool ok = true;
      ulen cur = r.first_node;
      std::vector<std::vector<ulen>> used(nn, std::vector<ulen>(nn, 0));
      std::vector<ulen> selfu(nn, 0);
      for (ulen e : r.edges) {
        const ulen a = g.get_edge_from(e), b = g.get_edge_to(e);
        ok = ok && a == cur;
        if (!ok)
          break;
        cur = b;
        if (a == b)
          selfu[a]++;
        else
          used[a][b]++; // direction-recorded
      }
      for (ulen a = 0; a < nn && ok; ++a)
        for (ulen b = a + 1; b < nn && ok; ++b) {
          ulen mult = 0;
          for (ulen ei = 0; ei < me; ++ei)
            if ((S.edges[ei].first == a && S.edges[ei].second == b) ||
                (S.edges[ei].first == b && S.edges[ei].second == a))
              ++mult;
          ok = used[a][b] + used[b][a] == mult; // each edge once
        }
      for (ulen a = 0; a < nn && ok; ++a) {
        ulen selfm = 0;
        for (ulen ei = 0; ei < me; ++ei)
          if (S.edges[ei].first == a && S.edges[ei].second == a)
            ++selfm;
        ok = selfu[a] == selfm; // one arc per self loop edge
      }
      R.ok(ok, what);
      if (close)
        R.ok(cur == r.first_node, "euler: the undirected circuit closes");
      else
        R.ok(cur == end_want, "euler: the undirected path ends right");
    };
    // the canonical starts: the lowest-id BUSY node (self loops mark
    // their node busy -- a loop-only node is a walk's home), the first
    // odd node (the path's)
    ulen canon = 0;
    for (ulen u = 0; u < nn; ++u)
      if (has[u]) {
        canon = u;
        break;
      }
    if (circ.ok)
      R.ok(circ.first_node == canon,
           "euler: the undirected circuit starts canonically");
    ulen end_want = odd == 2 ? odd_b : canon;
    if (path.ok)
      R.ok(path.first_node == (odd == 2 ? odd_a : canon),
           "euler: the undirected path starts at an odd node");
    check_tour("euler: the undirected circuit uses each edge once", circ, true,
               kz_none());
    check_tour("euler: the undirected path uses each edge once", path, false,
               end_want);
  }
}

// ---------------------------------------------------------------------------
// 20. gns.tr: the transitive reduction vs the redundancy rule
// ---------------------------------------------------------------------------
static void fz_tr(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 8);
  auto succs = kzg_dag(rng, n, 3);
  auto arcs = kzg_arcs(succs);
  const ulen m = arcs.size();
  // the reach sets: reach1[w] = the nodes w reaches through >= 1 arc
  auto reach1 = kzr_reach1(succs);
  // (u,v) is redundant iff some OTHER out-arc (u,w) already covers v
  std::vector<char> redundant(m, 0);
  for (ulen e = 0; e < m; ++e) {
    const ulen u = arcs[e].from, v = arcs[e].to;
    for (ulen f = 0; f < m && !redundant[e]; ++f)
      if (f != e && arcs[f].from == u && arcs[f].to != v &&
          reach1[arcs[f].to][v])
        redundant[e] = 1;
  }
  // the keep set: the FIRST arc of each non-redundant (u,v) pair
  std::vector<ulen> want{};
  for (ulen e = 0; e < m; ++e) {
    if (redundant[e])
      continue;
    bool first = true;
    for (ulen f = 0; f < e && first; ++f)
      first = !(arcs[f].from == arcs[e].from && arcs[f].to == arcs[e].to);
    if (first)
      want.push_back(e);
  }
  std::sort(want.begin(), want.end());
  auto g = gns::make_basic_forest<V>(succs);
  auto got = gns::calculate_transitive_reduction_dag<V>(g);
  R.ok(got.keep_edges == want, "tr: the keep set is the unique minimum");
  // the keep set preserves reachability exactly (>=1-arc relation)
  std::vector<std::vector<ulen>> kept(n);
  for (ulen e : got.keep_edges)
    kept[arcs[e].from].push_back(arcs[e].to);
  auto reach_kept = kzr_reach1(kept);
  bool eq = true;
  for (ulen u = 0; u < n && eq; ++u)
    for (ulen v = 0; v < n && eq; ++v)
      eq = reach_kept[u][v] == reach1[u][v];
  R.ok(eq, "tr: the kept arcs preserve reachability exactly");
}

// a small reachability helper on plain rows (used by rmas)
static bool rk_reaches_(const std::vector<std::vector<ulen>> &succs, ulen s,
                        ulen t) {
  std::vector<char> vis(succs.size(), 0);
  std::vector<ulen> wl{s};
  vis[s] = 1;
  while (!wl.empty()) {
    const ulen u = wl.back();
    wl.pop_back();
    if (u == t)
      return true;
    for (ulen v : succs[u])
      if (v < succs.size() && !vis[v]) {
        vis[v] = 1;
        wl.push_back(v);
      }
  }
  return false;
}

// ---------------------------------------------------------------------------
// 21. gns.rmas: the minimum feedback arc set -- any digraph, a weight
// per arc, the least-weight arcs whose removal leaves a DAG -- vs the
// subset enumeration (the definition), Kahn (the FAS-ness), and the
// order certificate
// ---------------------------------------------------------------------------
// acyclicity by Kahn's peeling (the FAS-ness certificate)
static bool kzr_is_dag_(const std::vector<std::vector<ulen>> &rows) {
  const ulen n = rows.size();
  std::vector<ulen> indeg(n, 0);
  for (ulen u = 0; u < n; ++u)
    for (ulen v : rows[u])
      if (v < n)
        indeg[v]++;
  std::vector<ulen> q{};
  for (ulen u = 0; u < n; ++u)
    if (!indeg[u])
      q.push_back(u);
  for (ulen qi = 0; qi < q.size(); ++qi)
    for (ulen v : rows[q[qi]])
      if (v < n && !--indeg[v])
        q.push_back(v);
  return q.size() == n;
}

// the minimum-weight feedback arc set by SUBSET ENUMERATION (the
// definition: the cheapest arc subset whose removal leaves a DAG).
// Exponential on purpose -- the generator keeps m tiny
static ulen kzr_fas_brute_(const std::vector<std::vector<ulen>> &rows,
                           const std::vector<KzgArc> &arcs,
                           const std::vector<ulen> &w) {
  const ulen n = rows.size();
  const ulen m = arcs.size();
  ulen best = 0;
  for (ulen x : w)
    best += x; // dropping everything always works
  std::vector<std::vector<ulen>> kept(n);
  const ulen full = ulen(1) << m;
  for (ulen mask = 0; mask < full; ++mask) {
    ulen cost = 0;
    for (ulen u = 0; u < n; ++u)
      kept[u].clear();
    for (ulen e = 0; e < m; ++e)
      if (mask & (ulen(1) << e))
        kept[arcs[e].from].push_back(arcs[e].to);
      else
        cost += w[e];
    if (cost < best && kzr_is_dag_(kept))
      best = cost;
  }
  return best;
}

static void fz_rmas(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = kz_pick(rng, 8);      // 0..7 nodes (the DP is 2^n)
  auto succs = kzg_digraph(rng, n, 3); // small: the brute force is 2^m
  auto arcs = kzg_arcs(succs);
  const ulen m = arcs.size();
  std::vector<ulen> w(m);
  for (auto &x : w)
    x = kz_pick(rng, 13); // zero weights live here too
  auto g = gns::make_basic_forest<V>(succs);
  auto got = gns::calculate_minimum_feedback_arc_set<V>(g, w);
  // the removed set: a strictly ascending arc subset
  bool ok = got.removed_arcs.size() <= m;
  for (ulen i = 0; i < got.removed_arcs.size() && ok; ++i)
    ok = got.removed_arcs[i] < m &&
         (i == 0 || got.removed_arcs[i] > got.removed_arcs[i - 1]);
  R.ok(ok, "rmas: the removed set is a sorted arc subset");
  // every self loop is a one-arc cycle: it cannot survive
  {
    std::vector<ulen> pos(n, kz_none());
    for (ulen i = 0; i < got.order.size(); ++i)
      pos[got.order[i]] = i;
    ok = got.order.size() == n;
    for (ulen e = 0; e < m && ok; ++e)
      if (arcs[e].from == arcs[e].to)
        ok = std::binary_search(got.removed_arcs.begin(),
                                got.removed_arcs.end(), e);
    R.ok(ok, "rmas: the self loops are all removed");
    // the order: a permutation, topological over the survivors
    ok = got.order.size() == n;
    for (ulen i = 0; i < n && ok; ++i)
      ok = got.order[i] < n &&
           std::count(got.order.begin(), got.order.end(), got.order[i]) == 1;
    for (ulen e = 0; e < m && ok; ++e)
      if (!std::binary_search(got.removed_arcs.begin(), got.removed_arcs.end(),
                              e))
        ok = pos[arcs[e].from] < pos[arcs[e].to];
    R.ok(ok, "rmas: the order is the survivors' topological order");
  }
  // the weight is the sum, and it is a feedback arc set (Kahn)
  {
    ulen tw = 0;
    for (ulen e : got.removed_arcs)
      tw += w[e];
    R.ok(ulen(got.total_weight) == tw, "rmas: the total weight is the sum");
    std::vector<std::vector<ulen>> kept(n);
    for (ulen e = 0; e < m; ++e)
      if (!std::binary_search(got.removed_arcs.begin(), got.removed_arcs.end(),
                              e))
        kept[arcs[e].from].push_back(arcs[e].to);
    R.ok(kzr_is_dag_(kept), "rmas: the removal leaves a DAG");
  }
  // the minimum is the definition's: the cheapest arc subset whose
  // removal leaves a DAG (subset enumeration, kept tiny on purpose)
  if (m <= 18)
    R.ok(ulen(got.total_weight) == kzr_fas_brute_(succs, arcs, w),
         "rmas: the weight is the subset-enumeration minimum");
  // cycle nodes: the diagonal of the >=1-arc reachability
  {
    auto reach1 = kzr_reach1(succs);
    auto cyc = gns::calculate_cycle_nodes<V>(g);
    bool eq = cyc.size() == n;
    for (ulen u = 0; u < n && eq; ++u)
      eq = bool(cyc[u]) == bool(reach1[u][u]);
    R.ok(eq, "rmas: the cycle nodes are the >=1-arc diagonal");
  }
}

// ---------------------------------------------------------------------------
// 26. gns.rmasp: the minimum feedback arc set of a REDUCIBLE flow
// graph -- the polynomial loop-cut walk (outermost first, the
// obligation discounts), held against the exact 2^V sweep: same
// weight, same certificates, and the irreducible witness reporting
// itself empty.
// ---------------------------------------------------------------------------
static void fz_rmasp(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 7); // 1..8 nodes: the sweep rides along
  auto [succs, cross] = kzg_reducible(rng, n, kz_chance(rng, 25));
  auto arcs = kzg_arcs(succs);
  const ulen m = arcs.size();
  std::vector<ulen> w(m);
  for (auto &x : w)
    x = kz_pick(rng, 13); // zero weights live here too
  auto g = gns::make_basic_forest<V>(succs);
  auto got = gns::calculate_minimum_feedback_arc_set_reducible<V>(0, g, w);
  // the given-dominators form must agree byte for byte: the caller's
  // idoms (upstream's dom kernel -- the same provider the An layer
  // uses) ride along, on the irreducible-witness path as much as on
  // the structured one
  {
    std::vector<std::vector<ulen>> preds(n);
    for (ulen u = 0; u < n; ++u)
      for (ulen v : succs[u])
        if (v < n)
          preds[v].push_back(u);
    const auto idoms = gns::calculate_dominators_from_entry<V>(
        0, g, gns::make_basic_forest<V>(preds));
    const auto gd =
        gns::calculate_minimum_feedback_arc_set_reducible_given_dominators<V>(
            0, g, w, idoms);
    R.ok(gd.reducible == got.reducible &&
             gd.total_weight == got.total_weight &&
             gd.packing_weight == got.packing_weight &&
             gd.removed_arcs == got.removed_arcs && gd.order == got.order,
         "rmasp: the given-dominators form matches the standalone one");
  }
  if (cross) {
    // the injected sibling pair owns a cycle with no on-cycle
    // dominator: the contract check must refuse the input
    R.ok(!got.reducible && got.removed_arcs.empty() && got.order.empty() &&
             ulen(got.total_weight) == 0,
         "rmasp: the irreducible witness reports itself, empty");
    return;
  }
  R.ok(got.reducible, "rmasp: the structured graph passes the contract");
  // the removed set: a strictly ascending arc subset; self loops gone
  {
    bool ok = true;
    for (ulen i = 0; i < got.removed_arcs.size() && ok; ++i)
      ok = got.removed_arcs[i] < m &&
           (i == 0 || got.removed_arcs[i] > got.removed_arcs[i - 1]);
    R.ok(ok, "rmasp: the removed set is a sorted arc subset");
    ok = true;
    for (ulen e = 0; e < m && ok; ++e)
      if (arcs[e].from == arcs[e].to)
        ok = std::binary_search(got.removed_arcs.begin(),
                                got.removed_arcs.end(), e);
    R.ok(ok, "rmasp: the self loops are all removed");
  }
  // the order: a permutation, topological over the survivors
  {
    std::vector<ulen> pos(n, kz_none());
    bool ok = got.order.size() == n;
    for (ulen i = 0; i < got.order.size() && ok; ++i) {
      ok = got.order[i] < n && pos[got.order[i]] == kz_none();
      if (ok)
        pos[got.order[i]] = i;
    }
    R.ok(ok, "rmasp: the order is a permutation");
    for (ulen e = 0; e < m && ok; ++e)
      if (!std::binary_search(got.removed_arcs.begin(), got.removed_arcs.end(),
                              e))
        ok = pos[arcs[e].from] < pos[arcs[e].to];
    R.ok(ok, "rmasp: the order is the survivors' topological order");
  }
  // the weight is the sum
  {
    ulen tw = 0;
    for (ulen e : got.removed_arcs)
      tw += w[e];
    R.ok(ulen(got.total_weight) == tw, "rmasp: the total weight is the sum");
  }
  // THE WITNESS: the packing is a lower bound on every feedback arc
  // set (the min-max theorem); when it meets the committed weight it
  // certifies the minimum outright, and the exactness itself is the
  // sweep differential below (the packing is a greedy innermost-first
  // witness -- a bound, not always the tightest one)
  R.ok(ulen(got.packing_weight) <= ulen(got.total_weight),
       "rmasp: the packing bounds the answer from below");
  // THE DIFFERENTIAL: the exact sweep's minimum, on every seed (the
  // sizes keep 2^V the honest reference)
  {
    auto exact = gns::calculate_minimum_feedback_arc_set<V>(g, w);
    R.ok(ulen(got.total_weight) == ulen(exact.total_weight),
         "rmasp: the polynomial weight is the exact minimum");
    // and the sweep's own answer stays a valid FAS here too (both
    // oracles, each spelling the other's contract)
    std::vector<std::vector<ulen>> kept(n);
    for (ulen e = 0; e < m; ++e)
      if (!std::binary_search(got.removed_arcs.begin(), got.removed_arcs.end(),
                              e))
        kept[arcs[e].from].push_back(arcs[e].to);
    R.ok(kzr_is_dag_(kept), "rmasp: the removal leaves a DAG");
  }
}

// ---------------------------------------------------------------------------
// 22. gns.meg: the minimum equivalent arc set -- preservation, the
// exact-on-acyclic guarantee, the honesty bounds
// ---------------------------------------------------------------------------
static void fz_meg(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 7);
  auto succs = kzg_digraph(rng, n, 3);
  auto arcs = kzg_arcs(succs);
  const ulen m = arcs.size();
  auto reach1 = kzr_reach1(succs);
  std::vector<ulen> comp_of{};
  std::vector<std::vector<ulen>> classes{};
  kzr_scc_classes(succs, comp_of, classes);
  auto g = gns::make_basic_forest<V>(succs);
  auto got = gns::calculate_minimum_equivalent_arc_set<V>(g);
  // ---- the kept arcs are distinct input arcs
  std::vector<ulen> keep = got.keep_edges;
  std::sort(keep.begin(), keep.end());
  bool ok = true;
  for (ulen i = 0; i < keep.size() && ok; ++i)
    ok = keep[i] < m && (i == 0 || keep[i] > keep[i - 1]);
  R.ok(ok, "meg: the keep set is a strictly sorted arc subset");
  // ---- reachability between DISTINCT nodes is preserved exactly
  {
    std::vector<std::vector<ulen>> kept(n);
    for (ulen e : keep)
      kept[arcs[e].from].push_back(arcs[e].to);
    auto rk = kzr_reach1(kept);
    ok = true;
    for (ulen u = 0; u < n && ok; ++u)
      for (ulen v = 0; v < n && ok; ++v)
        if (u != v)
          ok = rk[u][v] == reach1[u][v];
    R.ok(ok, "meg: distinct-pair reachability is preserved exactly");
  }
  // ---- acyclic graphs (no multi-node SCC): the answer IS the TR
  {
    bool acyclic = true;
    for (auto &c : classes)
      acyclic = acyclic && c.size() == 1;
    for (ulen u = 0; u < n && acyclic; ++u)
      for (ulen v : succs[u])
        if (v == u)
          acyclic = false; // a self loop is a cycle
    if (acyclic) {
      std::vector<char> redundant(m, 0);
      for (ulen e = 0; e < m; ++e) {
        const ulen u = arcs[e].from, v = arcs[e].to;
        for (ulen f = 0; f < m && !redundant[e]; ++f)
          if (f != e && arcs[f].from == u && arcs[f].to != v &&
              reach1[arcs[f].to][v])
            redundant[e] = 1;
      }
      std::vector<ulen> want{};
      for (ulen e = 0; e < m; ++e) {
        if (redundant[e] || arcs[e].from == arcs[e].to)
          continue;
        bool first = true;
        for (ulen f = 0; f < e && first; ++f)
          first = !(arcs[f].from == arcs[e].from && arcs[f].to == arcs[e].to);
        if (first)
          want.push_back(e);
      }
      std::sort(want.begin(), want.end());
      R.ok(keep == want, "meg: on a DAG the answer is the exact TR");
      R.ok(got.approx_scc_arcs == 0, "meg: no approximation arcs on a DAG");
    }
  }
  // ---- the inter-component part is the condensation's TR size; the
  // inside-SCC part obeys the 2*(size-1) honesty bound
  {
    // the condensation: de-paralleled comp pairs
    std::vector<std::pair<ulen, ulen>> carc{};
    for (ulen e = 0; e < m; ++e) {
      const ulen cu = comp_of[arcs[e].from], cv = comp_of[arcs[e].to];
      if (cu != cv)
        carc.push_back({cu, cv});
    }
    std::sort(carc.begin(), carc.end());
    carc.erase(std::unique(carc.begin(), carc.end()), carc.end());
    // the condensation's TR by the redundancy rule
    const ulen nc = classes.size();
    std::vector<std::vector<char>> creach(nc, std::vector<char>(nc, 0));
    for (auto [a, b] : carc)
      creach[a][b] = 1;
    for (ulen k = 0; k < nc; ++k)
      for (ulen i = 0; i < nc; ++i)
        for (ulen j = 0; j < nc; ++j)
          if (creach[i][k] && creach[k][j])
            creach[i][j] = 1;
    ulen tr_size = 0;
    for (auto [a, b] : carc) {
      bool red = false;
      for (auto [a2, b2] : carc)
        if (a2 == a && b2 != b && creach[b2][b]) {
          red = true;
          break;
        }
      tr_size += !red;
    }
    // count the kept inter-component arcs
    ulen kept_inter = 0;
    for (ulen e : keep)
      if (comp_of[arcs[e].from] != comp_of[arcs[e].to])
        ++kept_inter;
    R.ok(kept_inter == tr_size,
         "meg: the inter-component count is the condensation's TR");
    // the kept arcs inside each SCC: <= 2*(size-1)
    std::vector<ulen> kept_in(classes.size(), 0);
    for (ulen e : keep)
      if (comp_of[arcs[e].from] == comp_of[arcs[e].to])
        ++kept_in[comp_of[arcs[e].from]];
    ok = true;
    for (ulen c = 0; c < classes.size() && ok; ++c)
      if (classes[c].size() > 1)
        ok = kept_in[c] <= 2 * (classes[c].size() - 1);
    R.ok(ok, "meg: the inside-SCC arcs obey the 2(n-1) bound");
    R.ok(got.scc_count == nc, "meg: the scc count is reported");
    // each multi-node SCC stays strongly connected under the keeps
    ok = true;
    for (ulen c = 0; c < classes.size() && ok; ++c) {
      if (classes[c].size() < 2)
        continue;
      std::vector<std::vector<ulen>> kept(n);
      for (ulen e : keep)
        kept[arcs[e].from].push_back(arcs[e].to);
      for (ulen i = 0; i < classes[c].size() && ok; ++i)
        for (ulen j = 0; j < classes[c].size() && ok; ++j)
          if (i != j)
            ok = rk_reaches_(kept, classes[c][i], classes[c][j]);
    }
    R.ok(ok, "meg: each SCC stays strongly connected");
  }
}

// ---------------------------------------------------------------------------
// 23. gns.dist: Bellman-Ford/Floyd/DAG-SP/eccentricity vs definitions
// ---------------------------------------------------------------------------
static void fz_dist(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 6);
  auto succs = kzg_digraph(rng, n, 3);
  auto arcs = kzg_arcs(succs);
  const ulen m = arcs.size();
  std::vector<int64_t> w(m);
  for (auto &x : w)
    x = int64_t(kz_pick(rng, 19)) - 9;
  const ulen entry = kz_pick(rng, n);
  constexpr int64_t none_ = 0x7fffffffffffffff;
  // ---- the simple-path minima (Held-Karp) and the negative-cycle
  // poison (cycles reachable from entry, flooded forward)
  auto best = kzr_simple_path_min(succs, arcs, w, entry);
  std::vector<char> poison(n, 0);
  bool neg_any = false, neg_reached = false;
  {
    auto reach0 = kzr_reach0(succs);
    for (auto &c : kzr_directed_simple_cycles(succs)) {
      int64_t wsum = 0;
      for (ulen i = 0; i < c.size(); ++i) {
        const ulen a = c[i], b = c[(i + 1) % c.size()];
        int64_t bw = none_;
        for (ulen e = 0; e < m; ++e)
          if (arcs[e].from == a && arcs[e].to == b && w[e] < bw)
            bw = w[e];
        wsum += bw;
      }
      if (wsum < 0) {
        neg_any = true;
        if (reach0[entry][c[0]]) {
          neg_reached = true;
          for (ulen v : c)
            poison[v] = 1;
        }
      }
    }
    if (neg_reached) { // flood the poison forward
      for (bool ch = true; ch;) {
        ch = false;
        for (ulen u = 0; u < n; ++u)
          if (poison[u])
            for (ulen v : succs[u])
              if (v < n && !poison[v]) {
                poison[v] = 1;
                ch = true;
              }
      }
    }
  }
  // ---- Bellman-Ford
  {
    auto bf = gns::calculate_shortest_path_maybe_negative<V>(
        entry, gns::make_basic_forest<V>(succs), w);
    R.ok(bf.has_negative_cycle == neg_reached,
         "dist: the negative-cycle verdict matches the enumeration");
    bool eq = bf.dist.size() == n;
    for (ulen v = 0; v < n && eq; ++v) {
      if (poison[v] || best[v] == none_) {
        // poisoned or unreachable: not a fact (unreachable and poisoned
        // both read as none -- the caller cannot distinguish, by design)
        eq = bf.dist[v] == none_;
      } else {
        eq = bf.dist[v] == best[v];
      }
    }
    R.ok(eq, "dist: Bellman-Ford is the simple-path minima, poisoned "
             "where cycles reach");
  }
  // ---- Floyd-Warshall (all pairs): exact when no negative cycle
  // exists anywhere; the verdict flag always
  {
    auto ap = gns::calculate_all_pairs_shortest_paths<V>(
        gns::make_basic_forest<V>(succs), w);
    R.ok(ap.has_negative_cycle == neg_any,
         "dist: Floyd's verdict sees every negative cycle");
    if (!neg_any) {
      bool eq = ap.dist.size() == n * n;
      for (ulen u = 0; u < n && eq; ++u) {
        auto bu = kzr_simple_path_min(succs, arcs, w, u);
        for (ulen v = 0; v < n && eq; ++v) {
          const int64_t want = u == v ? 0 : bu[v];
          eq = ap.dist[u * n + v] == want;
        }
      }
      R.ok(eq, "dist: Floyd is the all-pairs simple-path minima");
    }
  }
  // ---- the DAG engine (a fresh DAG: negative weights are fine there)
  {
    auto dsuccs = kzg_dag(rng, n, 3);
    auto darcs = kzg_arcs(dsuccs);
    std::vector<int64_t> dw(darcs.size());
    for (auto &x : dw)
      x = int64_t(kz_pick(rng, 19)) - 9;
    const ulen dentry = kz_pick(rng, n);
    auto want = kzr_simple_path_min(dsuccs, darcs, dw, dentry);
    auto dsp = gns::calculate_dag_shortest_paths<V>(
        dentry, gns::make_basic_forest<V>(dsuccs), dw);
    bool eq = dsp.size() == n;
    for (ulen v = 0; v < n && eq; ++v)
      eq = want[v] == none_ ? dsp[v] == none_ : dsp[v] == want[v];
    R.ok(eq, "dist: the DAG sweep is the simple-path minima");
  }
  // ---- the eccentricities (unweighted, directed reach)
  {
    auto ecc = gns::calculate_eccentricities_unweighted<V>(
        gns::make_basic_forest<V>(succs));
    std::vector<ulen> recc(n, kz_none());
    for (ulen s = 0; s < n; ++s) {
      std::vector<ulen> lvl(n, kz_none());
      std::vector<ulen> que{s};
      lvl[s] = 0;
      for (ulen qi = 0; qi < que.size(); ++qi) {
        const ulen u = que[qi];
        for (ulen v : succs[u])
          if (v < n && lvl[v] == kz_none()) {
            lvl[v] = lvl[u] + 1;
            que.push_back(v);
          }
      }
      bool all = true;
      ulen far = 0;
      for (ulen v = 0; v < n; ++v)
        if (lvl[v] == kz_none())
          all = false;
        else
          far = far < lvl[v] ? lvl[v] : far;
      recc[s] = all ? far : kz_none();
    }
    bool eq = ecc.eccentricity == recc;
    ulen rdim = kz_none(), rrad = kz_none();
    std::vector<ulen> rcenter{};
    for (ulen u = 0; u < n; ++u)
      if (recc[u] != kz_none()) {
        rdim = rdim == kz_none() ? recc[u] : (rdim < recc[u] ? recc[u] : rdim);
        rrad = rrad == kz_none() ? recc[u] : (recc[u] < rrad ? recc[u] : rrad);
      }
    for (ulen u = 0; u < n; ++u)
      if (recc[u] != kz_none() && recc[u] == rrad)
        rcenter.push_back(u);
    R.ok(eq && ecc.diameter == rdim && ecc.radius == rrad &&
             ecc.center == rcenter,
         "dist: the eccentricities, diameter, radius, center");
  }
}

// ---------------------------------------------------------------------------
// 25. gns.closure: the maximum-weight closure -- the definitional
// subset sweep, then the returned set re-checked (closed, summing,
// never below the empty set)
// ---------------------------------------------------------------------------
static void fz_maxclosure(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 10); // every subset enumerated
  auto succs = kzg_digraph(rng, n, 3);
  std::vector<int64_t> w(n);
  for (auto &x : w)
    x = int64_t(kz_pick(rng, 17)) - 8; // signed: the lens needs both
  int64_t best = 0;                    // the empty set is closed
  for (ulen bits = 0; bits < (ulen(1) << n); ++bits) {
    bool closed = true;
    for (ulen u = 0; u < n && closed; ++u)
      if ((bits >> u) & 1)
        for (ulen v : succs[u])
          if (v < n && !((bits >> v) & 1)) {
            closed = false;
            break;
          }
    if (!closed)
      continue;
    int64_t sum = 0;
    for (ulen u = 0; u < n; ++u)
      if ((bits >> u) & 1)
        sum += w[u];
    best = best < sum ? sum : best;
  }
  auto cl = gns::calculate_maximum_weight_closure<V>(
      gns::make_basic_forest<V>(succs), w);
  R.ok(cl.weight == best, "closure: the weight is the maximum");
  bool closed = cl.in_closure.size() == n;
  int64_t sum = 0;
  for (ulen u = 0; u < n; ++u)
    if (cl.in_closure[u])
      sum += w[u];
  for (ulen u = 0; u < n && closed; ++u)
    if (cl.in_closure[u])
      for (ulen v : succs[u])
        if (v < n && !cl.in_closure[v])
          closed = false;
  R.ok(closed && sum == cl.weight && cl.weight >= 0,
       "closure: the set is closed, sums to the weight, and never "
       "loses to the empty set");
}

// ---------------------------------------------------------------------------
// 26. gns.pcover: the minimum path cover of a DAG -- the Dilworth
// count by branch-and-bound split matching, then the paths walked and
// re-checked (disjoint, covering, real arcs)
// ---------------------------------------------------------------------------
static void fz_pathcover(std::mt19937_64 &rng, KzRec &R) {
  const ulen n = 1 + kz_pick(rng, 8);
  auto succs = kzg_dag(rng, n, 0);
  // the reference: the maximum split-graph matching by SUBSET DP --
  // tails in order, the state the set of heads already taken (skip or
  // take an unused head); the answer is the fullest reachable state.
  // Coded from the definition, never from the library's Hopcroft-Karp
  const ulen nmask = ulen(1) << n;
  std::vector<std::vector<char>> reach(n + 1, std::vector<char>(nmask, 0));
  reach[0][0] = 1;
  for (ulen l = 0; l < n; ++l)
    for (ulen mask = 0; mask < nmask; ++mask)
      if (reach[l][mask]) {
        reach[l + 1][mask] = 1; // the tail stays a path head
        for (ulen v : succs[l])
          if (v < n && !((mask >> v) & 1))
            reach[l + 1][mask | (ulen(1) << v)] = 1;
      }
  ulen ref_match = 0;
  for (ulen mask = 0; mask < nmask; ++mask)
    if (reach[n][mask]) {
      ulen c = 0;
      for (ulen v = 0; v < n; ++v)
        c += (mask >> v) & 1;
      ref_match = ref_match < c ? c : ref_match;
    }
  auto pc = gns::calculate_minimum_path_cover_dag<V>(
      gns::make_basic_forest<V>(succs));
  R.ok(pc.path_count + ref_match == n,
       "pcover: paths + maximum split matching == n (Dilworth)");
  // the certificate: the paths are disjoint, cover every node once,
  // and every consecutive pair is a real arc
  std::vector<char> seen(n, 0);
  bool ok = true;
  ulen total = 0;
  for (const auto &p : pc.paths) {
    total += ulen(p.size());
    for (ulen i = 0; i < p.size() && ok; ++i) {
      ok &= p[i] < n && !seen[p[i]];
      if (!ok)
        break;
      seen[p[i]] = 1;
      if (i + 1 < p.size()) {
        bool arc = false;
        for (ulen v : succs[p[i]])
          arc |= (v == p[i + 1]);
        ok &= arc;
      }
    }
  }
  R.ok(ok && total == n && pc.path_count == pc.paths.size(),
       "pcover: disjoint, covering, real arcs, count agrees");
}
