
#ifndef MJZ_SRC_GRAPH_rmas_FILE_
#define MJZ_SRC_GRAPH_rmas_FILE_
#include "csr.hpp"
#include "dom.hpp"
#include "flow.hpp"
#include "scc.hpp"
#include "sequence.hpp"

MJZ_EXPORT
//
namespace mjz::graph_ns {

// THE MINIMUM ARC SET LAYER -- the least arcs to REMOVE so nothing
// cycles, and the least to KEEP so every path survives. The
// reachability relation itself is reach.hpp's contract: the checker
// answers any pair from ~O(V+E) standing bytes (sketches, dominance
// intervals, a cache rule, a traversal only when they cannot decide).
// This layer never materialises that relation a second time -- one
// query API per relation -- it only SPENDS it.
//
//   * THE MINIMUM FEEDBACK ARC SET: any digraph, a weight per arc, the
//     minimum-weight arcs whose REMOVAL leaves a DAG. Exact, and
//     exponential on purpose. A removal set is valid iff some order's
//     backward arcs all lie inside it (topologically sort what
//     survives), so the minimum removal weight equals the least
//     BACKWARD weight over the vertex orders, and the Held-Karp sweep
//     over the subsets finds that: f[S] = the least backward weight
//     among the orders of S, built by choosing S's first vertex v
//     against f[S\v] plus the weight the rest of S pours into v. A
//     self loop is a one-arc cycle, backward under every order -- it
//     rides along in the answer outright. 2^V words and 2^V pool scans
//     -- the exact answer for the small and dense (a loop nest, a hot
//     CFG), and the library says so instead of a polynomial pretending.
//     (The classical polynomial special case -- reducible flow graphs,
//     Ramachandran, "Finding a minimum feedback arc set in reducible
//     flow graphs", J. Algorithms 9, 299-313, 1988 -- is not
//     reproduced; the sweep is exact on EVERY graph at 2^V cost.) The
//     survivors' topological order rides home as the certificate.
//   * THE TRANSITIVE REDUCTION of a DAG -- the UNIQUE minimum arc set
//     with the same reachability (Aho-Garey-Ullman): the successors
//     of each node are swept in topological order, and an arc u->v
//     survives iff v is not yet covered by the reach accumulated from
//     u's earlier successors. Parallel arcs collapse to the first.
//     The DAG contract is checked (a cycle fails the assert).
//   * THE MINIMUM EQUIVALENT ARC SET of a GENERAL digraph: the
//     fewest arcs preserving who-reaches-whom (the minimum equivalent
//     graph) -- exact between SCCs (the condensation is a DAG, and
//     its transitive reduction is the unique inter-component
//     minimum, mapped back to one original arc per condensed arc --
//     the first in input order), plus a documented approximation
//     inside each SCC (an in-tree to the component's lowest-id member
//     unioned with an out-tree from it: 2*(|S|-1) arcs worst case, at
//     most twice the optimum -- the exact minimum equivalent graph is
//     NP-hard there, and the library says so instead of pretending).
//     The REMOVAL twin -- the least arcs whose deletion leaves a DAG
//     -- is the feedback arc set above.
//   * CYCLE NODES: the nodes sitting on some cycle (SCC size > 1 or a
//     self loop) -- the helper every client of the above ends up
//     writing, and the DAG contract checker.
//
// THE CONVENTIONS. weights are unsigned, per INPUT arc id, and every
// sum accumulates in weight_t; kept and removed arcs are INPUT arc
// ids, ascending; everything is a pure function of the CSR input.

// ---- the minimum feedback arc set --------------------------------------------
template <version_t version_v, class weight_t>
struct minimum_feedback_arc_set_result_t {
  std::vector<uintlen_t> removed_arcs{}; // the FAS: INPUT arc ids, ascending
  std::vector<uintlen_t> order{}; // the survivors' topological order
  weight_t total_weight{};        // the minimum the graph must pay
};

template <version_t version_v, class R,
          usable_range_exact_c<version_v> weights_t,
          std::unsigned_integral weight_t = std::ranges::range_value_t<weights_t>>
MJZ_CX_FN minimum_feedback_arc_set_result_t<version_v, weight_t>
calculate_minimum_feedback_arc_set(
    const basic_forest_t<version_v, R> &graph,
    const weights_t &weights) noexcept {
  minimum_feedback_arc_set_result_t<version_v, weight_t> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  const uintlen_t nedges = uintlen_t(std::ranges::size(graph.edges));
  if (!n)
    return ret;
  asserts(asserts.assume_rn, std::ranges::size(weights) == nedges);
  asserts(n < 26); // the sweep is exact and exponential: 2^V -- the
                   // caller holds the size class, the assert the shift
  auto rows = graph.range();           // row slots -> TARGETS
  auto edge_ids = graph.index_range(); // row slots -> arc ids
  auto edge_to = [&](uintlen_t e) noexcept {
    return uintlen_t(*(std::ranges::begin(graph.edges) + intlen_t(e)));
  };
  auto it_w = std::ranges::begin(weights);
  auto wof = [&](uintlen_t e) noexcept { return weight_t(*(it_w + intlen_t(e))); };

  // the pool: arcs between DISTINCT nodes, filed under their head --
  // the sweep asks one question per (subset, head): what does the rest
  // of the subset pour into this head. Self loops never enter the pool
  // (their cycle closes without a second node); junk slots are not arcs
  std::vector<std::vector<pair_t<uintlen_t, uintlen_t>>> pool(n);
  std::vector<char> rm(nedges, 0);
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t e : edge_ids[u]) {
      asserts(e < nedges);
      const uintlen_t v = edge_to(e);
      if (v >= n)
        continue; // a junk slot: not an arc
      if (v == u) {
        rm[e] = 1; // a self loop: backward under every order
        ret.total_weight += wof(e);
        continue;
      }
      pool[v].push_back(pair_t<uintlen_t, uintlen_t>{u, e});
    }
  }

  // the sweep. f[S] = the least backward weight among the orders of S,
  // counting only the pool arcs with both ends inside S: choosing S's
  // first vertex v demotes every pool arc (u,v) with u still in S to a
  // backward arc, and hands the rest to f[S\v]. f[full] is then the
  // least backward weight over ALL the orders -- the minimum removal
  // weight, by the topo-sort argument above
  const uintlen_t full = uintlen_t(1) << n;
  std::vector<weight_t> f(full, 0);
  std::vector<uintlen_t> choice(full, 0);
  for (uintlen_t S = 1; S < full; ++S) {
    weight_t best = 0;
    uintlen_t arg = 0;
    bool any = false;
    for (uintlen_t v = 0; v < n; ++v) {
      if (!(S & (uintlen_t(1) << v)))
        continue;
      const uintlen_t T = S ^ (uintlen_t(1) << v);
      weight_t cost = f[T];
      for (const pair_t<uintlen_t, uintlen_t> &ue : pool[v])
        if (T & (uintlen_t(1) << ue.first))
          cost += wof(ue.second);
      if (!any || cost < best) {
        best = cost;
        arg = v;
        any = true;
      }
    }
    asserts(any);
    f[S] = best;
    choice[S] = arg;
  }

  // the certificate: replay the choices front to back -- each pick is
  // the next entry of the survivors' topological order, and every pool
  // arc out of the still-unordered tail into the pick is backward
  {
    uintlen_t S = full - 1;
    while (S) {
      const uintlen_t v = choice[S];
      const uintlen_t T = S ^ (uintlen_t(1) << v);
      for (const pair_t<uintlen_t, uintlen_t> &ue : pool[v])
        if (T & (uintlen_t(1) << ue.first)) {
          rm[ue.second] = 1; // u is ordered AFTER v: the arc runs backward
          ret.total_weight += wof(ue.second);
        }
      ret.order.push_back(v);
      S = T;
    }
  }
  for (uintlen_t e = 0; e < nedges; ++e)
    if (rm[e])
      ret.removed_arcs.push_back(e); // row-major scan: ascending
  return ret;
}

// ---- the minimum feedback arc set of a REDUCIBLE flow graph ------------------
//
// THE POLYNOMIAL CASE. The sweep above is exact on every digraph at
// 2^V cost; on REDUCIBLE flow graphs the answer is polynomial
// (Ramachandran, "Finding a minimum feedback arc set in reducible
// flow graphs", J. Algorithms 9, 299-313, 1988 -- with the min-max
// companion, "A minimax arc theorem for reducible flow graphs",
// SIAM J. Disc. Math. 1990: the minimum feedback arc set weight
// equals the maximum arc-capacity-respecting cycle packing), and
// this is that algorithm as the library spells it: a cycle PACKING
// (the dual witness) priced to respect the outer loops, then one
// Dinic min cut per natural loop outermost-first, each loop's arcs
// priced by the packing loads of the loops inside it.
//
//   * THE CONTRACT CHECK. A rooted graph (the root reaching
//     everything) is reducible iff stripping the arcs u->v whose
//     target DOMINATES their source leaves a DAG. Both directions
//     are constructive: a simple cycle through such a back arc
//     u->v enters v exactly once, so every cycle member reaches u
//     avoiding v and is therefore dominated by v -- v is an
//     on-cycle header; while a cycle of the stripped graph would
//     own no on-cycle dominator at all (its arc into any candidate
//     header would be a dominator back arc, already stripped).
//     The check is the function's first act; an irreducible input
//     reports itself in the result and returns empty -- the exact
//     sweep above remains the answer there.
//   * THE STRUCTURE THE ALGORITHM RIDES. Every cycle of a
//     reducible graph owns EXACTLY ONE on-cycle header (two
//     headers on one simple cycle would dominate each other), so
//     the cycles partition by header: header h's cycles are
//     exactly the h_out -> h_in paths of h's region with the
//     header node split in two. The natural loops are laminar
//     (disjoint or nested), every entry to a region targets its
//     header, and an outer cycle crosses an inner region only
//     along header -> ... -> exit TRANSIT paths -- never along the
//     inner loop's back arcs (that would revisit the header it
//     already used to enter). Back arcs are private to their own
//     loop's demand; transit arcs are shared with every enclosing
//     loop, and that sharing is the whole subtlety.
//   * WHY THE NAIVE GREEDY IS WRONG, BOTH WAYS. Breaking loop L's
//     cycles is a min cut of L's split network, but committing
//     each loop's ISOLATED min cut -- innermost or outermost
//     first -- fails in both directions and the library can spell
//     both counterexamples. Innermost-first: an inner loop of two
//     arcs, a cheap back arc (1) and a heavy transit arc (10) that
//     an outer cycle also crosses, the outer's own arcs at 10 --
//     the isolated inner cut pays the back arc, the outer pays 10
//     in its tail, the truth pays the transit arc ONCE. Outermost-
//     first: the mirrored shape. The fix is the SHARED-CAPACITY
//     view: the two cuts are one ledger.
//   * THE PACKING (the dual witness). Pack the cycles loop by
//     loop, INNERMOST FIRST, as flows over the SHARED arc
//     capacities: loop L's packing is a max flow h_out -> h_in in
//     L's split network over what the inner packings left, and
//     the loads it leaves are the bookkeeping. The decomposition
//     choice matters (a greedy max flow would spend an outer
//     loop's transit arc on an inner cycle when a private detour
//     existed), so the packing flow is a MIN COST flow: an arc
//     costs the number of ENCLOSING loops whose paths cross it --
//     the flow prefers the private back arcs and the unshared
//     detours, leaving the contested transit for the loops that
//     have no alternative. By the min-max theorem the packing
//     total is a lower bound on every feedback arc set.
//   * THE CUTS (the primal). Walk the loops OUTERMOST FIRST. At
//     loop L every inner loop's cycles are still unbroken and L's
//     cycles wade through their regions; the min cut of L's split
//     network breaks all of L's cycles at once, and the prices
//     are the packing's: an arc e costs w(e) minus the packing
//     load the INNER loops of L route over e. The loads are the
//     right ledger where per-arc drops are not: deleting one of
//     two parallel transit arcs reroutes the inner flow (the
//     drop-based credit undercounts the pair), while the LOADS
//     sum to the inner flow value over any cut of it -- the
//     group credit is exact. A fully loaded arc is free to L
//     because the weight L pays for it is paid back by the inner
//     loop's own next cut; the ties this prices (cut the shared
//     transit now, or pay inside and outside separately) are the
//     choices of equal total. Each committed cut is filtered to
//     the arcs that still lie on some h_out -> h_in path of the
//     region (free-to-L arcs no cycle of L uses are not spent).
//   * THE WITNESS. The packing total rides home with the answer:
//     a feasible packing never exceeds any feedback arc set's
//     weight (the min-max theorem), so it bounds the answer from
//     below, and when the two meet the minimum is proven BY
//     WITNESS -- no exponential reference needed. The packing is
//     greedy (innermost first, the cost steering above), so the
//     bound is honest but not always tight; the exactness of the
//     committed answer itself is what the component fuzzer pins,
//     against the exact sweep's 2^V minimum on every seed.
//
// THE CONVENTIONS. Same as the sweep: weights unsigned, per INPUT
// arc id, sums in weight_t, removed arcs INPUT ids ascending. The
// root must reach every node (dominators are the contract's
// vocabulary); an irreducible graph reports itself instead of
// pretending.
template <version_t version_v, class weight_t>
struct reducible_fas_result_t {
  std::vector<uintlen_t> removed_arcs{}; // the FAS: INPUT arc ids, ascending
  std::vector<uintlen_t> order{}; // the survivors' topological order
  weight_t total_weight{};        // the minimum the graph must pay
  weight_t packing_weight{};      // the dual witness: <= every FAS's weight
  bool reducible{};               // the contract check's verdict
};

template <version_t version_v, class R,
          usable_range_exact_c<version_v> weights_t,
          std::unsigned_integral weight_t = std::ranges::range_value_t<weights_t>>
MJZ_CX_FN reducible_fas_result_t<version_v, weight_t>
calculate_minimum_feedback_arc_set_reducible(
    uintlen_t root, const basic_forest_t<version_v, R> &graph,
    const weights_t &weights) noexcept {
  reducible_fas_result_t<version_v, weight_t> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  const uintlen_t nedges = uintlen_t(std::ranges::size(graph.edges));
  ret.reducible = true;
  if (!n)
    return ret;
  asserts(root < n);
  asserts(std::ranges::size(weights) == nedges);
  auto rows = graph.range();           // row slots -> TARGETS
  auto edge_ids = graph.index_range(); // row slots -> arc ids
  auto edge_to = [&](uintlen_t e) noexcept {
    return uintlen_t(*(std::ranges::begin(graph.edges) + intlen_t(e)));
  };
  auto it_w = std::ranges::begin(weights);
  auto wof = [&](uintlen_t e) noexcept {
    return weight_t(*(it_w + intlen_t(e)));
  };

  // ---- the arc table, the predecessors, the self loops ----------------
  std::vector<uintlen_t> from_of(nedges, uintlen_t(-1));
  std::vector<char> live(nedges, 1);
  std::vector<char> rm(nedges, 0);
  std::vector<std::vector<uintlen_t>> preds(n);
  for (uintlen_t u = 0; u < n; ++u) {
    for (uintlen_t e : edge_ids[u]) {
      asserts(e < nedges);
      from_of[e] = u;
      const uintlen_t v = edge_to(e);
      if (v >= n)
        continue; // a junk slot: not an arc
      preds[v].push_back(u);
      if (v == u) { // a one-arc cycle: backward under every order
        rm[e] = 1;
        live[e] = 0; // and gone from every later network
      }
    }
  }
  // the root-reaches-everything contract (dominators are the
  // vocabulary; the assert is the library's say-so)
  {
    std::vector<char> seen(n, 0);
    std::vector<uintlen_t> stack{root};
    seen[root] = 1;
    while (!stack.empty()) {
      const uintlen_t u = stack.back();
      stack.pop_back();
      for (uintlen_t v : rows[u])
        if (v < n && !seen[v]) {
          seen[v] = 1;
          stack.push_back(v);
        }
    }
    for (uintlen_t u = 0; u < n; ++u)
      asserts(seen[u]);
  }

  // ---- dominators, the dominance query, the reducibility check --------
  const auto idoms = calculate_dominators_from_entry<version_v>(
      root, graph, make_basic_forest<version_v>(preds));
  auto dominates = [&](uintlen_t a, uintlen_t b) noexcept -> bool {
    // a dominates b iff a sits on b's idom chain (the chain walk;
    // the interval table is the bulk answer, this is the small one)
    if (a == b)
      return true;
    for (uintlen_t x = idoms[b];; x = idoms[x]) {
      if (x == a)
        return true;
      if (x == uintlen_t(-1) || x == root || idoms[x] == x)
        return false;
    }
  };
  {
    std::vector<std::vector<uintlen_t>> stripped(n);
    for (uintlen_t u = 0; u < n; ++u) {
      for (uintlen_t e : edge_ids[u]) {
        const uintlen_t v = edge_to(e);
        if (v >= n || v == u)
          continue;
        if (dominates(v, u))
          continue; // a dominator back arc: the loops' seed
        stripped[u].push_back(v);
      }
    }
    std::vector<uintlen_t> indeg(n, 0);
    for (uintlen_t u = 0; u < n; ++u)
      for (uintlen_t v : stripped[u])
        indeg[v]++;
    std::vector<uintlen_t> queue{};
    for (uintlen_t u = 0; u < n; ++u)
      if (!indeg[u])
        queue.push_back(u);
    for (uintlen_t qh = 0; qh < queue.size(); ++qh)
      for (uintlen_t v : stripped[queue[qh]])
        if (!--indeg[v])
          queue.push_back(v);
    if (queue.size() != n) { // a cycle survived the strip: the
      // input is not this contract's; say so, answer nothing
      ret.reducible = false;
      ret.removed_arcs.clear();
      ret.order.clear();
      ret.total_weight = weight_t();
      ret.packing_weight = weight_t();
      return ret;
    }
  }

  // ---- the natural loops: one region per header, the union over the
  // header's back arcs -- the reverse flood from the back arc tails
  // that never crosses the header (the loop layer's contract spelled
  // inline; the loop layer's own API speaks dominance intervals)
  std::vector<std::vector<uintlen_t>> loops{};
  {
    std::vector<uintlen_t> headers{}; // ascending, deduplicated
    for (uintlen_t e = 0; e < nedges; ++e) {
      const uintlen_t u = from_of[e], v = edge_to(e);
      if (u < n && v < n && u != v && dominates(v, u))
        headers.push_back(v);
    }
    std::ranges::sort(headers);
    headers.erase(std::unique(headers.begin(), headers.end()), headers.end());
    for (uintlen_t h : headers) {
      std::vector<char> in(n, 0);
      std::vector<uintlen_t> stack{};
      in[h] = 1;
      for (uintlen_t e = 0; e < nedges; ++e) { // seed the back arc tails
        const uintlen_t u = from_of[e], v = edge_to(e);
        if (u < n && v < n && u != v && v == h && dominates(h, u) &&
            !in[u]) {
          in[u] = 1;
          stack.push_back(u);
        }
      }
      while (!stack.empty()) { // who reaches a tail without the header
        const uintlen_t u = stack.back();
        stack.pop_back();
        for (uintlen_t p : preds[u])
          if (p < n && !in[p]) {
            in[p] = 1;
            stack.push_back(p);
          }
      }
      std::vector<uintlen_t> members{};
      for (uintlen_t u = 0; u < n; ++u)
        if (in[u])
          members.push_back(u); // ascending, the header included
      loops.push_back(std::move(members));
    }
  }
  const uintlen_t nloops = uintlen_t(loops.size());
  if (!nloops) { // a DAG plus self loops: Kahn alone is the answer
    std::vector<uintlen_t> indeg(n, 0);
    for (uintlen_t e = 0; e < nedges; ++e)
      if (live[e] && edge_to(e) < n)
        indeg[edge_to(e)]++;
    std::vector<uintlen_t> queue{};
    for (uintlen_t u = 0; u < n; ++u)
      if (!indeg[u])
        queue.push_back(u);
    for (uintlen_t qh = 0; qh < queue.size(); ++qh) {
      const uintlen_t u = queue[qh];
      for (uintlen_t e : edge_ids[u])
        if (live[e] && edge_to(e) < n && !--indeg[edge_to(e)])
          queue.push_back(edge_to(e));
    }
    asserts(queue.size() == n);
    ret.order = std::move(queue);
    for (uintlen_t e = 0; e < nedges; ++e)
      if (rm[e]) {
        ret.removed_arcs.push_back(e); // the row scan: ascending
        ret.total_weight += wof(e);
      }
    return ret;
  }

  // ---- the region lattice: the marks, the per-node deepest region,
  // and the region tree. Laminarity makes header containment the
  // whole nesting test (an inner region can never contain an outer
  // header: two headers on one cycle would dominate each other).
  std::vector<std::vector<char>> mark(nloops);
  std::vector<uintlen_t> region_of(n, uintlen_t(-1)); // the DEEPEST region
  for (uintlen_t i = 0; i < nloops; ++i) {
    mark[i].assign(n, 0);
    for (uintlen_t x : loops[i])
      mark[i][x] = 1;
  }
  {
    std::vector<uintlen_t> by_size(nloops, 0);
    for (uintlen_t i = 0; i < nloops; ++i)
      by_size[i] = i;
    std::ranges::sort(by_size, std::ranges::less{},
                      [&](uintlen_t i) { return loops[i].size(); });
    for (uintlen_t k = nloops; k--;) // the smallest regions claim last
      for (uintlen_t x : loops[by_size[k]])
        region_of[x] = by_size[k];
  }
  std::vector<uintlen_t> parent_loop(nloops, uintlen_t(-1));
  for (uintlen_t i = 0; i < nloops; ++i) {
    const uintlen_t h = loops[i][0]; // the header: the region's first
    uintlen_t best = uintlen_t(-1);
    for (uintlen_t j = 0; j < nloops; ++j)
      if (j != i && mark[j][h] &&
          (best == uintlen_t(-1) || loops[j].size() < loops[best].size()))
        best = j;
    parent_loop[i] = best;
  }
  std::vector<std::vector<uintlen_t>> children(nloops);
  for (uintlen_t i = 0; i < nloops; ++i)
    if (parent_loop[i] != uintlen_t(-1))
      children[parent_loop[i]].push_back(i); // ascending: deterministic

  // ---- the split network of a region (the members keep their ids,
  // the header gains h_out -- arcs out of the header leave it -- and
  // h_in -- arcs into the header arrive there; skip_edge carves one
  // arc out for probes). The arc list is row-major (from id ascending,
  // then insertion order), so network edge ids index it directly.
  auto member_id_of = [&](uintlen_t i) noexcept {
    std::vector<uintlen_t> member_id(n, uintlen_t(-1));
    const uintlen_t k = uintlen_t(loops[i].size());
    for (uintlen_t idx = 0; idx < k; ++idx)
      member_id[loops[i][idx]] = idx; // members 0..k-1; h_out=k, h_in=k+1
    return member_id;
  };
  struct net_t {
    uintlen_t node_count{};
    uintlen_t h_out{};
    uintlen_t h_in{};
    std::vector<pair_t<uintlen_t, uintlen_t>> ends{}; // net edge -> (from,to)
    std::vector<uintlen_t> edge_of{};                 // net edge -> INPUT id
    std::vector<std::vector<uintlen_t>> succs{};
  };
  auto build_network = [&](uintlen_t i, uintlen_t skip_edge) noexcept {
    net_t net{};
    const uintlen_t k = uintlen_t(loops[i].size());
    const uintlen_t header = loops[i][0];
    const auto member_id = member_id_of(i);
    net.node_count = k + 2;
    net.h_out = k;
    net.h_in = k + 1;
    std::vector<std::vector<pair_t<uintlen_t, uintlen_t>>> buckets(
        net.node_count);
    for (uintlen_t u : loops[i]) {
      for (uintlen_t e : edge_ids[u]) {
        if (!live[e] || e == skip_edge)
          continue;
        const uintlen_t v = edge_to(e);
        if (v >= n || member_id[v] == uintlen_t(-1))
          continue; // junk or outside the region
        if (v == u)
          continue; // self loops ride with the global answer
        const uintlen_t fu = u == header ? net.h_out : member_id[u];
        const uintlen_t tv = v == header ? net.h_in : member_id[v];
        buckets[fu].push_back(pair_t<uintlen_t, uintlen_t>{tv, e});
      }
    }
    net.succs.assign(net.node_count, {});
    for (uintlen_t f = 0; f < net.node_count; ++f)
      for (const pair_t<uintlen_t, uintlen_t> &te : buckets[f]) {
        net.succs[f].push_back(te.first);
        net.ends.push_back(pair_t<uintlen_t, uintlen_t>{f, te.first});
        net.edge_of.push_back(te.second);
      }
    return net;
  };
  // the forward/backward sweep pair over a network: the nodes an
  // h_out walk reaches / the nodes that reach h_in -- together they
  // pick the arcs ON SOME header path (the only arcs a flow can
  // route, the only ones a cut needs)
  auto net_paths = [&](const net_t &net) noexcept {
    std::vector<char> fwd(net.node_count, 0), bwd(net.node_count, 0);
    std::vector<uintlen_t> stack{net.h_out};
    fwd[net.h_out] = 1;
    while (!stack.empty()) {
      const uintlen_t u = stack.back();
      stack.pop_back();
      for (uintlen_t v : net.succs[u])
        if (!fwd[v]) {
          fwd[v] = 1;
          stack.push_back(v);
        }
    }
    std::vector<std::vector<uintlen_t>> radj(net.node_count);
    for (const auto &[fu, tv] : net.ends)
      radj[tv].push_back(fu);
    stack = {net.h_in};
    bwd[net.h_in] = 1;
    while (!stack.empty()) {
      const uintlen_t u = stack.back();
      stack.pop_back();
      for (uintlen_t v : radj[u])
        if (!bwd[v]) {
          bwd[v] = 1;
          stack.push_back(v);
        }
    }
    return pair_t<std::vector<char>, std::vector<char>>{std::move(fwd),
                                                        std::move(bwd)};
  };

  // ---- the loop order: outermost first (region size descending, the
  // header ascending on ties -- disjoint regions are order-free); the
  // packing below walks it reversed
  std::vector<uintlen_t> walk(nloops, 0);
  for (uintlen_t i = 0; i < nloops; ++i)
    walk[i] = i;
  std::ranges::sort(walk, [&](uintlen_t a, uintlen_t b) noexcept {
    if (loops[a].size() != loops[b].size())
      return loops[a].size() > loops[b].size();
    return loops[a][0] < loops[b][0];
  });

  // ---- THE PACKING: the cycles of every loop, innermost first, as
  // min-cost flows over the SHARED capacities -- an arc costs the
  // number of ENCLOSING loops whose header paths cross it (the flow
  // spends the private arcs first and saves the contested transit
  // for the loops with no alternative), and each loop's loads ride
  // home as the discount ledger for the enclosing cut
  std::vector<std::vector<pair_t<uintlen_t, weight_t>>> loads(nloops);
  {
    std::vector<weight_t> used(nedges, weight_t());
    // the relevance: for every loop, the arcs its header paths cross
    std::vector<std::vector<char>> on_path(nloops);
    for (uintlen_t turn = 0; turn < nloops; ++turn) {
      const uintlen_t i = walk[turn];
      const auto net = build_network(i, uintlen_t(-1));
      const auto [fwd, bwd] = net_paths(net);
      on_path[i].assign(nedges, 0);
      for (uintlen_t k = 0; k < net.edge_of.size(); ++k)
        if (fwd[net.ends[k].first] && bwd[net.ends[k].second])
          on_path[i][net.edge_of[k]] = 1;
    }
    for (uintlen_t turn = nloops; turn-- > 0;) { // innermost first
      const uintlen_t i = walk[turn];
      const auto net = build_network(i, uintlen_t(-1));
      std::vector<weight_t> caps{}, costs{};
      caps.reserve(net.edge_of.size());
      costs.reserve(net.edge_of.size());
      for (uintlen_t k = 0; k < net.edge_of.size(); ++k) {
        const uintlen_t e = net.edge_of[k];
        caps.push_back(weight_t(wof(e) - (used[e] < wof(e) ? used[e] : wof(e))));
        // the price: how many ENCLOSING loops' paths cross this arc
        uintlen_t cost = 0;
        for (uintlen_t p = parent_loop[i]; p != uintlen_t(-1);
             p = parent_loop[p])
          if (on_path[p][e])
            ++cost;
        costs.push_back(weight_t(cost));
      }
      const auto flow = calculate_min_cost_flow<version_v>(
          net.h_out, net.h_in, make_basic_forest<version_v>(net.succs), caps,
          costs);
      ret.packing_weight += weight_t(flow.flow);
      for (uintlen_t k = 0; k < net.edge_of.size(); ++k) {
        const uintlen_t e = net.edge_of[k];
        const weight_t f = flow.edge_flow[k];
        if (f) {
          loads[i].push_back(pair_t<uintlen_t, weight_t>{e, f});
          used[e] += f;
        }
      }
    }
  }

  // ---- THE CUTS: outermost first, each loop's arcs priced by the
  // packing loads of its proper inner loops (the loads sum to the
  // inner flow over any of its cuts -- the group credit is exact,
  // where per-arc drop credits undercount the rerouted parallel
  // pairs), the committed cut filtered to the arcs still on some
  // header path (a free arc no cycle of the loop uses is not spent)
  std::vector<weight_t> disc(nedges, weight_t());
  std::vector<uintlen_t> touched{};
  for (uintlen_t turn = 0; turn < nloops; ++turn) {
    const uintlen_t i = walk[turn];
    // the inner packing loads, summed per arc (static: the packing
    // ran on the whole graph before any cut)
    for (uintlen_t e : touched)
      disc[e] = weight_t();
    touched.clear();
    {
      std::vector<uintlen_t> stack{i};
      while (!stack.empty()) {
        const uintlen_t j = stack.back();
        stack.pop_back();
        for (uintlen_t c : children[j]) {
          for (const pair_t<uintlen_t, weight_t> &ed : loads[c]) {
            if (!disc[ed.first])
              touched.push_back(ed.first);
            disc[ed.first] += ed.second;
          }
          stack.push_back(c);
        }
      }
    }
    const auto net = build_network(i, uintlen_t(-1));
    std::vector<weight_t> caps{};
    caps.reserve(net.edge_of.size());
    for (uintlen_t k = 0; k < net.edge_of.size(); ++k) {
      const uintlen_t e = net.edge_of[k];
      const weight_t d = disc[e] < wof(e) ? disc[e] : wof(e);
      caps.push_back(weight_t(wof(e) - d));
    }
    const auto flow = calculate_maximum_flow<version_v>(
        net.h_out, net.h_in, make_basic_forest<version_v>(net.succs), caps);
    const auto [fwd, bwd] = net_paths(net);
    std::vector<uintlen_t> commit{};
    for (uintlen_t k = 0; k < net.edge_of.size(); ++k) {
      const auto &[fu, tv] = net.ends[k];
      if (flow.cut_side[fu] && !flow.cut_side[tv] && fwd[fu] && bwd[tv])
        commit.push_back(net.edge_of[k]);
    }
    std::ranges::sort(commit);
    for (uintlen_t e : commit) {
      asserts(live[e]);
      live[e] = 0;
      rm[e] = 1;
    }
  }

  // ---- the certificate: the survivors' topological order (Kahn; a
  // cycle among them would be a broken cut and the assert says so)
  {
    std::vector<uintlen_t> indeg(n, 0);
    for (uintlen_t e = 0; e < nedges; ++e)
      if (live[e] && edge_to(e) < n)
        indeg[edge_to(e)]++;
    std::vector<uintlen_t> queue{};
    for (uintlen_t u = 0; u < n; ++u)
      if (!indeg[u])
        queue.push_back(u);
    for (uintlen_t qh = 0; qh < queue.size(); ++qh) {
      const uintlen_t u = queue[qh];
      for (uintlen_t e : edge_ids[u])
        if (live[e] && edge_to(e) < n && !--indeg[edge_to(e)])
          queue.push_back(edge_to(e));
    }
    asserts(queue.size() == n);
    ret.order = std::move(queue);
  }
  for (uintlen_t e = 0; e < nedges; ++e)
    if (rm[e]) {
      ret.removed_arcs.push_back(e); // the row scan: ascending
      ret.total_weight += wof(e); // the self loops and the cuts, once
    }
  return ret;
}

// ---- cycle nodes (and the DAG contract checker) ------------------------------
template <version_t version_v, class R>
MJZ_CX_FN std::vector<char>
calculate_cycle_nodes(const basic_forest_t<version_v, R> &graph) noexcept {
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  std::vector<char> cyc(n, 0);
  const auto scc = calculate_strongly_connected_components<version_v>(graph);
  for (auto &&row : scc.range()) {
    if (std::ranges::size(row) > 1)
      for (uintlen_t u : row)
        cyc[u] = 1;
  }
  auto rows = graph.range();
  for (uintlen_t u = 0; u < n; ++u)
    for (uintlen_t v : rows[u])
      if (v == u)
        cyc[u] = 1;
  return cyc;
}

// ---- the transitive reduction of a DAG ---------------------------------------
template <version_t version_v>
struct transitive_reduction_result_t {
  std::vector<uintlen_t> keep_edges{}; // surviving INPUT arc ids
};

template <version_t version_v, class R>
MJZ_CX_FN transitive_reduction_result_t<version_v>
calculate_transitive_reduction_dag(
    const basic_forest_t<version_v, R> &graph) noexcept {
  transitive_reduction_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  const uintlen_t nedges = uintlen_t(std::ranges::size(graph.edges));
  if (!n)
    return ret;
  // the DAG contract: no node sits on a cycle
  {
    const auto cyc = calculate_cycle_nodes<version_v>(graph);
    for (uintlen_t u = 0; u < n; ++u)
      asserts(!cyc[u]);
  }
  auto rows = graph.range();          // targets
  auto edge_ids = graph.index_range(); // arc ids
  const uintlen_t words = (n + 63) / 64;
  std::vector<uint64_t> reach(uintlen_t(n) * words, 0);
  auto set_bit = [&](uintlen_t u, uintlen_t v) noexcept {
    reach[uintlen_t(u) * words + (v >> 6)] |= uint64_t(1) << uint32_t(v & 63);
  };
  auto test_bit = [&](uintlen_t u, uintlen_t v) noexcept -> bool {
    return (reach[uintlen_t(u) * words + (v >> 6)] >> uint32_t(v & 63)) &
           uint64_t(1);
  };
  // per-node reach, descending topological order (the wave numbers of
  // the acyclic sequenced-components kernel; a node's successors all
  // carry strictly larger waves)
  const auto [waves, wave_count] =
      calculate_acyclic_graph_sequenced_components<version_v>(graph);
  std::vector<uintlen_t> order(n, 0);
  for (uintlen_t u = 0; u < n; ++u)
    order[u] = u;
  std::ranges::sort(order, std::ranges::greater{},
                    [&](uintlen_t u) { return uintlen_t(waves[u]); });
  for (uintlen_t u : order) {
    // the successors in ASCENDING wave order (a sibling that can
    // reach another sibling is swept first), ties by arc id
    std::vector<uintlen_t> arcs{};
    for (uintlen_t e : edge_ids[u])
      if (e < nedges)
        arcs.push_back(e);
    auto target = [&](uintlen_t e) noexcept {
      return uintlen_t(*(std::ranges::begin(graph.edges) + intlen_t(e)));
    };
    std::ranges::sort(arcs, std::ranges::less{}, [&](uintlen_t e) noexcept {
      return pair_t<uintlen_t, uintlen_t>{uintlen_t(waves[target(e)]), e};
    });
    for (uintlen_t e : arcs) {
      const uintlen_t v = target(e);
      if (v >= n || v == u)
        continue; // junk slot / self loop: dropped by contract
      if (test_bit(u, v))
        continue; // covered by an earlier sibling: redundant arc
      set_bit(u, v);
      for (uintlen_t w = 0; w < words; ++w)
        reach[uintlen_t(u) * words + w] |= reach[uintlen_t(v) * words + w];
      ret.keep_edges.push_back(e);
    }
  }
  std::ranges::sort(ret.keep_edges);
  return ret;
}

// ---- the minimum equivalent arc set (general digraphs) ------------------------
template <version_t version_v>
struct minimum_equivalent_arc_set_result_t {
  std::vector<uintlen_t> keep_edges{}; // the surviving INPUT arc ids
  // honesty labels: exact between components, an approximation inside
  // (the true minimum equivalent graph is NP-hard)
  uintlen_t scc_count{};
  uintlen_t approx_scc_arcs{};
};

template <version_t version_v, class R>
MJZ_CX_FN minimum_equivalent_arc_set_result_t<version_v>
calculate_minimum_equivalent_arc_set(
    const basic_forest_t<version_v, R> &graph) noexcept {
  minimum_equivalent_arc_set_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  const uintlen_t nedges = uintlen_t(std::ranges::size(graph.edges));
  if (!n)
    return ret;
  auto rows = graph.range();          // targets
  auto edge_ids = graph.index_range(); // arc ids
  auto edge_to = [&](uintlen_t e) noexcept {
    return uintlen_t(*(std::ranges::begin(graph.edges) + intlen_t(e)));
  };
  // ---- the condensation, de-paralleled and row-sorted
  const auto scc = calculate_strongly_connected_components<version_v>(graph);
  std::vector<uintlen_t> comp_of(n, uintlen_t(-1));
  node_to_scc_map<version_v>(comp_of, scc);
  const uintlen_t ncomp = uintlen_t(std::ranges::size(scc.range()));
  ret.scc_count = ncomp;
  std::vector<std::vector<uintlen_t>> uniq(ncomp);
  {
    std::vector<std::vector<char>> seen(ncomp,
                                        std::vector<char>(ncomp, 0));
    for (uintlen_t u = 0; u < n; ++u) {
      for (uintlen_t e : edge_ids[u]) {
        const uintlen_t v = edge_to(e);
        if (v >= n)
          continue;
        const uintlen_t cu = comp_of[u], cv = comp_of[v];
        if (cu == cv)
          continue;
        if (!seen[cu][cv]) {
          seen[cu][cv] = 1;
          uniq[cu].push_back(cv);
        }
      }
    }
    for (uintlen_t c = 0; c < ncomp; ++c)
      std::ranges::sort(uniq[c]);
  }
  const treversal_result_t<version_v> condensed =
      make_basic_forest<version_v>(uniq);
  // ---- the exact inter-component minimum: the condensation's TR
  const auto red = calculate_transitive_reduction_dag<version_v>(condensed);
  std::vector<char> keep_cond(
      uintlen_t(std::ranges::size(condensed.edges)), 0);
  for (uintlen_t e : red.keep_edges)
    keep_cond[e] = 1;
  // the kept condensed arcs as a sorted (cu, cv) table for the
  // first-input-edge lookup (index_range walks ARC IDS; range() would
  // walk TARGETS -- conflating them indexes the keep mask with node
  // numbers, the row/id trap the new files kept hitting)
  std::vector<pair_t<uintlen_t, uintlen_t>> kept_pairs{};
  {
    auto cedge_ids = condensed.index_range();
    for (uintlen_t cu = 0; cu < ncomp; ++cu)
      for (uintlen_t ce : cedge_ids[cu]) {
        asserts(ce < uintlen_t(std::ranges::size(condensed.edges)));
        if (!keep_cond[ce])
          continue;
        const uintlen_t cv = uintlen_t(
            *(std::ranges::begin(condensed.edges) + intlen_t(ce)));
        kept_pairs.push_back({cu, cv});
      }
    std::ranges::sort(kept_pairs);
  }
  // map each kept pair to the FIRST input edge realizing it
  {
    std::vector<char> claimed(kept_pairs.size(), 0);
    std::vector<uintlen_t> from_of_edge(nedges, uintlen_t(-1));
    for (uintlen_t u = 0; u < n; ++u)
      for (uintlen_t e : edge_ids[u]) {
        asserts(e < nedges);
        from_of_edge[e] = u;
      }
    for (uintlen_t e = 0; e < nedges; ++e) {
      const uintlen_t u = from_of_edge[e], v = edge_to(e);
      if (u >= n || v >= n)
        continue;
      const uintlen_t cu = comp_of[u], cv = comp_of[v];
      if (cu == cv)
        continue;
      // the kept-pairs binary search (sorted pair_t compares
      // lexicographically)
      const auto it = std::ranges::lower_bound(
          kept_pairs, pair_t<uintlen_t, uintlen_t>{cu, cv});
      if (it != kept_pairs.end() && it->first == cu && it->second == cv) {
        const uintlen_t k = uintlen_t(it - kept_pairs.begin());
        if (!claimed[k]) {
          claimed[k] = 1;
          ret.keep_edges.push_back(e);
        }
      }
    }
  }
  // ---- inside each SCC: the in-tree + out-tree around the lowest-id
  // member (2*(|S|-1) arcs worst case, <= 2x the optimum; the exact
  // answer is NP-hard and the library does not fake it)
  {
    uintlen_t ci = 0;
    for (auto &&row : scc.range()) {
      const uintlen_t size = uintlen_t(std::ranges::size(row));
      if (size > 1) {
        uintlen_t rep = row[0];
        for (uintlen_t u : row)
          rep = rep < u ? rep : u;
        std::vector<uintlen_t> index_of(n, uintlen_t(-1));
        for (uintlen_t i = 0; i < size; ++i)
          index_of[row[i]] = i;
        // the in-tree: reverse BFS toward the rep (each member picks
        // one arc INTO the growing tree)
        {
          std::vector<uintlen_t> parent_edge(size, uintlen_t(-1));
          std::vector<char> seen(size, 0);
          std::vector<uintlen_t> queue{rep};
          seen[index_of[rep]] = 1;
          for (uintlen_t qh = 0; qh < queue.size(); ++qh) {
            const uintlen_t v = queue[qh];
            for (uintlen_t i = 0; i < size; ++i) {
              const uintlen_t u = row[i];
              if (seen[i])
                continue;
              for (uintlen_t e : edge_ids[u]) {
                if (edge_to(e) == v) {
                  seen[i] = 1;
                  parent_edge[i] = e;
                  queue.push_back(u);
                  break;
                }
              }
            }
          }
          for (uintlen_t i = 0; i < size; ++i)
            if (parent_edge[i] != uintlen_t(-1)) {
              ret.keep_edges.push_back(parent_edge[i]);
              ret.approx_scc_arcs++;
            }
        }
        // the out-tree: forward BFS from the rep
        {
          std::vector<uintlen_t> parent_edge(size, uintlen_t(-1));
          std::vector<char> seen(size, 0);
          std::vector<uintlen_t> index_of2(n, uintlen_t(-1));
          for (uintlen_t i = 0; i < size; ++i)
            index_of2[row[i]] = i;
          std::vector<uintlen_t> queue{rep};
          seen[index_of2[rep]] = 1;
          for (uintlen_t qh = 0; qh < queue.size(); ++qh) {
            const uintlen_t u = queue[qh];
            for (uintlen_t e : edge_ids[u]) {
              const uintlen_t v = edge_to(e);
              if (v >= n || comp_of[v] != ci || v == u)
                continue;
              if (seen[index_of2[v]])
                continue;
              seen[index_of2[v]] = 1;
              parent_edge[index_of2[v]] = e;
              queue.push_back(v);
            }
          }
          for (uintlen_t i = 0; i < size; ++i)
            if (parent_edge[i] != uintlen_t(-1)) {
              ret.keep_edges.push_back(parent_edge[i]);
              ret.approx_scc_arcs++;
            }
        }
      }
      ++ci;
    }
  }
  std::ranges::sort(ret.keep_edges);
  ret.keep_edges.erase(std::unique(ret.keep_edges.begin(),
                                   ret.keep_edges.end()),
                       ret.keep_edges.end());
  return ret;
}

}; // namespace mjz::graph_ns
#endif // MJZ_SRC_GRAPH_rmas_FILE_
