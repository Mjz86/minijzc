
#ifndef MJZ_SRC_GRAPH_distances_FILE_
#define MJZ_SRC_GRAPH_distances_FILE_
#include "csr.hpp"
#include "dijkstra.hpp"
#include "sequence.hpp"

MJZ_EXPORT
//
namespace mjz::graph_ns {

// THE DISTANCES LAYER -- the shortest-path family around dijkstra's
// positive-weight engine:
//
//   * BELLMAN-FORD: the maybe-negative single-source engine (plain
//     rounds, O(V*E), deterministic edge order), reporting the
//     negative-cycle verdict -- distances of nodes reachable FROM a
//     negative cycle are not facts, so they come back as unreachable
//     and `has_negative_cycle` says why.
//   * FLOYD-WARSHALL: all-pairs on the dense matrix (O(V^3), the
//     classic in-place relaxation), same negative-cycle verdict.
//   * DAG SHORTEST PATHS: one pass over the topological order (the
//     acyclic engine, O(V+E) -- the compiler's own loop-shape
//     machinery's favorite); the DAG contract is asserted.
//   * ECCENTRICITIES (unweighted): BFS from every node (O(V*(V+E))),
//     with the DIAMETER (max eccentricity) and the RADIUS/CENTER
//     (min eccentricity) derived in the same sweep. Unreachable
//     pairs read as eccentricity NONE_V -- the caller decides whether
//     that is "infinite" or "not a fact" (the graph may be a forest).
//
// THE WEIGHTS. dist_t is the metric type: signed or unsigned integers
// (or uintN widths). NONE_V (uintlen_t(-1)) marks "no path"; the
// caller passes INF_INITed rows where the algorithms write.

// ---- Bellman-Ford ------------------------------------------------------------
template <version_t version_v>
struct bellman_ford_result_t {
  std::vector<int64_t> dist{}; // NONE_V = no path / poisoned by a cycle
  bool has_negative_cycle{};
};

template <version_t version_v, class R,
          usable_range_exact_c<version_v> weights_t>
MJZ_CX_FN bellman_ford_result_t<version_v>
calculate_shortest_path_maybe_negative(
    uintlen_t entry, const basic_forest_t<version_v, R> &graph,
    const weights_t &weights) noexcept {
  bellman_ford_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  constexpr int64_t none_ = 0x7fffffffffffffff;
  ret.dist.assign(n, none_);
  if (entry >= n)
    return ret;
  auto rows = graph.range();          // targets
  auto edge_ids = graph.index_range(); // arc ids
  auto it_w = std::ranges::begin(weights);
  ret.dist[entry] = 0;
  // n-1 plain rounds (deterministic: row order, then arc order)
  for (uintlen_t round = 0; round + 1 < n; ++round) {
    bool changed = false;
    for (uintlen_t u = 0; u < n; ++u) {
      if (ret.dist[u] == none_)
        continue;
      for (uintlen_t k = 0; k < uintlen_t(std::ranges::size(rows[u])); ++k) {
        const uintlen_t v = uintlen_t(rows[u][intlen_t(k)]);
        if (v >= n)
          continue;
        const uintlen_t e = uintlen_t(edge_ids[u][intlen_t(k)]);
        const int64_t cand =
            ret.dist[u] + int64_t(*(it_w + intlen_t(e)));
        if (cand < ret.dist[v]) {
          ret.dist[v] = cand;
          changed = true;
        }
      }
    }
    if (!changed)
      break;
  }
  // the verdict round: anything still relaxing sits on/behind a
  // negative cycle -- flood the poison from every such node
  std::vector<char> poison(n, 0);
  bool any = false;
  for (uintlen_t u = 0; u < n; ++u) {
    if (ret.dist[u] == none_)
      continue;
    for (uintlen_t k = 0; k < uintlen_t(std::ranges::size(rows[u])); ++k) {
      const uintlen_t v = uintlen_t(rows[u][intlen_t(k)]);
      if (v >= n)
        continue;
      const uintlen_t e = uintlen_t(edge_ids[u][intlen_t(k)]);
      if (ret.dist[u] + int64_t(*(it_w + intlen_t(e))) < ret.dist[v]) {
        poison[u] = 1;
        any = true;
      }
    }
  }
  ret.has_negative_cycle = any;
  if (any) {
    std::vector<uintlen_t> stack{};
    for (uintlen_t u = 0; u < n; ++u)
      if (poison[u])
        stack.push_back(u);
    while (!stack.empty()) {
      const uintlen_t u = stack.back();
      stack.pop_back();
      for (uintlen_t v : rows[u]) {
        if (v < n && !poison[v]) {
          poison[v] = 1;
          stack.push_back(v);
        }
      }
    }
    for (uintlen_t u = 0; u < n; ++u)
      if (poison[u])
        ret.dist[u] = none_;
  }
  return ret;
}

// ---- Floyd-Warshall -----------------------------------------------------------
template <version_t version_v>
struct all_pairs_result_t {
  std::vector<int64_t> dist{}; // n*n row-major; NONE_V = no path
  bool has_negative_cycle{};
};

template <version_t version_v, class R,
          usable_range_exact_c<version_v> weights_t>
MJZ_CX_FN all_pairs_result_t<version_v>
calculate_all_pairs_shortest_paths(
    const basic_forest_t<version_v, R> &graph,
    const weights_t &weights) noexcept {
  all_pairs_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  constexpr int64_t none_ = 0x7fffffffffffffff;
  ret.dist.assign(uintlen_t(n) * n, none_);
  for (uintlen_t u = 0; u < n; ++u)
    ret.dist[uintlen_t(u) * n + u] = 0;
  auto edge_ids = graph.index_range();
  auto it_w = std::ranges::begin(weights);
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t e : edge_ids[u]) {
      const uintlen_t v = graph.get_edge_to(e);
      if (v >= n)
        continue;
      const int64_t w = int64_t(*(it_w + intlen_t(e)));
      const uintlen_t k = uintlen_t(u) * n + v;
      if (w < ret.dist[k])
        ret.dist[k] = w; // parallel arcs: the cheapest wins
    }
  }
  for (uintlen_t k = 0; k < n; ++k) {
    for (uintlen_t i = 0; i < n; ++i) {
      const int64_t dik = ret.dist[uintlen_t(i) * n + k];
      if (dik == none_)
        continue;
      for (uintlen_t j = 0; j < n; ++j) {
        const int64_t dkj = ret.dist[uintlen_t(k) * n + j];
        if (dkj == none_)
          continue;
        const int64_t cand = dik + dkj;
        if (cand < ret.dist[uintlen_t(i) * n + j])
          ret.dist[uintlen_t(i) * n + j] = cand;
      }
    }
  }
  for (uintlen_t u = 0; u < n; ++u)
    if (ret.dist[uintlen_t(u) * n + u] < 0)
      ret.has_negative_cycle = true; // a negative closed walk exists
  return ret;
}

// ---- DAG shortest paths -------------------------------------------------------
template <version_t version_v, class R,
          usable_range_exact_c<version_v> weights_t>
MJZ_CX_FN std::vector<int64_t>
calculate_dag_shortest_paths(uintlen_t entry,
                             const basic_forest_t<version_v, R> &graph,
                             const weights_t &weights) noexcept {
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  constexpr int64_t none_ = 0x7fffffffffffffff;
  std::vector<int64_t> dist(n, none_);
  if (entry >= n)
    return dist;
  {
    const auto cyc = calculate_cycle_nodes<version_v>(graph);
    for (uintlen_t u = 0; u < n; ++u)
      asserts(!cyc[u]); // the DAG contract
  }
  auto rows = graph.range();          // targets
  auto edge_ids = graph.index_range(); // arc ids
  auto it_w = std::ranges::begin(weights);
  const auto [waves, wave_count] =
      calculate_acyclic_graph_sequenced_components<version_v>(graph);
  std::vector<uintlen_t> order(n, 0);
  for (uintlen_t u = 0; u < n; ++u)
    order[u] = u;
  std::ranges::sort(order, std::ranges::less{},
                    [&](uintlen_t u) { return uintlen_t(waves[u]); });
  dist[entry] = 0;
  for (uintlen_t u : order) {
    if (dist[u] == none_)
      continue;
    for (uintlen_t k = 0; k < uintlen_t(std::ranges::size(rows[u])); ++k) {
      const uintlen_t v = uintlen_t(rows[u][intlen_t(k)]);
      if (v >= n)
        continue;
      const uintlen_t e = uintlen_t(edge_ids[u][intlen_t(k)]);
      const int64_t cand = dist[u] + int64_t(*(it_w + intlen_t(e)));
      if (cand < dist[v])
        dist[v] = cand;
    }
  }
  return dist;
}

// ---- eccentricity / diameter / radius (unweighted) ------------------------------
template <version_t version_v>
struct eccentricity_result_t {
  static constexpr uintlen_t none_v = uintlen_t(-1);
  std::vector<uintlen_t> eccentricity{}; // per node; none_v = unreachable pair
  uintlen_t diameter{none_v};
  uintlen_t radius{none_v};
  std::vector<uintlen_t> center{}; // the nodes at the radius
};

template <version_t version_v, class R>
MJZ_CX_FN eccentricity_result_t<version_v>
calculate_eccentricities_unweighted(
    const basic_forest_t<version_v, R> &graph) noexcept {
  eccentricity_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  ret.eccentricity.assign(n, 0);
  auto rows = graph.range();
  std::vector<uintlen_t> level(n), queue(n);
  for (uintlen_t s = 0; s < n; ++s) {
    std::ranges::fill(level, uintlen_t(-1));
    level[s] = 0;
    uintlen_t qh = 0, qt = 0;
    queue[qt++] = s;
    uintlen_t far = 0;
    bool all = true;
    while (qh < qt) {
      const uintlen_t u = queue[qh++];
      for (uintlen_t v : rows[u]) {
        if (v >= n)
          continue;
        if (level[v] == uintlen_t(-1)) {
          level[v] = level[u] + 1;
          far = far < level[v] ? level[v] : far;
          queue[qt++] = v;
        }
      }
    }
    for (uintlen_t u = 0; u < n; ++u)
      if (level[u] == uintlen_t(-1))
        all = false;
    ret.eccentricity[s] = all ? far : eccentricity_result_t<version_v>::none_v;
  }
  for (uintlen_t u = 0; u < n; ++u)
    if (ret.eccentricity[u] != ret.none_v)
      ret.diameter = ret.diameter == ret.none_v
                         ? ret.eccentricity[u]
                         : (ret.diameter < ret.eccentricity[u]
                                ? ret.eccentricity[u]
                                : ret.diameter);
  for (uintlen_t u = 0; u < n; ++u)
    if (ret.eccentricity[u] != ret.none_v)
      ret.radius = ret.radius == ret.none_v
                       ? ret.eccentricity[u]
                       : (ret.eccentricity[u] < ret.radius
                              ? ret.eccentricity[u]
                              : ret.radius);
  if (ret.radius != ret.none_v)
    for (uintlen_t u = 0; u < n; ++u)
      if (ret.eccentricity[u] == ret.radius)
        ret.center.push_back(u);
  return ret;
}

}; // namespace mjz::graph_ns
#endif // MJZ_SRC_GRAPH_distances_FILE_
