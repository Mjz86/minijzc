
#ifndef MJZ_SRC_GRAPH_cut_FILE_
#define MJZ_SRC_GRAPH_cut_FILE_
#include "csr.hpp"
#include "flow.hpp"

MJZ_EXPORT
//
namespace mjz::graph_ns {

// THE CUT LAYER. The s-t minimum cut is flow.hpp's
// calculate_st_min_cut; this file adds the cut families around it:
//
//   * the GLOBAL minimum cut of an UNDIRECTED graph (Stoer-Wagner:
//     n-1 maximum-adjacency phases, each folding the phase's tightest
//     vertex into its neighbor -- O(n^3) on the weight matrix, exact).
//   * the GLOBAL minimum cut of a DIGRAPH (the classical sweep: fix
//     any source s; the global cut is the cheapest of the 2(n-1)
//     s-t cuts {mincut(s,t), mincut(t,s)} over every other node --
//     every global cut (S,T) has s on one side and some t on the
//     other, so the sweep cannot miss it).
//   * the MAXIMUM cut: the exact answer for tiny graphs (2^n
//     enumeration) and the greedy + local-search witness for the
//     general case (the derandomized greedy alone guarantees half the
//     optimum; the flip loop only improves it).
//   * the GOMORY-HU tree (Gusfield's simplification: n-1 max flows,
//     no edge contractions): the tree whose path-minima encode EVERY
//     pair's minimum cut value.
//   * the MAXIMUM-WEIGHT CLOSURE (the project-selection lens): the
//     heaviest successor-closed node set under SIGNED node weights,
//     one min cut away -- the source feeds the positives, the
//     negatives drain to the sink, requirement arcs run at infinity,
//     and S's side of the min cut is the answer.
//
// THE UNDIRECTED CONVENTION (Stoer-Wagner and Gomory-Hu): the rows
// are SYMMETRIC (each undirected edge present as both arcs, its
// weight on each), and the algorithms count each pair exactly once
// (the arc u->v with u < v; parallel arcs count separately, so
// multi-edges scale the cut exactly as intended; self loops never
// cross a cut). The MAX CUT is the DIRECTED cut (every crossing ARC
// counts its own weight) -- symmetric input simply doubles the
// answer, consistently.

template <version_t version_v, class weight_t> struct global_min_cut_result_t {
  weight_t weight{};        // the global minimum cut value
  std::vector<char> side{}; // one side of the cheapest cut
};
template <version_t version_v, class weight_t> struct max_cut_result_t {
  weight_t weight{};        // the cut value achieved
  std::vector<char> side{}; // one side of the cut
};
template <version_t version_v, class weight_t> struct gomory_hu_tree_t {
  // node i's tree edge goes to parent[i] with weight[i] (node 0 is
  // the root, parent[0] and weight[0] are zero). The minimum cut
  // value between ANY two nodes is the minimum weight on their unique
  // tree path (gomory_hu_query below).
  std::vector<uintlen_t> parent{};
  std::vector<weight_t> weight{};
};

// ---- Stoer-Wagner ---------------------------------------------------------
template <
    version_t version_v, class R, usable_range_exact_c<version_v> weights_t,
    std::unsigned_integral weight_t = std::ranges::range_value_t<weights_t>>
MJZ_CX_FN global_min_cut_result_t<version_v, weight_t>
calculate_global_min_cut_undirected(const basic_forest_t<version_v, R> &graph,
                                    const weights_t &weights) noexcept {
  global_min_cut_result_t<version_v, weight_t> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  ret.side.assign(n, 0);
  if (n <= 1)
    return ret; // no cut exists; the empty side says so
  auto it_w = std::ranges::begin(weights);
  // the working matrix: w[u][v] = the total undirected weight between
  // u and v (each input edge folded into both cells), groups track
  // the merged supernodes through the phases
  std::vector<std::vector<weight_t>> w(n, std::vector<weight_t>(n));
  {
    auto edge_ids = graph.index_range();
    for (uintlen_t u = 0; u < n; ++u) {
      for (uintlen_t e : edge_ids[u]) {
        const uintlen_t v = graph.get_edge_to(e);
        asserts(v < n);
        if (v == u || u > v)
          continue; // self loops never cross; the u<v arc counts once
        w[u][v] += weight_t(*(it_w + intlen_t(e)));
        w[v][u] = w[u][v];
      }
    }
  }
  std::vector<std::vector<uintlen_t>> group(n);
  for (uintlen_t u = 0; u < n; ++u)
    group[u].push_back(u);
  std::vector<char> merged(n, 0);
  std::vector<weight_t> conn(n); // connection weight to the added set
  std::vector<char> added(n);
  weight_t best = weight_t(-1);        // "infinite": every weight_t cut is less
  std::vector<uintlen_t> best_group{}; // the best phase's isolated side
  // n-1 phases: each isolates one vertex t (cut-of-the-phase = its
  // connection weight at isolation), then folds t into the vertex
  // added just before it
  for (uintlen_t phase = 1; phase < n; ++phase) {
    std::ranges::fill(added, 0);
    std::ranges::fill(conn, weight_t());
    uintlen_t prev = uintlen_t(-1), last = uintlen_t(-1);
    for (uintlen_t added_count = 0, live = n - phase + 1; added_count < live;
         ++added_count) {
      // the unmerged vertex most tightly connected to the added set
      // (maximum-adjacency order; the FIRST pick is arbitrary -- node
      // order is the deterministic tiebreak)
      uintlen_t pick = uintlen_t(-1);
      for (uintlen_t u = 0; u < n; ++u) {
        if (merged[u] || added[u])
          continue;
        if (pick == uintlen_t(-1) || conn[u] > conn[pick])
          pick = u;
      }
      asserts(pick != uintlen_t(-1));
      added[pick] = 1;
      prev = last;
      last = pick;
      for (uintlen_t u = 0; u < n; ++u)
        if (!merged[u] && !added[u])
          conn[u] += w[pick][u];
    }
    if (conn[last] < best || best == weight_t(-1)) {
      best = conn[last];
      best_group = group[last]; // the SNAPSHOT (later phases keep merging)
    }
    // fold `last` into `prev`: merge weights, groups, and liveness
    asserts(prev != uintlen_t(-1));
    for (uintlen_t u = 0; u < n; ++u) {
      if (u == prev || u == last || merged[u])
        continue;
      w[prev][u] += w[last][u];
      w[u][prev] = w[prev][u];
    }
    for (uintlen_t u : group[last])
      group[prev].push_back(u);
    group[last].clear();
    merged[last] = 1;
  }
  ret.weight = best;
  // the best phase's snapshot is the isolated side (empty only when
  // no cut exists -- a one-node graph)
  for (uintlen_t u : best_group) {
    asserts(u < n);
    ret.side[u] = 1;
  }
  return ret;
}

// ---- the directed global minimum cut --------------------------------------
template <version_t version_v, class R, usable_range_exact_c<version_v> caps_t,
          detail::flow_cap_c cap_t = std::ranges::range_value_t<caps_t>>
MJZ_CX_FN global_min_cut_result_t<version_v, cap_t>
calculate_global_min_cut_directed(const basic_forest_t<version_v, R> &graph,
                                  const caps_t &caps) noexcept {
  global_min_cut_result_t<version_v, cap_t> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  if (n <= 1)
    return ret;
  cap_t best = cap_t(-1);
  bool have_best = false;
  std::vector<char> best_side{};
  const uintlen_t s = 0;
  for (uintlen_t t = 0; t < n; ++t) {
    if (t == s)
      continue;
    // both directions run: every global cut (S, T) has s on one side
    // and some t on the other, so the sweep cannot miss it
    auto cut01 = calculate_st_min_cut<version_v>(s, t, graph, caps);
    if (!have_best || cut01.weight < best) {
      best = cut01.weight;
      best_side = std::move(cut01.source_side); // the s side realizes it
      have_best = true;
    }
    auto cut10 = calculate_st_min_cut<version_v>(t, s, graph, caps);
    if (!have_best || cut10.weight < best) {
      best = cut10.weight;
      // the t side realizes this weight -- reporting its complement
      // would pair the side with the OTHER direction's value (the flip
      // the component fuzzer caught: the side must satisfy
      // cap(side -> rest) == weight, and only the source side does)
      best_side = std::move(cut10.source_side);
      have_best = true;
    }
  }
  ret.weight = best;
  ret.side = std::move(best_side);
  return ret;
}

// ---- the maximum cut ------------------------------------------------------
// exact, by enumeration: n <= ~24 in practice (2^(n-1) sides, node 0
// pinned to side 0 -- the two orientations of a cut are one cut)
template <version_t version_v, class R,
          usable_range_exact_c<version_v> weights_t,
          std::integral weight_t = std::ranges::range_value_t<weights_t>>
MJZ_CX_FN max_cut_result_t<version_v, weight_t>
calculate_max_cut_bruteforce(const basic_forest_t<version_v, R> &graph,
                             const weights_t &weights) noexcept {
  max_cut_result_t<version_v, weight_t> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  ret.side.assign(n, 0);
  if (n <= 1)
    return ret;
  asserts(n <= 31); // the enumeration's domain, stated as a contract
  auto it_w = std::ranges::begin(weights);
  const uintlen_t enumerate_bits = n - 1;
  const uintlen_t limit = uintlen_t(1) << enumerate_bits;
  weight_t best = weight_t();
  uint32_t best_bits = 0;
  for (uint32_t bits = 0; uintlen_t(bits) < limit; ++bits) {
    weight_t val = weight_t();
    auto edge_ids = graph.index_range();
    for (uintlen_t u = 0; u < n; ++u) {
      const bool su = u == 0 ? false : bool((bits >> (u - 1)) & 1);
      for (uintlen_t e : edge_ids[u]) {
        const uintlen_t v = graph.get_edge_to(e);
        if (v == u || v >= n)
          continue;
        const bool sv = v == 0 ? false : bool((bits >> (v - 1)) & 1);
        if (su != sv)
          val += weight_t(*(it_w + intlen_t(e)));
      }
    }
    if (bits == 0 || val > best) {
      best = val;
      best_bits = bits;
    }
  }
  ret.weight = best;
  for (uintlen_t u = 1; u < n; ++u)
    ret.side[u] = (best_bits >> (u - 1)) & 1;
  return ret;
}

// the general-case witness: the derandomized greedy (each node joins
// the side that maximizes the crossing weight against the nodes
// already placed -- alone this guarantees at least half the optimum)
// followed by the first-improvement flip loop (a maximal cut under
// single flips; each flip strictly raises the value, so the loop is a
// descending-at-most-weight_t-steps ratchet, never a cycle)
template <version_t version_v, class R,
          usable_range_exact_c<version_v> weights_t,
          std::integral weight_t = std::ranges::range_value_t<weights_t>>
MJZ_CX_FN max_cut_result_t<version_v, weight_t>
calculate_max_cut_greedy(const basic_forest_t<version_v, R> &graph,
                         const weights_t &weights) noexcept {
  max_cut_result_t<version_v, weight_t> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  ret.side.assign(n, 0);
  if (n <= 1)
    return ret;
  auto it_w = std::ranges::begin(weights);
  auto edge_ids = graph.index_range();
  // the greedy placement: nodes in id order, each joining the side
  // against which its placed edges weigh more (ties to side 0)
  for (uintlen_t u = 0; u < n; ++u) {
    weight_t w0 = weight_t(), w1 = weight_t();
    for (uintlen_t e : edge_ids[u]) {
      const uintlen_t v = graph.get_edge_to(e);
      if (v >= n || v == u)
        continue;
      if (v < u) { // only placed nodes count
        const weight_t wt = weight_t(*(it_w + intlen_t(e)));
        if (ret.side[v])
          w0 += wt;
        else
          w1 += wt;
      }
    }
    ret.side[u] = w0 > w1 ? 1 : 0;
  }
  // first-improvement flips, round-robin, until stable: each flip's
  // delta is the tally below (BOTH directions -- an out-only tally
  // would miss the in-arcs and could flip downhill on asymmetric
  // input), strictly positive by the test, and the directed cut value
  // lives in a finite lattice -- termination is structural
  std::vector<std::vector<uintlen_t>> adj(n);
  std::vector<std::vector<weight_t>> adj_w(n);
  {
    std::vector<uintlen_t> deg(n, 0);
    for (uintlen_t u = 0; u < n; ++u)
      for (uintlen_t e : edge_ids[u]) {
        const uintlen_t v = graph.get_edge_to(e);
        if (v < n && v != u)
          deg[u]++, deg[v]++; // the tail's out-slot, the head's in-slot
      }
    for (uintlen_t u = 0; u < n; ++u) {
      adj[u].reserve(deg[u]);
      adj_w[u].reserve(deg[u]);
    }
    for (uintlen_t u = 0; u < n; ++u) {
      for (uintlen_t e : edge_ids[u]) {
        const uintlen_t v = graph.get_edge_to(e);
        if (v >= n || v == u)
          continue;
        const weight_t wt = weight_t(*(it_w + intlen_t(e)));
        adj[u].push_back(v); // u's out-arc
        adj_w[u].push_back(wt);
        adj[v].push_back(u); // and v's view of the same arc
        adj_w[v].push_back(wt);
      }
    }
  }
  bool improved = true;
  while (improved) {
    improved = false;
    for (uintlen_t u = 0; u < n; ++u) {
      weight_t same = weight_t(), cross = weight_t();
      for (uintlen_t k = 0; k < adj[u].size(); ++k) {
        if (ret.side[adj[u][k]] == ret.side[u])
          same += adj_w[u][k];
        else
          cross += adj_w[u][k];
      }
      if (same > cross) {
        ret.side[u] = !ret.side[u];
        improved = true;
      }
    }
  }
  ret.weight = weight_t();
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t k = 0; k < adj[u].size(); ++k) {
      const uintlen_t v = adj[u][k];
      if (u < v && ret.side[u] != ret.side[v])
        ret.weight += adj_w[u][k];
    }
  }
  return ret;
}

// ---- the Gomory-Hu tree (Gusfield) ----------------------------------------
// n-1 max flows on the SYMMETRIZED graph (each input edge becomes a
// both-ways arc pair carrying its full weight -- the undirected
// convention above). parent[i] starts at 0; each round flows (i,
// parent[i]), records the value, and re-parents every later node on
// the i-side of that cut.
template <
    version_t version_v, class R, usable_range_exact_c<version_v> weights_t,
    std::unsigned_integral weight_t = std::ranges::range_value_t<weights_t>>
MJZ_CX_FN gomory_hu_tree_t<version_v, weight_t>
calculate_gomory_hu_tree(const basic_forest_t<version_v, R> &graph,
                         const weights_t &weights) noexcept {
  gomory_hu_tree_t<version_v, weight_t> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  ret.parent.assign(n, 0);
  ret.weight.assign(n, weight_t());
  if (n <= 1)
    return ret;
  // the symmetrized capacity graph: (u, v, w) triples in one sweep
  // (each input edge once, either direction -- the undirected
  // convention), then both arcs per triple
  std::vector<uintlen_t> sym_from{}, sym_to{};
  std::vector<weight_t> sym_w{};
  {
    auto edge_ids = graph.index_range();
    auto it_w = std::ranges::begin(weights);
    for (uintlen_t u = 0; u < n; ++u) {
      for (uintlen_t e : edge_ids[u]) {
        const uintlen_t v = graph.get_edge_to(e);
        if (v == u || v >= n || u > v)
          continue; // the u<v arc carries the undirected edge once
        sym_from.push_back(u);
        sym_to.push_back(v);
        sym_w.push_back(weight_t(*(it_w + intlen_t(e))));
      }
    }
  }
  const uintlen_t m = uintlen_t(sym_from.size());
  // the symmetrized graph: rows of TARGETS (the CSR convention), with
  // the capacities riding along in the same row-major flattening
  // make_basic_forest produces
  std::vector<std::vector<uintlen_t>> rows(n);
  std::vector<std::vector<weight_t>> row_caps(n);
  for (uintlen_t i = 0; i < m; ++i) {
    rows[sym_from[i]].push_back(sym_to[i]);
    row_caps[sym_from[i]].push_back(sym_w[i]);
    rows[sym_to[i]].push_back(sym_from[i]);
    row_caps[sym_to[i]].push_back(sym_w[i]);
  }
  std::vector<weight_t> sym_caps{};
  for (uintlen_t u = 0; u < n; ++u)
    sym_caps.insert(sym_caps.end(), row_caps[u].begin(), row_caps[u].end());
  const auto sym_graph = make_basic_forest<version_v>(rows);
  // Gusfield's loop: flow (i, parent[i]) on the symmetrized graph,
  // record, re-parent the i-side members that still point at the old
  // parent
  for (uintlen_t i = 1; i < n; ++i) {
    auto cut =
        calculate_st_min_cut<version_v>(i, ret.parent[i], sym_graph, sym_caps);
    ret.weight[i] = cut.weight;
    for (uintlen_t j = i + 1; j < n; ++j) {
      if (ret.parent[j] == ret.parent[i] && cut.source_side[j])
        ret.parent[j] = i; // j rode the i side: hang it under i
    }
  }
  return ret;
}

// the tree query: the minimum cut value between u and v is the
// smallest tree-edge weight on their unique path (walk both up to the
// common ancestor by depth)
template <version_t version_v, class weight_t>
MJZ_CX_FN weight_t
gomory_hu_query(const gomory_hu_tree_t<version_v, weight_t> &tree, uintlen_t u,
                uintlen_t v) noexcept {
  const uintlen_t n = tree.parent.size();
  asserts(u < n && v < n);
  if (u == v)
    return weight_t();
  auto depth_of = [&](uintlen_t x) {
    uintlen_t d = 0;
    while (x != 0) {
      x = tree.parent[x];
      ++d;
    }
    return d;
  };
  uintlen_t du = depth_of(u), dv = depth_of(v);
  weight_t best = weight_t(-1);
  bool any = false;
  uintlen_t a = u, b = v;
  while (du > dv) {
    if (!any || tree.weight[a] < best) {
      best = tree.weight[a];
      any = true;
    }
    a = tree.parent[a];
    --du;
  }
  while (dv > du) {
    if (!any || tree.weight[b] < best) {
      best = tree.weight[b];
      any = true;
    }
    b = tree.parent[b];
    --dv;
  }
  while (a != b) {
    if (!any || tree.weight[a] < best) {
      best = tree.weight[a];
      any = true;
    }
    if (tree.weight[b] < best)
      best = tree.weight[b];
    a = tree.parent[a];
    b = tree.parent[b];
  }
  return any ? best : weight_t();
}

// ---- the maximum-weight closure -------------------------------------------
// A closure is a successor-closed node set: an arc u->v with u inside
// forces v inside. With SIGNED node weights the heaviest closure is
// one min cut away (Picard): the source feeds every positive node its
// weight, every negative node drains to the sink, and every
// requirement arc runs at INFINITY -- cutting a requirement arc admits
// an unclosed choice, so the min cut trades exactly the weight of the
// nodes it strands, and the source side IS a heaviest closure. The
// answer is the positive weight kept: sum(w+) - mincut (>= 0: the
// empty set is closed, and cutting every source arc buys it).
template <version_t version_v> struct max_closure_result_t {
  int64_t weight{};               // int64 floor, the min_cost_flow precedent
  std::vector<char> in_closure{}; // the chosen closure (n entries)
};
template <version_t version_v, class R,
          usable_range_exact_c<version_v> weights_t>
MJZ_CX_FN max_closure_result_t<version_v>
calculate_maximum_weight_closure(const basic_forest_t<version_v, R> &graph,
                                 const weights_t &weights) noexcept {
  max_closure_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  ret.in_closure.assign(n, 0);
  if (!n)
    return ret;
  auto it_w = std::ranges::begin(weights);
  // the augmented network: node n is the source, n+1 the sink. Rows
  // and capacities are built TOGETHER, then flattened row-major --
  // the forest enumerates edges row by row, and the cap stream must
  // walk that exact order
  const uintlen_t S = n, T = n + 1;
  uint64_t pos_sum = 0;
  std::vector<std::vector<uintlen_t>> rows(n + 2);
  std::vector<std::vector<uint64_t>> row_caps(n + 2);
  for (uintlen_t u = 0; u < n; ++u) {
    const int64_t w = int64_t(*(it_w + intlen_t(u)));
    if (w > 0) {
      rows[S].push_back(u);
      row_caps[S].push_back(uint64_t(w));
      pos_sum += uint64_t(w);
    } else if (w < 0) {
      rows[u].push_back(T);
      row_caps[u].push_back(uint64_t(-w));
    }
  }
  // the requirement arcs at infinity: severing one strands more
  // weight than keeping every positive node, which the min cut can
  // always afford instead; self loops are satisfied by any choice
  // (u inside forces u inside) and carry nothing
  const uint64_t inf = pos_sum + 1;
  auto edge_ids = graph.index_range();
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t e : edge_ids[u]) {
      const uintlen_t v = graph.get_edge_to(e);
      if (v >= n || v == u)
        continue;
      rows[u].push_back(v);
      row_caps[u].push_back(inf);
    }
  }
  std::vector<uint64_t> caps{};
  uintlen_t cap_total = 0;
  for (const auto &rc : row_caps)
    cap_total += uintlen_t(rc.size());
  caps.reserve(cap_total);
  for (const auto &rc : row_caps)
    caps.insert(caps.end(), rc.begin(), rc.end());
  auto aug = make_basic_forest<version_v>(rows);
  auto cut = calculate_st_min_cut<version_v>(S, T, aug, caps);
  ret.weight = int64_t(pos_sum) - int64_t(cut.weight);
  for (uintlen_t u = 0; u < n; ++u)
    ret.in_closure[u] = cut.source_side[u] ? 1 : 0;
  return ret;
}

};     // namespace mjz::graph_ns
#endif // MJZ_SRC_GRAPH_cut_FILE_
