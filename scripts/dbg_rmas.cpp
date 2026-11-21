// debug: rmas seed 1 -- dump keep set vs the reference
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
struct Arc {
  ulen f, t;
};
int main(int argc, char **argv) {
  const long seed = argc > 1 ? atol(argv[1]) : 1;
  const int comp = 51; // gns.rmas
  std::mt19937_64 rng(uint64_t(seed) * 0x9E3779B97F4A7C15ull +
                      uint64_t(comp) * 0xBF58476D1CE4E5B9ull +
                      0xD1B54A32D192ED03ull);
  const ulen n = 1 + kz_pick(rng, 7);
  std::vector<std::vector<ulen>> succs(n);
  for (ulen u = 0; u < n; ++u) {
    const int k = int(kz_pick(rng, 4));
    for (int j = 0; j < k; ++j)
      succs[u].push_back(kz_pick(rng, n));
  }
  std::vector<Arc> arcs;
  for (ulen u = 0; u < n; ++u)
    for (ulen v : succs[u])
      arcs.push_back({u, v});
  const ulen m = arcs.size();
  std::printf("n=%zu m=%zu\n", size_t(n), size_t(m));
  for (ulen e = 0; e < m; ++e)
    std::printf("  a%zu: %zu->%zu\n", size_t(e), size_t(arcs[e].f),
                size_t(arcs[e].t));
  // the reference classes (mutual reachability)
  std::vector<std::vector<char>> reach(n, std::vector<char>(n, 0));
  for (ulen s = 0; s < n; ++s) {
    std::vector<ulen> wl{s};
    reach[s][s] = 1;
    while (!wl.empty()) {
      ulen u = wl.back();
      wl.pop_back();
      for (ulen v : succs[u])
        if (!reach[s][v]) {
          reach[s][v] = 1;
          wl.push_back(v);
        }
    }
  }
  std::vector<ulen> comp_of(n, ulen(-1));
  std::vector<std::vector<ulen>> classes;
  for (ulen u = 0; u < n; ++u) {
    if (comp_of[u] != ulen(-1))
      continue;
    classes.push_back({});
    for (ulen v = u; v < n; ++v)
      if (comp_of[v] == ulen(-1) && reach[u][v] && reach[v][u]) {
        comp_of[v] = classes.size() - 1;
        classes.back().push_back(v);
      }
  }
  auto g = gns::make_basic_forest<V>(succs);
  auto got = gns::calculate_minimum_equivalent_arc_set<V>(g);
  std::printf("sccs=%zu classes:", classes.size());
  for (auto &c : classes) {
    std::printf(" {");
    for (ulen v : c)
      std::printf("%zu,", size_t(v));
    std::printf("}");
  }
  std::printf("\nkeep (%zu):", size_t(got.keep_edges.size()));
  for (ulen e : got.keep_edges)
    std::printf(" a%zu(%zu->%zu)", size_t(e), size_t(arcs[e].f),
                size_t(arcs[e].t));
  std::printf("\napprox_scc_arcs=%zu\n", size_t(got.approx_scc_arcs));
  // reachability preserved?
  std::vector<std::vector<ulen>> kept(n);
  for (ulen e : got.keep_edges)
    kept[arcs[e].f].push_back(arcs[e].t);
  for (ulen u = 0; u < n; ++u) {
    std::vector<char> vis(n, 0);
    std::vector<ulen> wl{u};
    vis[u] = 1;
    while (!wl.empty()) {
      ulen x = wl.back();
      wl.pop_back();
      for (ulen v : kept[x])
        if (!vis[v]) {
          vis[v] = 1;
          wl.push_back(v);
        }
    }
    for (ulen v = 0; v < n; ++v) {
      bool want = u != v && reach[u][v];
      if (want && (!vis[v] || v == u))
        std::printf("  LOST %zu->%zu (had %d kept %d)\n", size_t(u), size_t(v),
                    int(reach[u][v]), int(vis[v]));
      if (!want && v != u && vis[v] && vis[v] != reach[u][v])
        std::printf("  EXTRA %zu->%zu\n", size_t(u), size_t(v));
    }
  }
  return 0;
}
