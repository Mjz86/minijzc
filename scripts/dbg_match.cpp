// debug: the gns.match crash at seed 3
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
  const long seed = argc > 1 ? atol(argv[1]) : 3;
  const int comp = 44; // gns.match
  std::mt19937_64 rng(uint64_t(seed) * 0x9E3779B97F4A7C15ull +
                      uint64_t(comp) * 0xBF58476D1CE4E5B9ull +
                      0xD1B54A32D192ED03ull);
  const ulen nl = 1 + kz_pick(rng, 6);
  const ulen nr = 1 + kz_pick(rng, 6);
  std::vector<std::vector<ulen>> rows(nl);
  for (ulen l = 0; l < nl; ++l) {
    const int k = int(kz_pick(rng, 4));
    for (int j = 0; j < k; ++j) {
      if (kz_chance(rng, 85))
        rows[l].push_back(kz_pick(rng, nr));
      else
        rows[l].push_back(nr + kz_pick(rng, 3));
    }
  }
  std::printf("nl=%zu nr=%zu\n", size_t(nl), size_t(nr));
  for (ulen l = 0; l < nl; ++l) {
    std::printf("  L%zu:", size_t(l));
    for (ulen r : rows[l])
      std::printf(" %zu", size_t(r));
    std::printf("\n");
  }
  std::fflush(stdout);
  std::printf("calling HK...\n");
  std::fflush(stdout);
  auto hk = gns::calculate_maximum_bipartite_matching<V>(rows, nr);
  std::printf("HK ok size=%zu\n", size_t(hk.size));
  std::fflush(stdout);
  std::printf("calling Kuhn...\n");
  std::fflush(stdout);
  auto ku = gns::calculate_maximum_bipartite_matching_kuhn<V>(rows, nr);
  std::printf("Kuhn ok size=%zu\n", size_t(ku.size));
  std::fflush(stdout);
  std::printf("calling Konig...\n");
  std::fflush(stdout);
  auto cov = gns::calculate_bipartite_minimum_vertex_cover<V>(rows, nr, hk);
  std::printf("Konig ok\n");
  return 0;
}
