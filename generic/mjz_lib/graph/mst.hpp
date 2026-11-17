
#ifndef MJZ_SRC_GRAPH_mst_FILE_
#define MJZ_SRC_GRAPH_mst_FILE_
#include "csr.hpp"
#include "dsu.hpp"

MJZ_EXPORT
//
namespace mjz::graph_ns {

// THE MINIMUM SPANNING TREE / FOREST LAYER. Three classics, three
// determinism stories, one contract: the WEIGHT of the answer is the
// unique minimum (the minimum spanning forest's weight is unique even
// when the edge set is not), the EDGE SET is the deterministic
// witness each algorithm produces.
//
//   * KRUSKAL: edges sorted by (weight, edge id), union-find accept.
//     O(E log E). The output list is in pick order (weight ascending,
//     id ascending within a weight -- deterministic, and the order
//     clients can diff against).
//   * PRIM (dense): O(V^2) with the min-unvisited sweep; parallel
//     edges are legal (each is its own candidate; the cheapest wins,
//     ties by edge id). The output list is in addition order.
//   * BORUVKA: rounds of per-component cheapest edges; O(E log V).
//     The output list is in (round, component id) order.
//
// Disconnected inputs produce a minimum spanning FOREST (every
// component gets its own tree); the total is the sum over components.
// Self loops never enter a forest; they are skipped outright.
//
// THE WEIGHTS: weight_t is an unsigned integer (or a uintN width) --
// totals accumulate in weight_t, so pick a width where the forest's
// total fits.
template <version_t version_v, class weight_t>
struct minimum_spanning_forest_result_t {
  weight_t total{};                 // the sum over the forest (unique)
  std::vector<uintlen_t> edges{};   // the chosen input edge ids
};

// ---- Kruskal ---------------------------------------------------------------
template <version_t version_v, class R,
          usable_range_exact_c<version_v> weights_t,
          std::unsigned_integral weight_t = std::ranges::range_value_t<weights_t>>
MJZ_CX_FN minimum_spanning_forest_result_t<version_v, weight_t>
calculate_minimum_spanning_forest_kruskal(
    const basic_forest_t<version_v, R> &graph,
    const weights_t &weights) noexcept {
  minimum_spanning_forest_result_t<version_v, weight_t> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  const uintlen_t nedges = uintlen_t(std::ranges::size(graph.edges));
  std::vector<uintlen_t> by_weight(nedges, 0);
  for (uintlen_t e = 0; e < nedges; ++e)
    by_weight[e] = e;
  auto it_w = std::ranges::begin(weights);
  std::ranges::sort(by_weight, std::ranges::less{},
                    [&](uintlen_t e) noexcept {
                      return pair_t<weight_t, uintlen_t>{
                          weight_t(*(it_w + intlen_t(e))), e};
                    });
  disjoint_set_union_t<version_v, std::vector<uintlen_t>> dsu{};
  dsu.parent_then_rank.assign(n, 0);
  dsu.init(n);
  auto edge_ids = graph.index_range();
  auto edge_to = [&](uintlen_t e) noexcept {
    return uintlen_t(*(std::ranges::begin(graph.edges) + intlen_t(e)));
  };
  // from-of-edge table once (the same sweep the transpose uses)
  std::vector<uintlen_t> from_of_edge(nedges, uintlen_t(-1));
  for (uintlen_t u = 0; u < n; ++u)
    for (uintlen_t e : edge_ids[u]) {
      asserts(e < nedges);
      from_of_edge[e] = u;
    }
  for (uintlen_t e : by_weight) {
    const uintlen_t u = from_of_edge[e], v = edge_to(e);
    if (u == v)
      continue; // self loop
    if (dsu.find_and_unite_root(u, v)) {
      ret.edges.push_back(e);
      ret.total += weight_t(*(it_w + intlen_t(e)));
    }
  }
  return ret;
}

// ---- Prim (dense) ----------------------------------------------------------
template <version_t version_v, class R,
          usable_range_exact_c<version_v> weights_t,
          std::unsigned_integral weight_t = std::ranges::range_value_t<weights_t>>
MJZ_CX_FN minimum_spanning_forest_result_t<version_v, weight_t>
calculate_minimum_spanning_forest_prim(
    const basic_forest_t<version_v, R> &graph,
    const weights_t &weights) noexcept {
  minimum_spanning_forest_result_t<version_v, weight_t> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  if (!n)
    return ret;
  auto it_w = std::ranges::begin(weights);
  auto edge_ids = graph.index_range();
  auto edge_to = [&](uintlen_t e) noexcept {
    return uintlen_t(*(std::ranges::begin(graph.edges) + intlen_t(e)));
  };
  // every node starts unattached; each seed grows one tree per
  // component (the "forest Prim": restart from the next unplanted
  // node -- node id order, deterministic)
  std::vector<char> in_tree(n, 0);
  std::vector<weight_t> best(n, weight_t()); // cheapest attachment weight
  std::vector<uintlen_t> best_edge(
      n, uintlen_t(-1)); // the arc achieving it (NONE = unattached)
  for (uintlen_t seed = 0; seed < n; ++seed) {
    if (in_tree[seed])
      continue;
    in_tree[seed] = 1;
    for (uintlen_t e : edge_ids[seed]) {
      const uintlen_t v = edge_to(e);
      if (v >= n || v == seed)
        continue;
      const weight_t w = weight_t(*(it_w + intlen_t(e)));
      if (!in_tree[v] && (best_edge[v] == uintlen_t(-1) || w < best[v] ||
                          (w == best[v] && e < best_edge[v]))) {
        best[v] = w;
        best_edge[v] = e;
      }
    }
    for (;;) {
      uintlen_t pick = uintlen_t(-1);
      for (uintlen_t v = 0; v < n; ++v) {
        if (in_tree[v] || best_edge[v] == uintlen_t(-1))
          continue;
        if (pick == uintlen_t(-1) || best[v] < best[pick])
          pick = v; // ties: lowest node id (the scan order keeps it)
      }
      if (pick == uintlen_t(-1))
        break; // the component is exhausted
      in_tree[pick] = 1;
      ret.edges.push_back(best_edge[pick]);
      ret.total += best[pick];
      best_edge[pick] = uintlen_t(-1);
      for (uintlen_t e : edge_ids[pick]) {
        const uintlen_t v = edge_to(e);
        if (v >= n || v == pick || in_tree[v])
          continue;
        const weight_t w = weight_t(*(it_w + intlen_t(e)));
        if (best_edge[v] == uintlen_t(-1) || w < best[v] ||
            (w == best[v] && e < best_edge[v])) {
          best[v] = w;
          best_edge[v] = e;
        }
      }
    }
  }
  return ret;
}

// ---- Boruvka ---------------------------------------------------------------
template <version_t version_v, class R,
          usable_range_exact_c<version_v> weights_t,
          std::unsigned_integral weight_t = std::ranges::range_value_t<weights_t>>
MJZ_CX_FN minimum_spanning_forest_result_t<version_v, weight_t>
calculate_minimum_spanning_forest_boruvka(
    const basic_forest_t<version_v, R> &graph,
    const weights_t &weights) noexcept {
  minimum_spanning_forest_result_t<version_v, weight_t> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  const uintlen_t nedges = uintlen_t(std::ranges::size(graph.edges));
  if (!n)
    return ret;
  auto it_w = std::ranges::begin(weights);
  auto edge_ids = graph.index_range();
  std::vector<uintlen_t> from_of_edge(nedges, uintlen_t(-1));
  for (uintlen_t u = 0; u < n; ++u)
    for (uintlen_t e : edge_ids[u]) {
      asserts(e < nedges);
      from_of_edge[e] = u;
    }
  disjoint_set_union_t<version_v, std::vector<uintlen_t>> dsu{};
  dsu.parent_then_rank.assign(n, 0);
  dsu.init(n);
  std::vector<uintlen_t> comp_of(n);
  std::vector<uintlen_t> cheapest(n, uintlen_t(-1)); // per-ROOT edge
  for (;;) {
    // every component's cheapest outgoing edge (ties by edge id)
    for (uintlen_t u = 0; u < n; ++u)
      if (dsu.find_and_cache_root(u) == u)
        cheapest[u] = uintlen_t(-1);
    for (uintlen_t e = 0; e < nedges; ++e) {
      const uintlen_t u = from_of_edge[e];
      const uintlen_t v =
          uintlen_t(*(std::ranges::begin(graph.edges) + intlen_t(e)));
      if (u == v || v >= n)
        continue;
      const uintlen_t ru = dsu.find_and_cache_root(u);
      const uintlen_t rv = dsu.find_and_cache_root(v);
      if (ru == rv)
        continue;
      const weight_t w = weight_t(*(it_w + intlen_t(e)));
      auto better = [&](uintlen_t cur) noexcept {
        if (cur == uintlen_t(-1))
          return true;
        const weight_t cw = weight_t(*(it_w + intlen_t(cur)));
        return w < cw || (w == cw && e < cur);
      };
      if (better(cheapest[ru]))
        cheapest[ru] = e;
      if (better(cheapest[rv]))
        cheapest[rv] = e;
    }
    // unite along every chosen edge (the safe half of each pair --
    // find_and_unite is idempotent on the second sighting)
    bool grew = false;
    for (uintlen_t u = 0; u < n; ++u) {
      if (dsu.find_and_cache_root(u) != u)
        continue;
      const uintlen_t e = cheapest[u];
      if (e == uintlen_t(-1))
        continue;
      const uintlen_t v =
          uintlen_t(*(std::ranges::begin(graph.edges) + intlen_t(e)));
      if (dsu.find_and_unite_root(from_of_edge[e], v)) {
        ret.edges.push_back(e);
        ret.total += weight_t(*(it_w + intlen_t(e)));
        grew = true;
      }
    }
    if (!grew)
      break; // every component is isolated
  }
  std::ranges::sort(ret.edges); // (round, component) order is a story
  // for the comment; the id-sorted list is the stable contract
  return ret;
}

}; // namespace mjz::graph_ns
#endif // MJZ_SRC_GRAPH_mst_FILE_
