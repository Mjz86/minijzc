// debug: blossom size mismatch at seed 5114 (comp 45) -- dump the
// library's matching vs the bitmask DP reference, then the mate table
#define MJZ_WITH_iostream false
#define MJZ_TEST_MODE_ 1
#define MJZ_LOG_NEW_ALLOCATIONS_ 0
#define MJZ_aggregate_tuple_workaround false
#include "../mjz_lib/mjz_lib/alllib_import.hpp"
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>
using namespace mjz;
namespace gns = mjz::graph_ns;
inline constexpr version_t V{};

using ulen = mjz::uintlen_t;
static inline ulen kz_pick(std::mt19937_64 &rng, ulen n) {
  return n ? ulen(rng() % n) : 0;
}
static inline bool kz_chance(std::mt19937_64 &rng, int pct) {
  return int(rng() % 100) < pct;
}

struct KzgSymD {
  ulen n{};
  std::vector<std::pair<ulen, ulen>> edges{};
  std::vector<std::vector<ulen>> rows{};
};
static KzgSymD kzg_symmetric(std::mt19937_64 &rng, ulen n, ulen medges,
                             ulen self_pct = 10) {
  KzgSymD S{};
  S.n = n;
  S.rows.assign(n, {});
  const ulen m = kz_pick(rng, medges + 1);
  for (ulen k = 0; k < m; ++k) {
    ulen a = kz_pick(rng, n), b = kz_pick(rng, n);
    if (a == b && !kz_chance(rng, int(self_pct)))
      b = kz_pick(rng, n);
    S.edges.push_back({a, b});
    if (a == b) {
      S.rows[a].push_back(a);
      S.rows[a].push_back(a);
    } else {
      S.rows[a].push_back(b);
      S.rows[b].push_back(a);
    }
  }
  return S;
}

int main(int argc, char **argv) {
  const long seed = argc > 1 ? atol(argv[1]) : 5114;
  const int comp = argc > 2 ? atoi(argv[2]) : 45;
  std::mt19937_64 rng(uint64_t(seed) * 0x9E3779B97F4A7C15ull +
                      uint64_t(comp) * 0xBF58476D1CE4E5B9ull +
                      0xD1B54A32D192ED03ull);
  KzgSymD S = kzg_symmetric(rng, 1 + kz_pick(rng, 9), 14, 5);
  const ulen n = S.n;
  std::printf("n=%zu edges=%zu\n", size_t(n), size_t(S.edges.size()));
  std::vector<std::vector<char>> adj(n, std::vector<char>(n, 0));
  for (auto [a, b] : S.edges)
    if (a != b)
      adj[a][b] = adj[b][a] = 1;
  for (ulen e = 0; e < S.edges.size(); ++e)
    std::printf("edge %zu: %zu-%zu%s\n", size_t(e), size_t(S.edges[e].first),
                size_t(S.edges[e].second),
                S.edges[e].first == S.edges[e].second ? " (self)" : "");
  // the DP reference
  const ulen full = ulen(1) << n;
  std::vector<ulen> dp(full, 0);
  for (ulen mask = 1; mask < full; ++mask) {
    const ulen low = mask & (~mask + 1);
    const ulen lv = 63 - __builtin_clzll(low);
    ulen best = dp[mask ^ low];
    for (ulen v = 0; v < n; ++v)
      if (v != lv && (mask & (ulen(1) << v)) && adj[lv][v]) {
        const ulen pick = 1 + dp[mask ^ low ^ (ulen(1) << v)];
        best = best < pick ? pick : best;
      }
    dp[mask] = best;
  }
  std::printf("DP max = %zu\n", size_t(dp[full - 1]));
  // the library
  auto g = gns::make_basic_forest<V>(S.rows);
  auto got = gns::calculate_maximum_matching<V>(g);
  std::printf("lib size = %zu\n", size_t(got.size));
  std::printf("mates:");
  for (ulen u = 0; u < n; ++u)
    std::printf(" %ld", got.mate[u] == ulen(-1) ? -1L : long(got.mate[u]));
  std::printf("\n");
  // count matched pairs by the mate table
  ulen cnt = 0;
  for (ulen u = 0; u < n; ++u)
    if (got.mate[u] != ulen(-1))
      ++cnt;
  std::printf("mate entries = %zu (2x size = %zu)\n", size_t(cnt),
              size_t(2 * got.size));
  return 0;
}
