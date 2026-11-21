// debug: kcore seed 4 -- library peel vs the per-k removal
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
int main(int argc, char **argv) {
  const long seed = argc > 1 ? atol(argv[1]) : 4;
  const int comp = 47; // gns.kcore
  std::mt19937_64 rng(uint64_t(seed) * 0x9E3779B97F4A7C15ull +
                      uint64_t(comp) * 0xBF58476D1CE4E5B9ull +
                      0xD1B54A32D192ED03ull);
  // KzgSym S = kzg_symmetric(rng, 1 + kz_pick(rng, 7), 14, 10)
  const ulen n = 1 + kz_pick(rng, 7);
  std::vector<std::pair<ulen, ulen>> edges;
  std::vector<std::vector<ulen>> rows(n), slot_edge(n);
  {
    const ulen m = kz_pick(rng, 15);
    for (ulen k = 0; k < m; ++k) {
      ulen a = kz_pick(rng, n), b = kz_pick(rng, n);
      if (a == b && !kz_chance(rng, 10))
        b = kz_pick(rng, n);
      edges.push_back({a, b});
      const ulen ei = ulen(edges.size() - 1);
      if (a == b) {
        rows[a].push_back(a);
        slot_edge[a].push_back(ei);
        rows[a].push_back(a);
        slot_edge[a].push_back(ei);
      } else {
        rows[a].push_back(b);
        slot_edge[a].push_back(ei);
        rows[b].push_back(a);
        slot_edge[b].push_back(ei);
      }
    }
  }
  std::printf("n=%zu m=%zu\n", size_t(n), size_t(edges.size()));
  for (ulen ei = 0; ei < edges.size(); ++ei)
    std::printf("  e%zu: %zu-%zu%s\n", size_t(ei), size_t(edges[ei].first),
                size_t(edges[ei].second),
                edges[ei].first == edges[ei].second ? " (self)" : "");
  // the reference: per-k iterative removal, multiplicity kept
  std::vector<std::vector<ulen>> adj(n);
  for (auto [a, b] : edges)
    if (a != b) {
      adj[a].push_back(b);
      adj[b].push_back(a);
    }
  std::vector<ulen> want(n, 0);
  for (ulen k = 0; k <= n; ++k) {
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
  auto g = gns::make_basic_forest<V>(rows);
  auto got = gns::calculate_k_cores<V>(g);
  for (ulen u = 0; u < n; ++u)
    std::printf("  core[%zu]: want=%zu got=%zu %s\n", size_t(u),
                size_t(want[u]), size_t(got[u]),
                want[u] == got[u] ? "" : "  <<<< MISMATCH");
  return 0;
}
