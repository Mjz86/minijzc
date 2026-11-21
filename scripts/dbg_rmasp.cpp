// debug: rmasp differential failures -- regenerate the fuzzer's exact
// graph for a seed, print it, run both algorithms, diff the weights
#define MJZ_WITH_iostream false
#define MJZ_TEST_MODE_ 1
#define MJZ_LOG_NEW_ALLOCATIONS_ 0
#define MJZ_aggregate_tuple_workaround false
#include "src/minijz/graph_lib.hpp"
#include <cstdio>
#include <random>
using namespace mjz;
namespace gns = mjz::graph_ns;
using ulen = uintlen_t;

static inline ulen kz_none() { return ulen(-1); }
static inline ulen kz_pick(std::mt19937_64 &r, ulen n) {
  return n ? ulen(r() % n) : 0;
}
static inline bool kz_chance(std::mt19937_64 &r, int pct) {
  return int(r() % 100) < pct;
}
struct KzgArc2 {
  ulen from{}, to{};
};
static std::vector<KzgArc2>
kzg_arcs2(const std::vector<std::vector<ulen>> &rows) {
  std::vector<KzgArc2> arcs{};
  for (ulen u = 0; u < rows.size(); ++u)
    for (ulen v : rows[u])
      arcs.push_back({u, v});
  return arcs;
}

// ---- verbatim copy of the fuzzer's generator ----
static std::pair<std::vector<std::vector<ulen>>, bool>
kzg_reducible(std::mt19937_64 &rng, ulen n, bool inject_cross) {
  std::vector<ulen> par(n, kz_none());
  for (ulen i = 1; i < n; ++i)
    par[i] = kz_pick(rng, i);
  std::vector<char> marked(n, 0);
  for (ulen i = 0; i < n; ++i)
    marked[i] = kz_chance(rng, 35) ? 1 : 0;
  std::vector<std::vector<ulen>> rows(n);
  for (ulen i = 1; i < n; ++i)
    rows[par[i]].push_back(i);
  const auto rand_strict_ancestor = [&](ulen v) noexcept {
    if (par[v] == kz_none() || !kz_chance(rng, 60))
      return kz_none();
    ulen u = par[v];
    while (par[u] != kz_none() && kz_chance(rng, 60))
      u = par[u];
    return u;
  };
  const ulen bck = kz_pick(rng, n + 1);
  for (ulen k = 0; k < bck && n > 1; ++k) {
    const ulen v = 1 + kz_pick(rng, n - 1);
    const ulen h = rand_strict_ancestor(v);
    if (h != kz_none() && marked[h])
      rows[v].push_back(h);
  }
  const ulen fwd = kz_pick(rng, n + 1);
  for (ulen k = 0; k < fwd && n > 1; ++k) {
    const ulen v = 1 + kz_pick(rng, n - 1);
    const ulen a = rand_strict_ancestor(v);
    if (a == kz_none())
      continue;
    bool blocked = false;
    for (ulen z = par[v]; z != a && z != kz_none(); z = par[z])
      if (marked[z])
        blocked = true;
    if (!blocked)
      rows[a].push_back(v);
  }
  if (kz_chance(rng, 20) && n) {
    for (ulen k = 0, c = 1 + kz_pick(rng, 2); k < c; ++k) {
      const ulen u = kz_pick(rng, n);
      rows[u].push_back(u);
    }
  }
  bool witnessed = false;
  if (inject_cross && n > 2) {
    for (ulen tries = 0; tries < 12 && !witnessed; ++tries) {
      const ulen a = 1 + kz_pick(rng, n - 1);
      const ulen b = 1 + kz_pick(rng, n - 1);
      if (a == b)
        continue;
      auto is_anc = [&](ulen x, ulen y) noexcept {
        for (ulen z = y; z != kz_none(); z = par[z])
          if (z == x)
            return true;
        return false;
      };
      if (is_anc(a, b) || is_anc(b, a))
        continue;
      rows[a].push_back(b);
      rows[b].push_back(a);
      witnessed = true;
    }
  }
  return {std::move(rows), witnessed};
}

#include <exception>
#include <execinfo.h>
static void bt_handler() {
  void *trace[24];
  int n = backtrace(trace, 24);
  fprintf(stderr, "terminate! backtrace (%d frames):\n", n);
  backtrace_symbols_fd(trace, n, 2);
  std::abort();
}
int main(int argc, char **argv) {
  std::set_terminate(bt_handler);
  const long seed = argc > 1 ? atol(argv[1]) : 2601;
  const int comp = 55; // gns.rmasp's index in KZ_NAMES
  std::mt19937_64 rng(uint64_t(seed) * 0x9E3779B97F4A7C15ull +
                      uint64_t(comp) * 0xBF58476D1CE4E5B9ull +
                      0xD1B54A32D192ED03ull);
  // ---- verbatim copy of fz_rmasp's input generation ----
  const ulen n = 1 + kz_pick(rng, 7);
  auto [succs, cross] = kzg_reducible(rng, n, kz_chance(rng, 25));
  auto arcs = kzg_arcs2(succs);
  const ulen m = arcs.size();
  std::vector<ulen> w(m);
  for (auto &x : w)
    x = kz_pick(rng, 13);
  printf("seed %ld: n=%zu m=%zu cross=%d\n", seed, succs.size(), m, (int)cross);
  for (ulen e = 0; e < m; ++e)
    printf("  arc %zu: %zu -> %zu  w=%zu\n", e, arcs[e].from, arcs[e].to, w[e]);
  auto g = gns::make_basic_forest<V>(succs);
  auto got = gns::calculate_minimum_feedback_arc_set_reducible<V>(0, g, w);
  auto exact = gns::calculate_minimum_feedback_arc_set<V>(g, w);
  printf("poly=%zu arcs:", ulen(got.total_weight));
  for (ulen e : got.removed_arcs)
    printf(" %zu", e);
  printf("\nexact=%zu arcs:", ulen(exact.total_weight));
  for (ulen e : exact.removed_arcs)
    printf(" %zu", e);
  printf("\n");
  printf("MATCH: %s\n", got.total_weight == exact.total_weight ? "yes" : "NO");
  return 0;
}
