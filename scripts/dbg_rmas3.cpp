// debug: rmas seed 1630 -- traced copy of the search
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
static std::vector<std::vector<ulen>> kzg_reducible(std::mt19937_64 &rng,
                                                    ulen n_target) {
  std::vector<std::vector<ulen>> rows(n_target);
  ulen nodes = 1;
  int selfs = int(kz_pick(rng, 4));
  while (nodes < n_target || selfs > 0) {
    if (nodes < n_target && (selfs == 0 || kz_chance(rng, 55))) {
      const ulen v = kz_pick(rng, nodes);
      const ulen u = nodes++;
      std::vector<ulen> keep{};
      for (ulen x : rows[v]) {
        if (kz_chance(rng, 50))
          rows[u].push_back(x);
        else
          keep.push_back(x);
      }
      rows[v] = std::move(keep);
      rows[v].push_back(u);
    } else {
      --selfs;
      const ulen v = kz_pick(rng, nodes);
      rows[v].push_back(v);
    }
  }
  for (int t = int(kz_pick(rng, 4)); t-- > 0;) {
    const ulen x = kz_pick(rng, n_target), y = kz_pick(rng, n_target);
    if (x == y)
      continue;
    std::vector<char> seen(n_target, 0);
    std::vector<ulen> wl{0};
    seen[0] = 1;
    while (!wl.empty()) {
      const ulen u = wl.back();
      wl.pop_back();
      for (ulen v : rows[u])
        if (v != y && !seen[v]) {
          seen[v] = 1;
          wl.push_back(v);
        }
    }
    if (!seen[x])
      rows[x].push_back(y);
  }
  return rows;
}
int main(int argc, char **argv) {
  const long seed = argc > 1 ? atol(argv[1]) : 1630;
  std::mt19937_64 rng(uint64_t(seed) * 0x9E3779B97F4A7C15ull +
                      uint64_t(51) * 0xBF58476D1CE4E5B9ull +
                      0xD1B54A32D192ED03ull);
  const ulen n = 1 + kz_pick(rng, 7);
  const bool built = kz_chance(rng, 65); // the call advances the stream
  (void)built;
  std::vector<std::vector<ulen>> succs = kzg_reducible(rng, n);
  ulen root = 0;
  struct KA {
    ulen from{}, to{};
  };
  std::vector<KA> arcs{};
  for (ulen u = 0; u < succs.size(); ++u)
    for (ulen v : succs[u])
      arcs.push_back({u, v});
  const ulen m = arcs.size();
  std::vector<ulen> w(m);
  for (auto &x : w)
    x = kz_pick(rng, 13);
  for (ulen e = 0; e < m; ++e)
    std::printf("arc %zu: %zu -> %zu w=%zu\n", size_t(e), size_t(arcs[e].from),
                size_t(arcs[e].to), size_t(w[e]));
  auto g = gns::make_basic_forest<V>(succs);
  auto rows = g.range();
  auto edge_ids = g.index_range();
  std::vector<ulen> arc_from(m, 0), arc_to(m, 0);
  {
    ulen k = 0;
    for (ulen u = 0; u < n; ++u)
      for (ulen v : rows[u]) {
        arc_from[k] = u;
        arc_to[k] = v;
        ++k;
      }
  }
  // the DFS
  std::vector<ulen> disc(n, 0);
  std::vector<char> state(n, 0), is_back(m, 0);
  std::vector<ulen> back_arcs{};
  {
    struct frame_t {
      ulen node, k;
    };
    std::vector<frame_t> cs{{root, 0}};
    state[root] = 1;
    ulen timer = 1;
    while (!cs.empty()) {
      frame_t &fr = cs.back();
      const ulen u = fr.node;
      if (fr.k < ulen(rows[u].size())) {
        const ulen k = fr.k++;
        const ulen e = ulen(edge_ids[u][intlen_t(k)]);
        const ulen v = ulen(rows[u][intlen_t(k)]);
        if (state[v] == 0) {
          disc[v] = timer++;
          state[v] = 1;
          cs.push_back({v, 0});
        } else if (state[v] == 1) {
          is_back[e] = 1;
          back_arcs.push_back(e);
        }
        continue;
      }
      state[u] = 2;
      cs.pop_back();
    }
  }
  std::printf("backs:");
  for (ulen e : back_arcs)
    std::printf(" %zu", size_t(e));
  std::printf("\n");
  std::vector<char> cut_arc(m, 0), del_back(m, 0);
  std::vector<ulen> path_{};
  std::vector<uint32_t> seen(n, 0);
  uint32_t epoch = 0;
  std::vector<ulen> parent(n, ulen(-1));
  auto live_path = [&](ulen e) noexcept -> bool {
    path_.clear();
    const ulen h = arc_to[e], u = arc_from[e];
    if (h == u)
      return true;
    ++epoch;
    std::vector<ulen> stack{h};
    seen[h] = epoch;
    while (!stack.empty()) {
      const ulen x = stack.back();
      stack.pop_back();
      for (ulen e2 : edge_ids[x]) {
        if (is_back[e2] || cut_arc[e2])
          continue;
        const ulen y = arc_to[e2];
        if (seen[y] == epoch)
          continue;
        seen[y] = epoch;
        parent[y] = e2;
        if (y == u) {
          for (ulen z = u; z != h; z = arc_from[parent[z]])
            path_.push_back(parent[z]);
          return true;
        }
        stack.push_back(y);
      }
    }
    return false;
  };
  // witness one
  ulen best_cost = 0;
  std::vector<ulen> best_cut{}, best_del{};
  {
    const ulen S = n, T = n + 1;
    std::vector<std::vector<ulen>> net(n + 2), net_edge_of(n + 2);
    for (ulen u = 0; u < n; ++u)
      for (ulen e : edge_ids[u]) {
        if (is_back[e]) {
          net[S].push_back(arc_to[e]);
          net_edge_of[S].push_back(e);
          net[u].push_back(T);
          net_edge_of[u].push_back(e);
        } else {
          net[u].push_back(arc_to[e]);
          net_edge_of[u].push_back(e);
        }
      }
    std::vector<ulen> net_caps{};
    for (ulen x = 0; x < n + 2; ++x)
      for (ulen e : net_edge_of[x])
        net_caps.push_back(w[e]);
    auto flow = gns::calculate_maximum_flow<V>(
        S, T, gns::make_basic_forest<V>(net), net_caps);
    auto &side = flow.cut_side;
    for (ulen u = 0; u < n; ++u)
      for (ulen e : edge_ids[u]) {
        const ulen v = arc_to[e];
        const bool cut_it =
            is_back[e] ? (!side[v] || side[u]) : (side[u] && !side[v]);
        if (!cut_it)
          continue;
        if (is_back[e])
          best_del.push_back(e);
        else
          best_cut.push_back(e);
        best_cost += w[e];
      }
    std::printf("witness1 cost=%zu del:", size_t(best_cost));
    for (ulen e : best_del)
      std::printf(" %zu", size_t(e));
    std::printf(" cut:");
    for (ulen e : best_cut)
      std::printf(" %zu", size_t(e));
    std::printf("\n");
  }
  // witness two
  {
    std::vector<char> cut2(m, 0), del2(m, 0);
    ulen cost2 = 0;
    std::vector<ulen> order = back_arcs;
    std::sort(order.begin(), order.end(), [&](ulen a, ulen b) {
      return disc[arc_from[a]] > disc[arc_from[b]];
    });
    for (ulen e : order) {
      const ulen h = arc_to[e], u = arc_from[e];
      if (h != u) {
        std::vector<std::vector<ulen>> rows2(n);
        std::vector<ulen> caps2{}, ref{};
        for (ulen x = 0; x < n; ++x)
          for (ulen e2 : edge_ids[x])
            if (!is_back[e2] && !cut2[e2]) {
              rows2[x].push_back(arc_to[e2]);
              ref.push_back(e2);
              caps2.push_back(w[e2]);
            }
        auto fl = gns::calculate_maximum_flow<V>(
            h, u, gns::make_basic_forest<V>(rows2), caps2);
        std::printf("w2: e=%zu flow=%zu w=%zu\n", size_t(e), size_t(fl.flow),
                    size_t(w[e]));
        if (fl.flow < w[e]) {
          for (ulen i = 0; i < ref.size(); ++i) {
            const ulen e2 = ref[i];
            if (fl.cut_side[arc_from[e2]] && !fl.cut_side[arc_to[e2]]) {
              cut2[e2] = 1;
              cost2 += w[e2];
            }
          }
          continue;
        }
      }
      del2[e] = 1;
      cost2 += w[e];
    }
    std::printf("witness2 cost=%zu\n", size_t(cost2));
    if (cost2 < best_cost) {
      best_cost = cost2;
      best_cut.clear();
      best_del.clear();
      for (ulen e = 0; e < m; ++e) {
        if (cut2[e])
          best_cut.push_back(e);
        if (del2[e])
          best_del.push_back(e);
      }
    }
  }
  // the search, traced
  {
    ulen budget = ulen(1) << 18;
    bool exhausted = false;
    std::vector<ulen> branch_path{};
    int depth = 0;
    auto rec = [&](auto &&self, ulen cost) noexcept -> void {
      if (exhausted)
        return;
      if (budget == 0) {
        exhausted = 1;
        return;
      }
      --budget;
      ulen target = ulen(-1);
      ulen lb = 0;
      for (ulen e : back_arcs) {
        if (del_back[e])
          continue;
        if (!live_path(e))
          continue;
        ulen le = w[e];
        for (ulen f : path_)
          if (w[f] < le)
            le = w[f];
        if (le > lb)
          lb = le;
        if (target == ulen(-1)) {
          target = e;
          branch_path = path_;
        }
      }
      for (int i = 0; i < depth; ++i)
        std::printf("  ");
      std::printf("node cost=%zu lb=%zu best=%zu target=%zu\n", size_t(cost),
                  size_t(lb), size_t(best_cost),
                  target == ulen(-1) ? size_t(9999) : size_t(target));
      if (cost + lb >= best_cost)
        return;
      if (target == ulen(-1)) {
        best_cost = cost;
        best_cut.clear();
        best_del.clear();
        for (ulen e = 0; e < m; ++e) {
          if (cut_arc[e])
            best_cut.push_back(e);
          if (del_back[e])
            best_del.push_back(e);
        }
        std::printf("  SOLUTION %zu\n", size_t(cost));
        return;
      }
      del_back[target] = 1;
      ++depth;
      self(self, cost + w[target]);
      --depth;
      del_back[target] = 0;
      std::sort(branch_path.begin(), branch_path.end(), [&](ulen a, ulen b) {
        return w[a] != w[b] ? w[a] < w[b] : a < b;
      });
      for (ulen f : branch_path) {
        cut_arc[f] = 1;
        ++depth;
        self(self, cost + w[f]);
        --depth;
        cut_arc[f] = 0;
      }
    };
    rec(rec, 0);
    std::printf("search best=%zu exhausted=%d\n", size_t(best_cost),
                int(exhausted));
    std::printf("final del:");
    for (ulen e : best_del)
      std::printf(" %zu", size_t(e));
    std::printf(" cut:");
    for (ulen e : best_cut)
      std::printf(" %zu", size_t(e));
    std::printf("\n");
  }
  return 0;
}
