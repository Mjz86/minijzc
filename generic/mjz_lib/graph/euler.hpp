
#ifndef MJZ_SRC_GRAPH_euler_FILE_
#define MJZ_SRC_GRAPH_euler_FILE_
#include "csr.hpp"
#include "dsu.hpp"

MJZ_EXPORT
//
namespace mjz::graph_ns {

// THE EULER LAYER (Hierholzer's walk, iterative -- the stack holds
// theTour under construction; a node with exhausted arcs pops and
// appends to the answer). Two conventions:
//
//   * DIRECTED: the circuit needs every node's in-degree == out-degree
//     (within its component) and all edges in one weakly connected
//     piece; the path additionally allows exactly one start with
//     out-in == 1 and one end with in-out == 1 (everything else zero).
//   * UNDIRECTED (symmetric rows; each undirected edge is TWO arcs,
//     its twins): the circuit needs every degree even; the path
//     exactly two odd. Walking an arc consumes its TWIN too (the
//     first unused arc of the opposite direction -- the same
//     one-twin discipline the bridge DFS uses, so parallel edges and
//     self loops behave: a self loop's two arcs are twins of each
//     other).
//
// The result's `ok` is false when the degree conditions fail or the
// edges are not in one piece (checked before the walk; a failed walk
// never returns a partial tour). The edge list is the tour's arcs in
// walk order; the node list is implied (end of arc k = head of the
// walk; `first_node` is given).
template <version_t version_v>
struct euler_result_t {
  bool ok{};
  uintlen_t first_node{};
  std::vector<uintlen_t> edges{}; // the tour's arcs, walk order
};

namespace detail {
// weak connectivity over the arc endpoints (the directed circuit's
// one-piece test; self loops are neutral)
template <version_t version_v, class R>
MJZ_CX_FN bool euler_one_piece_(const basic_forest_t<version_v, R> &graph,
                                std::vector<char> &has_edge) noexcept {
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  disjoint_set_union_t<version_v, std::vector<uintlen_t>> dsu{};
  dsu.parent_then_rank.assign(n, 0);
  dsu.init(n);
  auto rows = graph.range(); // the row slots' TARGETS
  std::ranges::fill(has_edge, 0);
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t v : rows[u]) {
      if (v >= n)
        continue;
      has_edge[u] = 1;
      if (v != u) {
        has_edge[v] = 1;
        dsu.find_and_unite_root(u, v);
      }
    }
  }
  uintlen_t anchor = uintlen_t(-1);
  for (uintlen_t u = 0; u < n; ++u) {
    if (!has_edge[u])
      continue;
    if (anchor == uintlen_t(-1))
      anchor = u;
    else if (!dsu.is_united(anchor, u))
      return false;
  }
  return true;
}
} // namespace detail

// ---- the directed circuit --------------------------------------------------
template <version_t version_v, class R>
MJZ_CX_FN euler_result_t<version_v>
calculate_eulerian_circuit_directed(
    const basic_forest_t<version_v, R> &graph) noexcept {
  euler_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  std::vector<uintlen_t> indeg(n, 0), outdeg(n, 0);
  auto rows = graph.range();
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t v : rows[u]) {
      if (v >= n)
        continue;
      outdeg[u]++;
      indeg[v]++;
    }
  }
  std::vector<char> has_edge(n, 0);
  for (uintlen_t u = 0; u < n; ++u)
    if (indeg[u] != outdeg[u])
      return ret; // the degree conditions fail
  if (!detail::euler_one_piece_<version_v>(graph, has_edge))
    return ret;
  // the walk starts at the lowest-id node with edges (deterministic)
  uintlen_t start = uintlen_t(-1);
  for (uintlen_t u = 0; u < n && start == uintlen_t(-1); ++u)
    if (outdeg[u])
      start = u;
  if (start == uintlen_t(-1))
    start = 0; // no edges at all: the empty circuit is trivially ok
  ret.ok = true;
  ret.first_node = start;
  // Hierholzer: the working stack of (node, cursor); popping a node
  // appends the arc that re-enters it
  auto edge_ids = graph.index_range();
  std::vector<uintlen_t> cursors(n, 0);
  std::vector<uintlen_t> walk_nodes{start};
  std::vector<uintlen_t> walk_edges{};
  while (!walk_nodes.empty()) {
    const uintlen_t u = walk_nodes.back();
    if (cursors[u] < uintlen_t(std::ranges::size(rows[u]))) {
      const uintlen_t k = cursors[u]++;
      const uintlen_t e = uintlen_t(edge_ids[u][intlen_t(k)]);
      if (e >= uintlen_t(std::ranges::size(graph.edges)))
        continue;
      const uintlen_t v = uintlen_t(rows[u][intlen_t(k)]);
      if (v >= n)
        continue;
      walk_nodes.push_back(v);
      walk_edges.push_back(e);
      continue;
    }
    walk_nodes.pop_back();
    if (!walk_edges.empty()) {
      ret.edges.push_back(walk_edges.back());
      walk_edges.pop_back();
    }
  }
  std::ranges::reverse(ret.edges);
  return ret;
}

// ---- the directed path -----------------------------------------------------
template <version_t version_v, class R>
MJZ_CX_FN euler_result_t<version_v>
calculate_eulerian_path_directed(
    const basic_forest_t<version_v, R> &graph) noexcept {
  euler_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  std::vector<uintlen_t> indeg(n, 0), outdeg(n, 0);
  auto rows = graph.range();
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t v : rows[u]) {
      if (v >= n)
        continue;
      outdeg[u]++;
      indeg[v]++;
    }
  }
  uintlen_t start = uintlen_t(-1), end = uintlen_t(-1);
  for (uintlen_t u = 0; u < n; ++u) {
    if (indeg[u] + 1 == outdeg[u]) {
      if (start != uintlen_t(-1))
        return ret;
      start = u;
    } else if (outdeg[u] + 1 == indeg[u]) {
      if (end != uintlen_t(-1))
        return ret;
      end = u;
    } else if (indeg[u] != outdeg[u]) {
      return ret;
    }
  }
  if (start == uintlen_t(-1)) {
    // the balanced case degenerates to a circuit; walk it as a path
    // from its canonical start (an Eulerian circuit is an Eulerian
    // path)
    auto circ = calculate_eulerian_circuit_directed<version_v>(graph);
    circ.first_node = circ.ok ? circ.first_node : 0;
    return circ;
  }
  std::vector<char> has_edge(n, 0);
  if (!detail::euler_one_piece_<version_v>(graph, has_edge))
    return ret;
  ret.ok = true;
  ret.first_node = start;
  auto edge_ids = graph.index_range();
  std::vector<uintlen_t> cursors(n, 0);
  std::vector<uintlen_t> walk_nodes{start};
  std::vector<uintlen_t> walk_edges{};
  while (!walk_nodes.empty()) {
    const uintlen_t u = walk_nodes.back();
    if (cursors[u] < uintlen_t(std::ranges::size(rows[u]))) {
      const uintlen_t k = cursors[u]++;
      const uintlen_t e = uintlen_t(edge_ids[u][intlen_t(k)]);
      if (e >= uintlen_t(std::ranges::size(graph.edges)))
        continue;
      const uintlen_t v = uintlen_t(rows[u][intlen_t(k)]);
      if (v >= n)
        continue;
      walk_nodes.push_back(v);
      walk_edges.push_back(e);
      continue;
    }
    walk_nodes.pop_back();
    if (!walk_edges.empty()) {
      ret.edges.push_back(walk_edges.back());
      walk_edges.pop_back();
    }
  }
  std::ranges::reverse(ret.edges);
  return ret;
}

// ---- the undirected family -------------------------------------------------
namespace detail {
// consume arc e (u->v) and its twin: the FIRST unused arc v->u in row
// order (with parallel arcs each pair consumes independently; a self
// loop's twin is the next self arc in its own row -- the pair
// representation is the contract)
template <version_t version_v, class R>
MJZ_CX_FN void euler_consume_twin_(std::vector<char> &used,
                                    const basic_forest_t<version_v, R> &graph,
                                    uintlen_t u, uintlen_t e) noexcept {
  const uintlen_t v =
      uintlen_t(*(std::ranges::begin(graph.edges) + intlen_t(e)));
  auto rows = graph.range();           // targets
  auto edge_ids = graph.index_range(); // arc ids
  used[e] = 1;
  for (uintlen_t k = 0; k < uintlen_t(std::ranges::size(rows[v])); ++k) {
    const uintlen_t e2 = uintlen_t(edge_ids[v][intlen_t(k)]);
    const uintlen_t w = uintlen_t(rows[v][intlen_t(k)]);
    if (!used[e2] && w == u) {
      used[e2] = 1;
      return;
    }
  }
  asserts(asserts.expect_rn, false,
          "undirected walks need symmetric rows");
}
} // namespace detail

template <version_t version_v, class R>
MJZ_CX_FN euler_result_t<version_v>
calculate_eulerian_circuit_undirected(
    const basic_forest_t<version_v, R> &graph) noexcept {
  euler_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  std::vector<uintlen_t> deg(n, 0);
  auto rows = graph.range();
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t v : rows[u]) {
      if (v >= n || v == u)
        continue; // self loops are walked but never skew degrees
      deg[u]++; // u's OWN row counts each incident edge exactly once
                // -- the TRUE undirected degree. (A cross-increment
                // deg[v]++ would count every edge twice in the pair
                // representation and mute the parity test -- the bug
                // the component fuzzer pinned on the path graph.)
    }
  }
  for (uintlen_t u = 0; u < n; ++u)
    if (deg[u] % 2)
      return ret; // the circuit needs even degrees
  std::vector<char> has_edge(n, 0);
  if (!detail::euler_one_piece_<version_v>(graph, has_edge))
    return ret;
  uintlen_t start = uintlen_t(-1);
  for (uintlen_t u = 0; u < n && start == uintlen_t(-1); ++u)
    if (has_edge[u]) // ANY edge, self loops included -- a node holding
                     // only loops has degree 0 yet is still the walk's
                     // home (the loop-only graph the fuzzer pinned)
      start = u;
  if (start == uintlen_t(-1))
    start = 0;
  ret.ok = true;
  ret.first_node = start;
  auto edge_ids = graph.index_range();
  std::vector<char> used(uintlen_t(std::ranges::size(graph.edges)), 0);
  std::vector<uintlen_t> cursors(n, 0);
  std::vector<uintlen_t> walk_nodes{start};
  std::vector<uintlen_t> walk_edges{};
  while (!walk_nodes.empty()) {
    const uintlen_t u = walk_nodes.back();
    bool advanced = false;
    while (cursors[u] < uintlen_t(std::ranges::size(rows[u]))) {
      const uintlen_t k = cursors[u]++;
      const uintlen_t e = uintlen_t(edge_ids[u][intlen_t(k)]);
      if (e >= used.size() || used[e])
        continue;
      const uintlen_t v = uintlen_t(rows[u][intlen_t(k)]);
      if (v >= n)
        continue;
      detail::euler_consume_twin_<version_v>(used, graph, u, e);
      walk_nodes.push_back(v);
      walk_edges.push_back(e);
      advanced = true;
      break;
    }
    if (advanced)
      continue;
    walk_nodes.pop_back();
    if (!walk_edges.empty()) {
      ret.edges.push_back(walk_edges.back());
      walk_edges.pop_back();
    }
  }
  std::ranges::reverse(ret.edges);
  return ret;
}

template <version_t version_v, class R>
MJZ_CX_FN euler_result_t<version_v>
calculate_eulerian_path_undirected(
    const basic_forest_t<version_v, R> &graph) noexcept {
  euler_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  std::vector<uintlen_t> deg(n, 0);
  auto rows = graph.range();
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t v : rows[u]) {
      if (v < n && v != u)
        deg[u]++; // the TRUE undirected degree: u's own row, once per
                  // incident edge (the pair representation must not
                  // double it -- see the circuit sibling above)
    }
  }
  uintlen_t odd_a = uintlen_t(-1), odd_b = uintlen_t(-1);
  for (uintlen_t u = 0; u < n; ++u) {
    if (deg[u] % 2) {
      if (odd_a == uintlen_t(-1))
        odd_a = u;
      else if (odd_b == uintlen_t(-1))
        odd_b = u;
      else
        return ret; // three or more odd degrees: no path
    }
  }
  std::vector<char> has_edge(n, 0);
  if (!detail::euler_one_piece_<version_v>(graph, has_edge))
    return ret;
  const uintlen_t start = odd_a == uintlen_t(-1) ? 0 : odd_a;
  if (odd_a == uintlen_t(-1)) {
    // all even: the circuit, re-walked from its canonical start
    for (uintlen_t u = 0; u < n; ++u)
      if (has_edge[u]) { // any edge, self loops included (see circuit)
        ret.first_node = u;
        break;
      }
  } else {
    ret.first_node = start;
  }
  ret.ok = true;
  auto edge_ids = graph.index_range();
  std::vector<char> used(uintlen_t(std::ranges::size(graph.edges)), 0);
  std::vector<uintlen_t> cursors(n, 0);
  std::vector<uintlen_t> walk_nodes{ret.first_node};
  std::vector<uintlen_t> walk_edges{};
  while (!walk_nodes.empty()) {
    const uintlen_t u = walk_nodes.back();
    bool advanced = false;
    while (cursors[u] < uintlen_t(std::ranges::size(rows[u]))) {
      const uintlen_t k = cursors[u]++;
      const uintlen_t e = uintlen_t(edge_ids[u][intlen_t(k)]);
      if (e >= used.size() || used[e])
        continue;
      const uintlen_t v = uintlen_t(rows[u][intlen_t(k)]);
      if (v >= n)
        continue;
      detail::euler_consume_twin_<version_v>(used, graph, u, e);
      walk_nodes.push_back(v);
      walk_edges.push_back(e);
      advanced = true;
      break;
    }
    if (advanced)
      continue;
    walk_nodes.pop_back();
    if (!walk_edges.empty()) {
      ret.edges.push_back(walk_edges.back());
      walk_edges.pop_back();
    }
  }
  std::ranges::reverse(ret.edges);
  return ret;
}

}; // namespace mjz::graph_ns
#endif // MJZ_SRC_GRAPH_euler_FILE_
