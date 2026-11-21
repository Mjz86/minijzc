
#ifndef MJZ_SRC_GRAPH_matching_FILE_
#define MJZ_SRC_GRAPH_matching_FILE_
#include "csr.hpp"

MJZ_EXPORT
//
namespace mjz::graph_ns {

// THE MATCHING LAYER.
//
//   * BIPARTITE maximum matching over a CSR relation: left rows over
//     right ids (the same bipartite shape make_basic_inv_forest_
//     universe transposes). TWO algorithms: Hopcroft-Karp (the O(E
//     sqrt(V)) layered engine) and Kuhn's augmenting path (the O(V*E)
//     classic, kept as the family's simple sibling -- and as the
//     fuzzer's independent cross-check).
//   * KONIG's minimum vertex cover of the same bipartite graph, read
//     off a maximum matching by the alternating-reachability sweep
//     (|cover| == |matching| -- the duality the fuzzer pins).
//   * the MINIMUM PATH COVER of a DAG (Dilworth's lens): the fewest
//     vertex-disjoint paths covering the nodes = n - a maximum
//     bipartite matching over the split graph (tails x heads); the
//     chains are walked off the matched succ/pred pairing.
//   * GENERAL maximum matching (Edmonds' blossoms) over a symmetric
//     CSR graph: one alternating search per exposed vertex, the odd
//     cycles melted into even supernodes in place (a base union-find
//     and re-pointed parents -- no graph is ever rebuilt), iterative.
//
// THE CONVENTIONS. Bipartite input: `left_rows[l]` lists right ids
// (duplicates legal -- they are separate adjacency slots); `right_
// count` sizes the right space. General input: the graph is UNDIRECTED
// -- every edge must appear in BOTH endpoint rows (each direction is
// a separate arc of the same undirected edge; parallel arcs and self
// loops are tolerated, self loops never match). none_v marks the
// unmatched.

template <version_t version_v> struct bipartite_matching_result_t {
  std::vector<uintlen_t> right_of_left{}; // none_v = free
  std::vector<uintlen_t> left_of_right{}; // none_v = free
  uintlen_t size{};
};
template <version_t version_v> struct maximum_matching_result_t {
  std::vector<uintlen_t> mate{}; // none_v = unmatched (undirected)
  uintlen_t size{};
};

// ---- Hopcroft-Karp ---------------------------------------------------------
template <version_t version_v>
MJZ_CX_FN bipartite_matching_result_t<version_v>
calculate_maximum_bipartite_matching(const auto &left_rows,
                                     uintlen_t right_count) noexcept {
  bipartite_matching_result_t<version_v> ret{};
  const uintlen_t nl = uintlen_t(std::ranges::size(left_rows));
  constexpr uintlen_t free_v = uintlen_t(-1);
  ret.right_of_left.assign(nl, free_v);
  ret.left_of_right.assign(right_count, free_v);
  ret.size = 0;
  if (!nl || !right_count)
    return ret;
  auto rows = left_rows; // sized random-access copy of the row view
  std::vector<intlen_t> dist(nl);
  std::vector<uintlen_t> queue(nl);
  auto bfs_layers = [&]() noexcept {
    uintlen_t qh = 0, qt = 0;
    std::ranges::fill(dist, -1);
    for (uintlen_t l = 0; l < nl; ++l) {
      if (ret.right_of_left[l] == free_v) {
        dist[l] = 0;
        queue[qt++] = l;
      }
    }
    while (qh < qt) {
      const uintlen_t l = queue[qh++];
      for (uintlen_t r : rows[l]) {
        if (r >= right_count)
          continue; // junk slot: skipped, not fatal (the augmenter's rule)
        const uintlen_t l2 = ret.left_of_right[r];
        if (l2 != free_v && dist[l2] < 0) {
          dist[l2] = dist[l] + 1;
          queue[qt++] = l2;
        }
      }
    }
  };
  // try to augment from l; true if a free right node was reached
  // (iterative: the recursion is a right-then-left alternation, kept
  // as an explicit frame stack -- node, row cursor)
  std::vector<uintlen_t> frame_node{}, frame_cursor{};
  auto try_augment = [&](uintlen_t root) noexcept {
    frame_node.clear();
    frame_cursor.clear();
    frame_node.push_back(root);
    frame_cursor.push_back(0);
    // the trail of (left, right) pairs chosen on the way down; on
    // success the pairs re-pair along the whole stack
    struct step_t {
      uintlen_t l, r;
    };
    std::vector<step_t> trail{};
    while (!frame_node.empty()) {
      const uintlen_t l = frame_node.back();
      if (frame_cursor.back() >= uintlen_t(std::ranges::size(rows[l]))) {
        // exhausted: backtrack
        frame_node.pop_back();
        frame_cursor.pop_back();
        if (!trail.empty() && !frame_node.empty())
          trail.pop_back(); // the step that led here is abandoned
        continue;
      }
      const uintlen_t r = uintlen_t(
          *(std::ranges::begin(rows[l]) + intlen_t(frame_cursor.back()++)));
      if (r >= right_count)
        continue; // junk slot: skipped, not fatal
      const uintlen_t l2 = ret.left_of_right[r];
      if (l2 == free_v) {
        // augmenting path found: re-pair the trail plus this step
        trail.push_back({l, r});
        for (uintlen_t k = 0; k < trail.size(); ++k) {
          ret.right_of_left[trail[k].l] = trail[k].r;
          ret.left_of_right[trail[k].r] = trail[k].l;
        }
        return true;
      }
      if (dist[l2] == dist[l] + 1) {
        // continue the layered DFS through the matched left node
        trail.push_back({l, r});
        frame_node.push_back(l2);
        frame_cursor.push_back(0);
      }
    }
    return false;
  };
  for (;;) {
    bfs_layers();
    uintlen_t augmented = 0;
    for (uintlen_t l = 0; l < nl; ++l) {
      if (ret.right_of_left[l] == free_v && dist[l] == 0)
        augmented += try_augment(l) ? 1 : 0;
    }
    if (!augmented)
      break;
    ret.size += augmented;
  }
  return ret;
}

// ---- Kuhn ------------------------------------------------------------------
// the simple sibling: one DFS per left node, no layers -- O(V*E)
template <version_t version_v>
MJZ_CX_FN bipartite_matching_result_t<version_v>
calculate_maximum_bipartite_matching_kuhn(const auto &left_rows,
                                          uintlen_t right_count) noexcept {
  bipartite_matching_result_t<version_v> ret{};
  const uintlen_t nl = uintlen_t(std::ranges::size(left_rows));
  constexpr uintlen_t free_v = uintlen_t(-1);
  ret.right_of_left.assign(nl, free_v);
  ret.left_of_right.assign(right_count, free_v);
  ret.size = 0;
  std::vector<char> visited(right_count + 1, 0);
  auto rows = left_rows;
  // iterative DFS with the classic (node, cursor) frames; the trail
  // re-pairs on success exactly like Hopcroft-Karp's try_augment
  std::vector<uintlen_t> frame_node{}, frame_cursor{};
  struct step_t {
    uintlen_t l, r;
  };
  std::vector<step_t> trail{};
  for (uintlen_t root = 0; root < nl; ++root) {
    if (ret.right_of_left[root] != free_v)
      continue;
    std::ranges::fill(visited, 0);
    frame_node.clear();
    frame_cursor.clear();
    trail.clear();
    frame_node.push_back(root);
    frame_cursor.push_back(0);
    while (!frame_node.empty()) {
      const uintlen_t l = frame_node.back();
      if (frame_cursor.back() >= uintlen_t(std::ranges::size(rows[l]))) {
        frame_node.pop_back();
        frame_cursor.pop_back();
        if (!trail.empty() && !frame_node.empty())
          trail.pop_back();
        continue;
      }
      const uintlen_t r = uintlen_t(
          *(std::ranges::begin(rows[l]) + intlen_t(frame_cursor.back()++)));
      if (r >= right_count)
        continue;
      if (visited[r]) {
        continue;
      }
      visited[r] = 1;
      const uintlen_t l2 = ret.left_of_right[r];
      if (l2 == free_v) {
        trail.push_back({l, r});
        for (uintlen_t k = 0; k < trail.size(); ++k) {
          ret.right_of_left[trail[k].l] = trail[k].r;
          ret.left_of_right[trail[k].r] = trail[k].l;
        }
        ret.size++;
        break;
      }
      trail.push_back({l, r});
      frame_node.push_back(l2);
      frame_cursor.push_back(0);
    }
  }
  return ret;
}

// ---- Konig's minimum vertex cover ------------------------------------------
// from a maximum matching: Z = the right nodes reachable from free
// left nodes by alternating paths (free-left -> unmatched-right is
// impossible by maximality, so the sweep alternates matched-right ->
// left -> unmatched-right); the cover is (L \ Z_L) + (R in Z_R), and
// its size equals the matching (the duality).
template <version_t version_v>
MJZ_CX_FN std::vector<char> calculate_bipartite_minimum_vertex_cover(
    const auto &left_rows, uintlen_t right_count,
    const bipartite_matching_result_t<version_v> &matching) noexcept {
  const uintlen_t nl = uintlen_t(std::ranges::size(left_rows));
  constexpr uintlen_t free_v = uintlen_t(-1);
  std::vector<char> zl(nl, 0), zr(right_count, 0);
  auto rows = left_rows;
  std::vector<uintlen_t> stack{};
  for (uintlen_t l = 0; l < nl; ++l) {
    if (matching.right_of_left[l] == free_v) {
      zl[l] = 1;
      stack.push_back(l);
    }
  }
  while (!stack.empty()) {
    const uintlen_t l = stack.back();
    stack.pop_back();
    for (uintlen_t r : rows[l]) {
      if (r >= right_count || zr[r])
        continue;
      // only UNMATCHED (l,r) edges extend the alternating path (the
      // matched ones are walked the other way: right -> left)
      if (matching.right_of_left[l] == r)
        continue;
      zr[r] = 1;
      const uintlen_t l2 = matching.left_of_right[r];
      if (l2 != free_v && !zl[l2]) {
        zl[l2] = 1;
        stack.push_back(l2);
      }
    }
  }
  std::vector<char> cover(nl + right_count, 0);
  for (uintlen_t l = 0; l < nl; ++l)
    cover[l] = !zl[l]; // L \ Z
  for (uintlen_t r = 0; r < right_count; ++r)
    cover[nl + r] = zr[r]; // R in Z
  return cover;
}

// ---- Edmonds' blossoms (general graphs) ------------------------------------
// THE GENERAL MATCHING, one exposed root at a time: an alternating
// breadth-first search whose ODD vertices would be dead ends -- every
// augmenting path uses their unmatched arcs from the far end -- EXCEPT
// that an even-even arc of one tree closes an odd cycle: a BLOSSOM.
// The blossom is contracted IN PLACE: its members melt into one even
// supernode (base_, a union-find; find_ climbs to the cycle's even
// attachment point, the base), the ODD members join the scan queue --
// the whole cycle's adjacency now hangs off the even side -- and the
// search continues without touching the graph. The melt re-points
// parents along the way: a cycle member's p_ turns from its tree
// parent toward the other side of the closing arc, so the later
// augment walk -- which steps from a re-paired node to the displaced
// mate, then up p_ -- can enter the cycle at ANY member and thread it
// alternately to the base: down one side, across the closing arc, up
// the other (the blossom lemma). Nested blossoms need no special
// case: the melts walk the chains by the STABLE pointers only (an
// odd's original parent, an even's mate -- neither is ever
// re-written), so a new cycle threads straight through the inner
// ones, absorbing their supernodes whole, and the inner wiring keeps
// working for whichever member a later walk enters by.
// `graph` must be symmetric (each undirected edge in both rows);
// parallel arcs are separate slots; self loops never match.
// Complexity: O(V^2 * E) worst case -- library scale by design.
namespace detail {
constexpr uintlen_t blossom_free_v = uintlen_t(-1);

template <version_t version_v, class R>
MJZ_CX_FN void blossom_solve_(const basic_forest_t<version_v, R> &graph,
                              std::vector<uintlen_t> &mate) noexcept {
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  mate.assign(n, blossom_free_v);
  if (!n)
    return;
  auto rows = graph.range(); // row slots -> TARGETS
  // p_: the alternating-tree parent -- an odd's p_ is the even that
  // scanned it (an unmatched arc), a fresh even's p_ is its mate (a
  // matched one), and a melted member's p_ is its cycle turn. state_:
  // the ORIGINAL parity (-1 unseen, 0 even, 1 odd) -- melted odds keep
  // theirs; the supernode tells them apart at the queue. base_: the
  // contraction -- a melted node's find_ is its blossom's base
  std::vector<uintlen_t> p_(n, blossom_free_v), base_(n, 0);
  std::vector<uintlen_t> queue{}, mark_(n, blossom_free_v);
  std::vector<intlen_t> state_(n, -1);
  for (uintlen_t i = 0; i < n; ++i)
    base_[i] = i;
  uintlen_t token = 0;
  auto find_ = [&](uintlen_t x) noexcept {
    while (base_[x] != x) {
      base_[x] = base_[base_[x]];
      x = base_[x];
    }
    return x;
  };
  // the stable up-step: an even climbs through its mate (an odd, one
  // arc up), an odd through its original parent -- neither pointer is
  // ever re-written by a melt, so every chain walk below is immune to
  // whatever the melts have re-pointed
  auto step_up_ = [&](uintlen_t v) noexcept {
    return state_[v] == 0 ? p_[mate[v]] : p_[v];
  };
  // the first common supernode of the two closing endpoints: mark one
  // root chain, sweep the other (both end at the root, which carries
  // the first chain's mark -- the sweep cannot run past it)
  auto lca_ = [&](uintlen_t a, uintlen_t b) noexcept {
    ++token;
    uintlen_t v = a;
    for (;;) {
      mark_[find_(v)] = token;
      if (state_[v] == 0 && mate[v] == blossom_free_v)
        break; // the root
      v = step_up_(v);
    }
    v = b;
    for (;;) {
      if (mark_[find_(v)] == token)
        return find_(v);
      v = step_up_(v);
    }
  };
  // the melt: walk the endpoint's chain up to the base, turning every
  // member's p_ toward `other` (the endpoint across the closing arc,
  // then each odd down the cycle side), folding the crossed
  // supernodes into the base and queueing the odds -- interior to an
  // even supernode now, their arcs join the scan
  auto melt_ = [&](uintlen_t x, uintlen_t other, uintlen_t b) noexcept {
    uintlen_t v = x, child = other;
    while (find_(v) != b) {
      const uintlen_t up = step_up_(v); // read BEFORE the re-point
      p_[v] = child;
      base_[find_(v)] = b;
      queue.push_back(v);
      if (state_[v] == 0) {
        const uintlen_t odd = mate[v]; // the odd above, on the cycle
        base_[find_(odd)] = b;
        queue.push_back(odd);
        child = odd;
      } else {
        child = v; // the odd endpoint: the even above turns toward it
      }
      v = up;
    }
  };
  // the augment: re-pair along the alternating path from the exposed
  // end to the root -- each step's partner comes from p_, the
  // partner's displaced mate walks on (the root's mate is free; the
  // walk ends there)
  auto augment_ = [&](uintlen_t w) noexcept {
    uintlen_t v = w;
    while (v != blossom_free_v) {
      const uintlen_t pv = p_[v];
      const uintlen_t nv = mate[pv];
      mate[v] = pv;
      mate[pv] = v;
      v = nv;
    }
  };
  for (uintlen_t root = 0; root < n; ++root) {
    if (mate[root] != blossom_free_v)
      continue; // only the exposed start searches
    std::ranges::fill(state_, intlen_t(-1));
    std::ranges::fill(p_, blossom_free_v);
    for (uintlen_t i = 0; i < n; ++i)
      base_[i] = i; // the contractions of one search never leak
    queue.clear();
    state_[root] = 0;
    queue.push_back(root);
    bool augmented = false;
    for (uintlen_t qh = 0; qh < queue.size() && !augmented; ++qh) {
      const uintlen_t u = queue[qh];
      if (state_[u] == 1 && find_(u) == u)
        continue; // a plain odd: its unmatched arcs scan from their evens
      for (uintlen_t w : rows[u]) {
        if (w >= n)
          continue; // junk slot: not a neighbor
        if (find_(u) == find_(w))
          continue; // one supernode: a self loop or a cycle interior
        if (state_[w] == 1 && find_(w) == w)
          continue; // a plain odd of another blossom-free tree spot
        if (state_[w] == -1) {
          p_[w] = u;
          if (mate[w] == blossom_free_v) {
            augment_(w); // the exposed leaf: the path is open
            augmented = true;
            break;
          }
          state_[w] = 1;
          const uintlen_t x = mate[w];
          if (state_[x] == -1) {
            state_[x] = 0;
            p_[x] = w;
            queue.push_back(x);
          }
          continue;
        }
        // even meets even inside the one tree: an odd cycle closes
        const uintlen_t b = lca_(u, w);
        melt_(u, w, b);
        melt_(w, u, b);
      }
    }
  }
}
} // namespace detail

template <version_t version_v, class R>
MJZ_CX_FN maximum_matching_result_t<version_v>
calculate_maximum_matching(const basic_forest_t<version_v, R> &graph) noexcept {
  maximum_matching_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  if (!n)
    return ret;
  detail::blossom_solve_<version_v>(graph, ret.mate);
  ret.size = 0;
  for (uintlen_t u = 0; u < n; ++u)
    ret.size += ret.mate[u] != uintlen_t(-1);
  ret.size /= 2;
  return ret;
}

// ---- the minimum path cover of a DAG ---------------------------------------
// A vertex-disjoint path cover is a set of paths touching every node
// exactly once. Dilworth's lens: matching a tail u to the head v it
// flows into (the split graph's left = tails, right = heads, one slot
// per arc) fuses two paths into one, so the fewest paths is n minus a
// maximum bipartite matching. The reconstruction walks the matched
// succ/pred pairing from every headless node, starts ascending --
// deterministic in the input. The input must be a DAG (the name says
// it: matched arcs cannot cycle on a real DAG, and the walk cap is
// the belt-and-braces guard the capped resolves carry).
template <version_t version_v> struct min_path_cover_result_t {
  uintlen_t path_count{};                      // the minimum
  std::vector<uintlen_t> succ_of{};            // node -> successor / none
  std::vector<std::vector<uintlen_t>> paths{}; // the decomposition
};
template <version_t version_v, class R>
MJZ_CX_FN min_path_cover_result_t<version_v> calculate_minimum_path_cover_dag(
    const basic_forest_t<version_v, R> &graph) noexcept {
  min_path_cover_result_t<version_v> ret{};
  const uintlen_t n = uintlen_t(std::ranges::size(graph.range()));
  constexpr uintlen_t free_v = uintlen_t(-1);
  ret.succ_of.assign(n, free_v);
  if (!n)
    return ret;
  // the split relation: left = tails, right = heads (duplicates are
  // separate adjacency slots -- Hopcroft-Karp's documented tolerance;
  // junk and self loops fuse nothing and are dropped)
  std::vector<std::vector<uintlen_t>> rows(n);
  auto edge_ids = graph.index_range();
  for (uintlen_t u = 0; u < n; ++u)
    for (uintlen_t e : edge_ids[u]) {
      const uintlen_t v = graph.get_edge_to(e);
      if (v < n && v != u)
        rows[u].push_back(v);
    }
  auto m = calculate_maximum_bipartite_matching<version_v>(rows, n);
  ret.succ_of = m.right_of_left; // free_v where a path ends
  // the chains: every node no match flows into starts one
  std::vector<char> is_head(n, 1);
  for (uintlen_t u = 0; u < n; ++u) {
    const uintlen_t v = ret.succ_of[u];
    if (v != free_v && v < n)
      is_head[v] = 0;
  }
  for (uintlen_t s = 0; s < n; ++s) {
    if (!is_head[s])
      continue;
    std::vector<uintlen_t> path{};
    uintlen_t u = s, hops = 0;
    for (;;) {
      path.push_back(u);
      const uintlen_t v = ret.succ_of[u];
      if (v == free_v || v >= n || ++hops >= n)
        break; // path end / junk / the paranoia cap
      u = v;
    }
    ret.paths.push_back(std::move(path));
  }
  ret.path_count = uintlen_t(ret.paths.size());
  return ret;
}

};     // namespace mjz::graph_ns
#endif // MJZ_SRC_GRAPH_matching_FILE_
