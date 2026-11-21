
#ifndef MJZ_SRC_GRAPH_biconnect_FILE_
#define MJZ_SRC_GRAPH_biconnect_FILE_
#include "csr.hpp"

MJZ_EXPORT
//
namespace mjz::graph_ns {

// THE CONNECTIVITY LAYER (the undirected classics over a SYMMETRIC
// CSR graph: every undirected edge appears in BOTH endpoint rows as
// its own arc; parallel undirected edges are separate arc pairs and
// are correctly NOT bridges; self loops are ignored outright).
//
//   * BRIDGES: an arc whose removal disconnects the graph (Tarjan's
//     low-link, iterative DFS; the one-twin skip per tree entry is
//     what makes parallel edges safe).
//   * ARTICULATION POINTS: the vertices whose removal disconnects
//     (the root rule -- two or more DFS children -- plus the low-link
//     rule for inner vertices).
//   * BICONNECTED COMPONENTS: the edge partition into maximal
//     2-vertex-connected pieces (the edge stack of the same DFS); a
//     vertex touching two pieces is an articulation point by
//     construction, and the helper derives exactly that mask.
//   * K-CORES: the degeneracy peeling (bucket queue by current
//     degree, O(V+E); core[v] = the largest k keeping v).
//
// Determinism: DFS roots run in node order; within a row, arcs scan
// in row order; the twin skip consumes the FIRST matching arc. Every
// output is a pure function of the input CSR.

template <version_t version_v> struct biconnect_result_t {
  std::vector<char> is_bridge_edge{};   // per INPUT arc: the tree arc of
                                        // each bridge (one arc per bridge
                                        // edge -- its twin is the other
                                        // direction of the same edge)
  std::vector<char> is_articulation{};  // per node
  std::vector<uintlen_t> bcc_of_edge{}; // per INPUT arc (both arcs of
                                        // one undirected edge share)
  uintlen_t bcc_count{};
};

namespace detail {
// the one-twin-skip low-link DFS shared by all three biconnectivity
// answers. Frames: (node, cursor, skip_target, skip_used, parent_edge)
template <version_t version_v, class R>
MJZ_CX_FN biconnect_result_t<version_v>
biconnect_dfs_(const basic_forest_t<version_v, R> &graph) noexcept {
  biconnect_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  const uintlen_t nedges = uintlen_t(std::ranges::size(graph.edges));
  ret.is_bridge_edge.assign(nedges, 0);
  ret.is_articulation.assign(n, 0);
  ret.bcc_of_edge.assign(nedges, uintlen_t(-1));
  auto rows = graph.range();           // row slots -> TARGETS
  auto edge_ids = graph.index_range(); // row slots -> arc ids
  auto edge_to = [&](uintlen_t e) noexcept {
    return uintlen_t(*(std::ranges::begin(graph.edges) + intlen_t(e)));
  };
  std::vector<uintlen_t> disc(n, 0), low(n, 0);
  uintlen_t timer{};
  // the edge stack: the arcs waiting for their component to close
  std::vector<uintlen_t> edge_stack{};
  struct frame_t {
    uintlen_t node, cursor, skip_target, parent_edge;
    char skip_used;
    uintlen_t child_count;
  };
  std::vector<frame_t> stack{};
  for (uintlen_t root = 0; root < n; ++root) {
    if (disc[root])
      continue;
    stack.push_back({root, 0, uintlen_t(-1), uintlen_t(-1), 0, 0});
    disc[root] = low[root] = ++timer;
    while (!stack.empty()) {
      frame_t &fr = stack.back();
      const uintlen_t u = fr.node;
      if (fr.cursor < uintlen_t(std::ranges::size(rows[u]))) {
        const uintlen_t k = fr.cursor++;
        const uintlen_t e = uintlen_t(edge_ids[u][intlen_t(k)]);
        if (e >= nedges)
          continue; // junk slot: skipped, not fatal
        const uintlen_t v = edge_to(e);
        if (v >= n)
          continue;
        if (v == u)
          continue; // self loop: ignored
        // the ONE twin skip: entering from the parent, skip a single
        // arc back to it (parallel arcs leave their extra twins as
        // back edges -- that is the multi-edge safety)
        if (!fr.skip_used && v == fr.skip_target &&
            fr.parent_edge != uintlen_t(-1)) {
          fr.skip_used = 1;
          continue;
        }
        if (!disc[v]) {
          // tree edge: descend
          stack.push_back({v, 0, u, e, 0, 0});
          disc[v] = low[v] = ++timer;
          edge_stack.push_back(e);
          continue;
        }
        if (disc[v] < disc[u] && ret.bcc_of_edge[e] == uintlen_t(-1)) {
          // back edge to an ancestor (each undirected edge stack once:
          // the bcc_of_edge guard keeps the twin from double-pushing)
          edge_stack.push_back(e);
          low[u] = low[u] < disc[v] ? low[u] : disc[v];
        }
        continue;
      }
      // leaving u: fold the child's low, judge the tree edge
      if (stack.size() >= 2) {
        frame_t &parent = stack[stack.size() - 2];
        const uintlen_t p = parent.node;
        low[p] = low[p] < low[u] ? low[p] : low[u];
        ++parent.child_count;
        if (low[u] >= disc[p] && p != stack[0].node) {
          ret.is_articulation[p] = 1;
          // close the biconnected component down to (and including)
          // the tree edge p--u
          while (!edge_stack.empty()) {
            const uintlen_t e2 = edge_stack.back();
            edge_stack.pop_back();
            ret.bcc_of_edge[e2] = ret.bcc_count;
            if (e2 == fr.parent_edge)
              break;
          }
          ret.bcc_count++;
        } else if (p == stack[0].node) {
          // the root's children close their own components too
          while (!edge_stack.empty()) {
            const uintlen_t e2 = edge_stack.back();
            edge_stack.pop_back();
            ret.bcc_of_edge[e2] = ret.bcc_count;
            if (e2 == fr.parent_edge)
              break;
          }
          ret.bcc_count++;
        }
        if (low[u] > disc[p])
          ret.is_bridge_edge[fr.parent_edge] = 1;
      } else {
        // leaving the root: its remaining edges close the last piece
        while (!edge_stack.empty()) {
          const uintlen_t e2 = edge_stack.back();
          edge_stack.pop_back();
          if (ret.bcc_of_edge[e2] == uintlen_t(-1)) {
            ret.bcc_of_edge[e2] = ret.bcc_count;
            ret.bcc_count++;
          }
        }
      }
      stack.pop_back();
    }
  }
  return ret;
}
} // namespace detail

// the three answers in one sweep, plus the articulation mask
// completed by the touching-counts rule (a vertex is an articulation
// point iff it touches two or more biconnected pieces -- for the root
// this is exactly the two-children rule; the DFS marks the inner
// vertices, the sweep below adds the roots)
template <version_t version_v, class R>
MJZ_CX_FN biconnect_result_t<version_v>
calculate_biconnectivity(const basic_forest_t<version_v, R> &graph) noexcept {
  auto ret = detail::biconnect_dfs_<version_v>(graph);
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  auto edge_ids = graph.index_range();
  auto rows = graph.range(); // targets
  auto edge_to = [&](uintlen_t e) noexcept {
    return uintlen_t(*(std::ranges::begin(graph.edges) + intlen_t(e)));
  };
  // THE TWIN MIRROR: the DFS walks one arc per undirected edge (the
  // tree or back arc); the other half copies its sibling's answers.
  // Safe for parallel edges by construction: a parallel pair is never
  // a bridge and always sits in one biconnected piece, so whichever
  // assigned twin a half finds first tells the same truth
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t e : edge_ids[u]) {
      if (e >= ret.bcc_of_edge.size())
        continue;
      if (ret.bcc_of_edge[e] != uintlen_t(-1) || ret.is_bridge_edge[e])
        continue;
      const uintlen_t v = edge_to(e);
      if (v >= n || v == u)
        continue;
      for (uintlen_t e2 : edge_ids[v]) {
        if (e2 >= ret.bcc_of_edge.size() || e2 == e)
          continue;
        if (edge_to(e2) != u)
          continue;
        if (ret.is_bridge_edge[e2]) {
          // the twin of a bridge arc is a bridge arc of the SAME edge
          // -- and of the SAME (bridge's own singleton) component; a
          // flag without the id would leave the twin outside every
          // biconnected piece (the hole the component fuzzer pinned)
          ret.is_bridge_edge[e] = 1;
          if (ret.bcc_of_edge[e2] != uintlen_t(-1))
            ret.bcc_of_edge[e] = ret.bcc_of_edge[e2];
          break;
        }
        if (ret.bcc_of_edge[e2] != uintlen_t(-1)) {
          ret.bcc_of_edge[e] = ret.bcc_of_edge[e2];
          break;
        }
      }
    }
  }
  // a vertex's incident pieces: count distinct bcc ids among its
  // arcs (self loops carry no connectivity -- skip them outright)
  std::vector<uintlen_t> seen(ret.bcc_count, uintlen_t(-1));
  std::vector<uintlen_t> touch(n, 0);
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t e : edge_ids[u]) {
      if (e >= ret.bcc_of_edge.size())
        continue;
      const uintlen_t v = edge_to(e);
      if (v >= n || v == u)
        continue;
      const uintlen_t c = ret.bcc_of_edge[e];
      if (c == uintlen_t(-1))
        continue;
      if (seen[c] != u) {
        seen[c] = u;
        touch[u]++;
      }
    }
  }
  for (uintlen_t u = 0; u < n; ++u)
    ret.is_articulation[u] = touch[u] >= 2;
  return ret;
}

// bridges alone: the arc list (both arcs of each bridge edge)
template <version_t version_v, class R>
MJZ_CX_FN std::vector<uintlen_t>
calculate_bridges(const basic_forest_t<version_v, R> &graph) noexcept {
  auto all = calculate_biconnectivity<version_v>(graph); // with the mirror
  std::vector<uintlen_t> bridges{};
  for (uintlen_t e = 0; e < all.is_bridge_edge.size(); ++e)
    if (all.is_bridge_edge[e])
      bridges.push_back(e);
  return bridges;
}

// articulation points alone
template <version_t version_v, class R>
MJZ_CX_FN std::vector<char>
calculate_articulation_points(const basic_forest_t<version_v, R> &graph) {
  auto all = calculate_biconnectivity<version_v>(graph);
  return std::move(all.is_articulation);
}

// k-core decomposition: core[v] = the largest k such that v survives
// the k-core peel. THE LEVEL-PEEL: for k = 0, 1, 2, ... every alive
// vertex with current degree <= k is removed (core = k) and its
// neighbors' degrees drop, feeding the queue; when the level quiets,
// k advances. Degrees never grow, so a queued vertex stays valid for
// its level; the survivors at level k all have degree > k, exactly
// the (k+1)-core's membership test. O(V*maxdeg + E) worst case --
// the bucket-relink version saves the maxdeg factor but this one is
// the definitional algorithm, and definitional is what a library
// wants to read.
template <version_t version_v, class R>
MJZ_CX_FN std::vector<uintlen_t>
calculate_k_cores(const basic_forest_t<version_v, R> &graph) noexcept {
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  std::vector<uintlen_t> core(n, 0);
  if (!n)
    return core;
  auto edge_ids = graph.index_range();
  auto edge_to = [&](uintlen_t e) noexcept {
    return uintlen_t(*(std::ranges::begin(graph.edges) + intlen_t(e)));
  };
  // the undirected adjacency (symmetric rows: u's row already lists
  // each incident edge exactly once; self loops never matter)
  std::vector<std::vector<uintlen_t>> adj(n);
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t e : edge_ids[u]) {
      const uintlen_t v = edge_to(e);
      if (v < n && v != u)
        adj[u].push_back(v);
    }
  }
  std::vector<uintlen_t> deg(n, 0);
  for (uintlen_t u = 0; u < n; ++u)
    deg[u] = uintlen_t(adj[u].size());
  std::vector<char> alive(n, 1);
  std::vector<uintlen_t> stack{};
  uintlen_t alive_count = n;
  for (uintlen_t k = 0; alive_count; ++k) {
    stack.clear();
    for (uintlen_t v = 0; v < n; ++v)
      if (alive[v] && deg[v] <= k)
        stack.push_back(v);
    while (!stack.empty()) {
      const uintlen_t v = stack.back();
      stack.pop_back();
      if (!alive[v])
        continue;
      asserts(deg[v] <= k); // degrees only shrink; the queue stays valid
      core[v] = k;
      alive[v] = 0;
      --alive_count;
      for (uintlen_t u : adj[v]) {
        if (!alive[u])
          continue;
        if (deg[u] > 0)
          --deg[u];
        if (deg[u] <= k)
          stack.push_back(u);
      }
    }
  }
  return core;
}

};     // namespace mjz::graph_ns
#endif // MJZ_SRC_GRAPH_biconnect_FILE_
