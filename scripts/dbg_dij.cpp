// debug: the gns.dij crash at seed 12 -- regenerate the exact input
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
  const long seed = argc > 1 ? atol(argv[1]) : 12;
  const int comp = 35; // gns.dij's index in KZ_NAMES
  std::mt19937_64 rng(uint64_t(seed) * 0x9E3779B97F4A7C15ull +
                      uint64_t(comp) * 0xBF58476D1CE4E5B9ull +
                      0xD1B54A32D192ED03ull);
  const ulen n = 1 + kz_pick(rng, 7);
  std::vector<std::vector<ulen>> succs(n);
  for (ulen b = 0; b < n; ++b) {
    const int k = int(kz_pick(rng, 4));
    for (int j = 0; j < k; ++j)
      succs[b].push_back(kz_pick(rng, n));
  }
  const ulen m = [&] {
    ulen t = 0;
    for (auto &r : succs)
      t += r.size();
    return t;
  }();
  std::vector<int64_t> w(m);
  for (auto &x : w)
    x = int64_t(kz_pick(rng, 41));
  const ulen entry = kz_pick(rng, n);
  const ulen exit_ = kz_pick(rng, n);
  // consume the same stream the fuzzer does: kzr_simple_path_min has
  // no rng use; the fuzzer order: n, succs, w, entry, then dij, then
  // rsuccs/d_exit (no rng), then the h coin and mode coin
  std::printf("n=%zu m=%zu entry=%zu exit=%zu\n", size_t(n), size_t(m),
              size_t(entry), size_t(exit_));
  for (ulen u = 0; u < n; ++u) {
    std::printf("  %zu:", size_t(u));
    for (ulen v : succs[u])
      std::printf(" %zu", size_t(v));
    std::printf("\n");
  }
  auto g = gns::make_basic_forest<V>(succs);
  std::vector<uint32_t> w32(w.begin(), w.end());
  std::printf("calling dijkstra...\n");
  std::fflush(stdout);
  auto [par, dist] =
      gns::calculate_shortest_positive_path_distanced<V>(entry, g, w32);
  std::printf("dijkstra OK\n");
  std::fflush(stdout);
  // the fuzzer's h decision (same stream position)
  const bool use_h_coin = kz_chance(rng, 70);
  const bool half = kz_chance(rng, 50);
  std::printf("h coin: use=%d half=%d\n", int(use_h_coin), int(half));
  // reconstruct d_exit (the fuzzer computes it with the reference)
  std::vector<std::vector<ulen>> rsuccs(n);
  ulen ai = 0;
  for (ulen u = 0; u < n; ++u)
    for (ulen v : succs[u])
      rsuccs[v].push_back(u);
  // true d to exit by brute simple-path min (reuse the Held-Karp idea
  // quickly: Bellman over simple paths is overkill here; do DFS min)
  std::vector<uint32_t> h(n, 0);
  {
    std::vector<char> vis(n, 0);
    std::function<uint32_t(ulen, uint32_t)> dfs = [&](ulen u,
                                                      uint32_t d) -> uint32_t {
      if (u == exit_)
        return d;
      uint32_t best = ~uint32_t(0);
      vis[u] = 1;
      ulen e0 = 0;
      for (ulen x = 0; x < u; ++x)
        e0 += succs[x].size();
      for (ulong i = 0; i < succs[u].size(); ++i) {
        const ulen v = succs[u][i];
        if (vis[v])
          continue;
        const uint32_t r = dfs(v, d + w32[e0 + i]);
        best = best < r ? best : r;
      }
      vis[u] = 0;
      return best;
    };
    ulen reach_all = 1;
    for (ulen v = 0; v < n; ++v) {
      if (v == exit_)
        continue;
      vis.assign(n, 0);
      if (dfs(v, 0) == ~uint32_t(0))
        reach_all = 0;
    }
    if (reach_all && use_h_coin) {
      for (ulen v = 0; v < n; ++v) {
        vis.assign(n, 0);
        const uint32_t d = dfs(v, 0);
        if (d != ~uint32_t(0))
          h[v] = half ? d / 2 : d;
      }
    }
    std::printf("reach_all=%d\n", int(reach_all));
  }
  for (ulen v = 0; v < n; ++v)
    std::printf("h[%zu]=%u\n", size_t(v), h[v]);
  std::printf("calling A*...\n");
  std::fflush(stdout);
  auto path = gns::calculate_A_start_path<V>(entry, exit_, g, w32, h);
  std::printf("A* OK, path len %zu\n", path.size());
  return 0;
}
