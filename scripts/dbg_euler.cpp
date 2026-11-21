// debug: euler undirected mismatches (dump library vs reference walk)
#define MJZ_WITH_iostream false
#define MJZ_TEST_MODE_ 1
#define MJZ_LOG_NEW_ALLOCATIONS_ 0
#define MJZ_aggregate_tuple_workaround false
#include "src/minijz/graph_lib.hpp"
#include <cstdio>
using namespace mjz;
namespace gns = mjz::graph_ns;
using ulen = uintlen_t;
static inline ulen kz_pick(std::mt19937_64 &r, ulen n) {
  return n ? ulen(r() % n) : 0;
}
static inline bool kz_chance(std::mt19937_64 &r, int pct) {
  return int(r() % 100) < pct;
}
static inline ulen kz_none() { return ulen(-1); }
static std::vector<std::vector<ulen>> digraph(std::mt19937_64 &rng, ulen n,
                                              ulen deg) {
  std::vector<std::vector<ulen>> rows(n);
  for (ulen u = 0; u < n; ++u) {
    const int k = int(kz_pick(rng, deg + 1));
    for (int j = 0; j < k; ++j)
      rows[u].push_back(kz_pick(rng, n));
  }
  return rows;
}
int main(int argc, char **argv) {
  const long seed = argc > 1 ? atol(argv[1]) : 196;
  std::mt19937_64 rng(uint64_t(seed) * 0x9E3779B97F4A7C15ull +
                      uint64_t(48) * 0xBF58476D1CE4E5B9ull +
                      0xD1B54A32D192ED03ull);
  const ulen n = 1 + kz_pick(rng, 7);
  // ---- the directed half (consumes its RNG first)
  {
    std::vector<std::vector<ulen>> rows(n);
    if (kz_chance(rng, 60)) {
      ulen cur = 0;
      const ulen steps = 1 + kz_pick(rng, 10);
      for (ulen i = 0; i < steps; ++i) {
        const ulen next = kz_pick(rng, n);
        rows[cur].push_back(next);
        cur = next;
      }
      rows[cur].push_back(0);
    } else {
      rows = digraph(rng, n, 3);
    }
  }
  // ---- the undirected half
  struct Sym {
    ulen n{};
    std::vector<std::pair<ulen, ulen>> edges{};
    std::vector<std::vector<ulen>> rows{};
    std::vector<std::vector<ulen>> slot_edge{};
  };
  Sym S{};
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
    const ulen ei = ulen(S.edges.size());
    S.edges.push_back({cur, 0});
    if (cur == 0) {
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
    // kzg_symmetric(rng, n, 8, 15)
    const ulen m = kz_pick(rng, 9);
    for (ulen k = 0; k < m; ++k) {
      ulen a = kz_pick(rng, n), b = kz_pick(rng, n);
      if (a == b && !kz_chance(rng, 15))
        b = kz_pick(rng, n);
      S.edges.push_back({a, b});
      const ulen ei = ulen(S.edges.size() - 1);
      if (a == b) {
        S.rows[a].push_back(a);
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
  }
  const ulen nn = S.n, me = S.edges.size();
  std::printf("== euler seed %ld: n=%zu me=%zu\n", seed, size_t(nn),
              size_t(me));
  for (ulen ei = 0; ei < me; ++ei)
    std::printf("  edge %zu: %zu-%zu\n", size_t(ei), size_t(S.edges[ei].first),
                size_t(S.edges[ei].second));
  // the reference conditions
  std::vector<ulen> deg(nn, 0);
  for (ulen u = 0; u < nn; ++u)
    for (ulen v : S.rows[u])
      if (v != u)
        deg[u]++;
  bool one_piece = true;
  {
    gns::disjoint_set_union_t<V, std::vector<uintlen_t>> dsu{};
    dsu.parent_then_rank.assign(nn, 0);
    dsu.init(nn);
    std::vector<char> has(nn, 0);
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
  std::printf("one_piece=%d odd=%zu (%zu,%zu) deg:", int(one_piece),
              size_t(odd), size_t(odd_a), size_t(odd_b));
  for (ulen u = 0; u < nn; ++u)
    std::printf(" %zu", size_t(deg[u]));
  std::printf("\n");
  auto g = gns::make_basic_forest<V>(S.rows);
  std::printf("arcs (row-major):\n");
  for (ulen u = 0; u < nn; ++u)
    for (ulen e : g.index_range()[u])
      std::printf("  arc %zu: %zu -> %zu (edge %zu)\n", size_t(e), size_t(u),
                  size_t(g.get_edge_to(e)),
                  size_t(S.slot_edge[u][0] == kz_none() ? 0 : 0));
  auto circ = gns::calculate_eulerian_circuit_undirected<V>(g);
  auto path = gns::calculate_eulerian_path_undirected<V>(g);
  std::printf("circ: ok=%d first=%zu edges:", int(circ.ok),
              circ.ok ? size_t(circ.first_node) : 0);
  for (ulen e : circ.edges)
    std::printf(" %zu(%zu->%zu)", size_t(e), size_t(g.get_edge_from(e)),
                size_t(g.get_edge_to(e)));
  std::printf("\n");
  std::printf("path: ok=%d first=%zu edges:", int(path.ok),
              path.ok ? size_t(path.first_node) : 0);
  for (ulen e : path.edges)
    std::printf(" %zu(%zu->%zu)", size_t(e), size_t(g.get_edge_from(e)),
                size_t(g.get_edge_to(e)));
  std::printf("\n");
  return 0;
}
