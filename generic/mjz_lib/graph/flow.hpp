
#ifndef MJZ_SRC_GRAPH_flow_FILE_
#define MJZ_SRC_GRAPH_flow_FILE_
#include "csr.hpp"

MJZ_EXPORT
//
namespace mjz::graph_ns {

// THE FLOW LAYER. Dinic's maximum flow over a CSR digraph with
// per-edge capacities, the s-t minimum cut read off the final
// residual, and the min-cost successive-shortest-path flow built on
// the same residual skeleton.
//
// THE MODEL. The client's graph is the CAPACITY graph: edge i (u->v)
// has capacity caps[i]. Internally each edge i becomes the residual
// ARC PAIR (2i, 2i^1): arc 2i starts with caps[i] of headroom, its
// twin 2i^1 with zero; pushing f along an edge moves f from 2i's
// headroom to 2i^1's. Parallel edges are legal input (each is its own
// pair); a self loop never carries flow (its two arcs share one
// node, so the level rule never crosses it). An UNDIRECTED client
// passes each undirected edge as two input arcs (u->v and v->u, the
// capacity split between them) -- undirected max flow is a
// convention, not an algorithm.
//
// THE WEIGHTS. cap_t must be an unsigned integer (or one of the
// uintN fixed widths): residuals are compared to zero and subtracted,
// which is only sound without signedness traps. The flow VALUE is at
// most the sum of capacities leaving the source -- pick a width where
// that sum fits.
template <version_t version_v, class cap_t>
struct max_flow_result_t {
  cap_t flow{};                   // the maximum source->sink value
  std::vector<cap_t> edge_flow{}; // per INPUT edge: the net flow it carries
  std::vector<char> cut_side{};   // the MIN CUT's source side (post-flow)
};

namespace detail {

template <class cap_t>
concept flow_cap_c = std::unsigned_integral<cap_t> || requires() {
  typename cap_t::mjz_uintN_t_id_val_t_2354675648764874753789;
};

// the residual skeleton every flow algorithm here shares: the arc
// pairs, the per-node arc rows (each row's arcs in ascending edge
// index -- one ascending scan fills them deterministically), the
// headroom, and the edge endpoints cached once
template <version_t version_v, flow_cap_c cap_t>
struct residual_graph_t {
  uintlen_t node_count{};
  std::vector<uintlen_t> edge_from{};    // per input edge
  std::vector<uintlen_t> edge_to{};      // per input edge
  std::vector<uintlen_t> arcs_of_node{}; // CSR: node -> arc row bounds
  std::vector<uintlen_t> arc_ids{};      // the row-flattened arc list
  std::vector<uintlen_t> arc_to{};       // arc -> head node
  std::vector<cap_t> arc_headroom{};     // arc -> residual capacity

  MJZ_CX_FN uintlen_t arc_tail(uintlen_t arc) const noexcept {
    return arc_to[arc ^ 1];
  }
  MJZ_CX_FN cap_t arc_cap(uintlen_t arc) const noexcept {
    return arc_headroom[arc];
  }
  MJZ_CX_FN void arc_push(uintlen_t arc, cap_t f) noexcept {
    asserts(asserts.assume_rn, f <= arc_headroom[arc]);
    arc_headroom[arc] -= f;
    arc_headroom[arc ^ 1] += f;
  }
  MJZ_CX_FN uintlen_t arc_row_begin(uintlen_t node) const noexcept {
    return arcs_of_node[node];
  }
  MJZ_CX_FN uintlen_t arc_row_end(uintlen_t node) const noexcept {
    return arcs_of_node[node + 1];
  }

  template <class R, usable_range_exact_c<version_v> caps_t>
  MJZ_CX_FN static residual_graph_t
  make(const basic_forest_t<version_v, R> &graph,
       const caps_t &caps) noexcept {
    residual_graph_t ret{};
    ret.node_count = uintlen_t(std::ranges::size(graph.range()));
    const uintlen_t nedges = uintlen_t(std::ranges::size(graph.edges));
    ret.edge_from.resize(nedges);
    ret.edge_to.resize(nedges);
    auto it_caps = std::ranges::begin(caps);
    auto edge_ids = graph.index_range(); // row slots -> arc ids
    for (uintlen_t u = 0; u < ret.node_count; ++u) {
      for (uintlen_t e : edge_ids[u]) {
        asserts(e < nedges);
        ret.edge_from[e] = u;
        ret.edge_to[e] = graph.get_edge_to(e);
      }
    }
    // every node owns a row slot for each arc that leaves it: arc 2i
    // (tail = edge_from[i]) and arc 2i^1 (tail = edge_to[i])
    ret.arcs_of_node.assign(ret.node_count + 1, 0);
    for (uintlen_t i = 0; i < nedges; ++i) {
      ret.arcs_of_node[ret.edge_from[i] + 1]++;
      ret.arcs_of_node[ret.edge_to[i] + 1]++;
    }
    for (uintlen_t u = 0; u < ret.node_count; ++u)
      ret.arcs_of_node[u + 1] += ret.arcs_of_node[u];
    ret.arc_ids.resize(uintlen_t(nedges * 2));
    ret.arc_to.resize(uintlen_t(nedges * 2));
    ret.arc_headroom.resize(uintlen_t(nedges * 2));
    std::vector<uintlen_t> fill = ret.arcs_of_node; // begin cursors
    for (uintlen_t i = 0; i < nedges; ++i) {
      const uintlen_t fwd = fill[ret.edge_from[i]]++;
      const uintlen_t bwd = fill[ret.edge_to[i]]++;
      ret.arc_ids[fwd] = i * 2;
      ret.arc_ids[bwd] = i * 2 + 1;
      ret.arc_to[i * 2] = ret.edge_to[i];
      ret.arc_to[i * 2 + 1] = ret.edge_from[i];
      ret.arc_headroom[i * 2] = cap_t(*(it_caps + intlen_t(i)));
      ret.arc_headroom[i * 2 + 1] = cap_t();
    }
    return ret;
  }

  // the source side of the cut the current residual defines: every
  // node reachable from `entry` through arcs with headroom
  MJZ_CX_FN std::vector<char> reachable_side(uintlen_t entry) const noexcept {
    std::vector<char> side(node_count, 0);
    std::vector<uintlen_t> stack{entry};
    side[entry] = 1;
    while (!stack.empty()) {
      const uintlen_t u = stack.back();
      stack.pop_back();
      for (uintlen_t k = arc_row_begin(u); k < arc_row_end(u); ++k) {
        const uintlen_t arc = arc_ids[k];
        if (arc_headroom[arc] && !side[arc_to[arc]]) {
          side[arc_to[arc]] = 1;
          stack.push_back(arc_to[arc]);
        }
      }
    }
    return side;
  }
};

// Dinic's level-graph phases (shared by every max-flow variant here):
// repeat (BFS levels over positive-headroom arcs, one blocking flow
// with per-node cursors) until the sink stops being leveled
template <version_t version_v, flow_cap_c cap_t>
MJZ_CX_FN void
dinic_run_(residual_graph_t<version_v, cap_t> &res, uintlen_t source,
           uintlen_t sink) noexcept {
  if (source == sink || source >= res.node_count ||
      sink >= res.node_count)
    return;
  std::vector<intlen_t> level(res.node_count);
  std::vector<uintlen_t> queue(res.node_count);
  std::vector<uintlen_t> cursor(res.node_count + 1);
  std::vector<uintlen_t> path{};
  for (;;) {
    // ---- the level graph: BFS over positive-headroom arcs
    std::ranges::fill(level, -1);
    level[source] = 0;
    uintlen_t qh = 0, qt = 0;
    queue[qt++] = source;
    while (qh < qt) {
      const uintlen_t u = queue[qh++];
      for (uintlen_t k = res.arc_row_begin(u); k < res.arc_row_end(u);
           ++k) {
        const uintlen_t arc = res.arc_ids[k];
        const uintlen_t w = res.arc_to[arc];
        if (res.arc_headroom[arc] && level[w] < 0) {
          level[w] = level[u] + 1;
          queue[qt++] = w;
        }
      }
    }
    if (level[sink] < 0)
      return; // nothing more can reach the sink
    // ---- one blocking flow over the level graph
    std::ranges::copy(res.arcs_of_node, cursor.begin());
    path.clear();
    uintlen_t v = source;
    for (;;) {
      if (v == sink) {
        // bottleneck along the path, push it, retreat to the first
        // arc the bottleneck saturated (the prefix keeps its headroom)
        cap_t f = res.arc_cap(path[0]);
        for (uintlen_t arc : path)
          f = res.arc_cap(arc) < f ? res.arc_cap(arc) : f;
        for (uintlen_t arc : path)
          res.arc_push(arc, f);
        uintlen_t k = 0;
        while (k < path.size() && res.arc_cap(path[k]))
          ++k;
        v = k < path.size() ? res.arc_tail(path[k]) : source;
        path.resize(k);
        continue;
      }
      uintlen_t arc = uintlen_t(-1);
      while (cursor[v] < res.arc_row_end(v)) {
        const uintlen_t cand = res.arc_ids[cursor[v]];
        if (res.arc_headroom[cand] &&
            level[res.arc_to[cand]] == level[v] + 1) {
          arc = cand;
          break;
        }
        ++cursor[v];
      }
      if (arc != uintlen_t(-1)) {
        path.push_back(arc);
        v = res.arc_to[arc];
        continue;
      }
      // dead end in the level graph: retire v and retreat
      level[v] = -1;
      if (v == source)
        break;
      const uintlen_t last = path.back();
      path.pop_back();
      v = res.arc_tail(last);
      ++cursor[v]; // past the arc that led into the dead node
    }
  }
}

// the net per-edge flow from a finished residual: caps[i]-headroom(2i)
// (a directed edge's headroom can only exceed its capacity by the
// amount of flow cancelled back along it, so the difference IS the
// net forward flow)
template <version_t version_v, flow_cap_c cap_t,
          usable_range_exact_c<version_v> caps_t>
MJZ_CX_FN std::vector<cap_t>
edge_flows_of_(const residual_graph_t<version_v, cap_t> &res,
               const caps_t &caps) noexcept {
  auto it_caps = std::ranges::begin(caps);
  std::vector<cap_t> edge_flow(res.edge_from.size());
  for (uintlen_t i = 0; i < edge_flow.size(); ++i)
    edge_flow[i] =
        cap_t(*(it_caps + intlen_t(i))) - res.arc_headroom[i * 2];
  return edge_flow;
}

// the net value crossing into the sink: inflow minus outflow, each
// edge's net read off its forward arc
template <version_t version_v, flow_cap_c cap_t,
          usable_range_exact_c<version_v> caps_t>
MJZ_CX_FN cap_t flow_value_of_(const residual_graph_t<version_v, cap_t> &res,
                               const caps_t &caps, uintlen_t sink) noexcept {
  auto it_caps = std::ranges::begin(caps);
  cap_t net{};
  for (uintlen_t i = 0; i < res.edge_from.size(); ++i) {
    const cap_t f =
        cap_t(*(it_caps + intlen_t(i))) - res.arc_headroom[i * 2];
    if (res.edge_to[i] == sink)
      net += f;
    if (res.edge_from[i] == sink)
      net -= f;
  }
  return net;
}
} // namespace detail

// THE MAXIMUM FLOW (Dinic: O(V^2 E) worst case, one level graph per
// blocking flow). Deterministic in the input's edge order; the VALUE
// is the maximum, the per-edge split is one witness.
template <version_t version_v, class R,
          usable_range_exact_c<version_v> caps_t,
          detail::flow_cap_c cap_t = std::ranges::range_value_t<caps_t>>
MJZ_CX_FN max_flow_result_t<version_v, cap_t>
calculate_maximum_flow(uintlen_t source, uintlen_t sink,
                       const basic_forest_t<version_v, R> &graph,
                       const caps_t &caps) noexcept {
  asserts(source != sink);
  max_flow_result_t<version_v, cap_t> ret{};
  auto res = detail::residual_graph_t<version_v, cap_t>::make(graph, caps);
  detail::dinic_run_<version_v>(res, source, sink);
  ret.flow = detail::flow_value_of_(res, caps, sink);
  ret.edge_flow = detail::edge_flows_of_(res, caps);
  ret.cut_side = res.reachable_side(source);
  return ret;
}

// THE s-t MINIMUM CUT: the max flow, the source side the final
// residual defines, and the input edges that cross it (their weight
// sum equals the flow -- max-flow min-cut, an invariant the component
// fuzzer pins)
template <version_t version_v, detail::flow_cap_c cap_t>
struct st_min_cut_result_t {
  cap_t weight{};                     // = the max flow value
  std::vector<char> source_side{};    // the S side of a minimum cut
  std::vector<uintlen_t> cut_edges{}; // input edge ids crossing S->T
};
template <version_t version_v, class R,
          usable_range_exact_c<version_v> caps_t,
          detail::flow_cap_c cap_t = std::ranges::range_value_t<caps_t>>
MJZ_CX_FN st_min_cut_result_t<version_v, cap_t>
calculate_st_min_cut(uintlen_t source, uintlen_t sink,
                     const basic_forest_t<version_v, R> &graph,
                     const caps_t &caps) noexcept {
  auto flow = calculate_maximum_flow<version_v>(source, sink, graph, caps);
  st_min_cut_result_t<version_v, cap_t> ret{};
  ret.weight = flow.flow;
  ret.source_side = std::move(flow.cut_side);
  auto edge_ids = graph.index_range();
  for (uintlen_t u = 0; u < std::ranges::size(edge_ids); ++u) {
    for (uintlen_t e : edge_ids[u]) {
      const uintlen_t v = graph.get_edge_to(e);
      if (ret.source_side[u] && !ret.source_side[v])
        ret.cut_edges.push_back(e);
    }
  }
  std::ranges::sort(ret.cut_edges);
  return ret;
}

// THE MIN-COST FLOW (successive shortest paths over the residual,
// one SPFA shortest path per augmentation -- the twin arcs carry
// negated costs, which the queue-based Bellman-Ford absorbs; the
// INPUT costs must not themselves form a negative cycle). Stops at
// `flow_limit` (none_v = push until no augmenting path exists). The
// accumulated cost is the exact minimum for the flow carried.
template <version_t version_v, detail::flow_cap_c cap_t>
struct min_cost_flow_result_t {
  cap_t flow{};
  int64_t cost{}; // int64 floor; a wider caller domain is their call
  std::vector<cap_t> edge_flow{};
};
template <version_t version_v, class R,
          usable_range_exact_c<version_v> caps_t,
          usable_range_exact_c<version_v> costs_t,
          detail::flow_cap_c cap_t = std::ranges::range_value_t<caps_t>>
MJZ_CX_FN min_cost_flow_result_t<version_v, cap_t>
calculate_min_cost_flow(uintlen_t source, uintlen_t sink,
                        const basic_forest_t<version_v, R> &graph,
                        const caps_t &caps, const costs_t &costs,
                        uintlen_t flow_limit = uintlen_t(-1)) noexcept {
  asserts(source != sink);
  min_cost_flow_result_t<version_v, cap_t> ret{};
  auto res = detail::residual_graph_t<version_v, cap_t>::make(graph, caps);
  auto it_costs = std::ranges::begin(costs);
  const uintlen_t n = res.node_count;
  constexpr int64_t unreached_v = 0x7fffffffffffffff;
  std::vector<int64_t> dist(n);
  std::vector<uintlen_t> parent_arc(n);
  std::vector<char> in_queue(n);
  std::vector<uintlen_t> queue{};
  queue.reserve(n);
  for (;;) {
    // ---- SPFA: shortest path by cost over positive-headroom arcs
    std::ranges::fill(dist, unreached_v);
    std::ranges::fill(in_queue, 0);
    dist[source] = 0;
    parent_arc[source] = uintlen_t(-1);
    queue.clear();
    queue.push_back(source);
    in_queue[source] = 1;
    for (uintlen_t qh = 0; qh < queue.size(); ++qh) {
      const uintlen_t u = queue[qh];
      in_queue[u] = 0;
      for (uintlen_t k = res.arc_row_begin(u); k < res.arc_row_end(u);
           ++k) {
        const uintlen_t arc = res.arc_ids[k];
        if (!res.arc_headroom[arc])
          continue;
        const uintlen_t w = res.arc_to[arc];
        const int64_t c = int64_t(*(it_costs + intlen_t(arc / 2)));
        const int64_t step = arc & 1 ? -c : c;
        if (dist[u] + step < dist[w]) {
          dist[w] = dist[u] + step;
          parent_arc[w] = arc;
          if (!in_queue[w]) {
            in_queue[w] = 1;
            queue.push_back(w);
          }
        }
      }
    }
    if (dist[sink] == unreached_v)
      break; // no augmenting path left
    // ---- the bottleneck along the parent chain, capped by the limit
    cap_t f{};
    {
      uintlen_t v = sink;
      bool first = true;
      while (v != source) {
        const uintlen_t arc = parent_arc[v];
        if (first) {
          f = res.arc_cap(arc);
          first = false;
        } else {
          f = res.arc_cap(arc) < f ? res.arc_cap(arc) : f;
        }
        v = res.arc_tail(arc);
      }
    }
    if (flow_limit != uintlen_t(-1)) {
      const uintlen_t room =
          flow_limit > uintlen_t(ret.flow) ? flow_limit - uintlen_t(ret.flow)
                                           : 0;
      if (uintlen_t(f) > room)
        f = cap_t(room);
      if (!f)
        break; // the caller's limit is met
    }
    uintlen_t v = sink;
    while (v != source) {
      const uintlen_t arc = parent_arc[v];
      res.arc_push(arc, f);
      v = res.arc_tail(arc);
    }
    ret.flow += f;
    ret.cost += dist[sink] * int64_t(f);
    if (flow_limit != uintlen_t(-1) && uintlen_t(ret.flow) >= flow_limit)
      break;
  }
  ret.edge_flow = detail::edge_flows_of_(res, caps);
  return ret;
}

}; // namespace mjz::graph_ns
#endif // MJZ_SRC_GRAPH_flow_FILE_
