// debug: replicate fz_ai for one seed with round tracing
#include "../src/minijz/apps/fuzz_kernels.hpp"
#include <cstdio>

using opt_ns::iv_ns::iv_t;

struct CB {
  int64_t c;
  const std::vector<int64_t> *step, *ja, *jb, *rl, *rh;
  const std::vector<std::vector<char>> *marked;
  void entry_top(std::vector<iv_t> &st) const {
    st[0] = {c, c, false};
    st[1] = {0, 0, false};
  }
  void refine_edge(uintlen_t p, uintlen_t b, std::vector<iv_t> &st) const {
    if (!(*marked)[p][b] || st[0].bot)
      return;
    const int64_t lo = (*rl)[p] > st[0].lo ? (*rl)[p] : st[0].lo;
    const int64_t hi = (*rh)[p] < st[0].hi ? (*rh)[p] : st[0].hi;
    st[0] = lo <= hi ? iv_t{lo, hi, false} : iv_t{0, 0, true};
  }
  void phi_join(uintlen_t, std::vector<iv_t> &, const iv_t *, uintlen_t) const {
  }
  void transfer(uintlen_t b, std::vector<iv_t> &st) const {
    if (!st[0].bot) {
      st[0].hi = st[0].hi == INT64_MAX ? INT64_MAX : st[0].hi + (*step)[b];
      if (st[0].hi < st[0].lo)
        st[0].hi = INT64_MAX;
    }
    st[1] = opt_ns::iv_ns::iv_join(st[1], iv_t{(*ja)[b], (*jb)[b], false});
  }
};

// traced copy of solve_forward
static bool trace_solve(uintlen_t nb, uintlen_t nv,
                        const std::vector<std::vector<ulen>> &preds,
                        const std::vector<char> &widen_at,
                        uintlen_t entry_block, uintlen_t plain_cap,
                        uintlen_t widen_cap, uintlen_t narrow_rounds,
                        const CB &cb) {
  std::vector<iv_t> entry((size_t)nb * nv, iv_t{0, 0, true});
  std::vector<iv_t> exit_((size_t)nb * nv, iv_t{0, 0, true});
  std::vector<char> touched(nb, 0);
  std::vector<iv_t> st(nv), edge(nv);
  bool widening = false;
  uintlen_t round = 0;
  for (;;) {
    bool changed = false;
    for (uintlen_t b = 0; b < nb; ++b) {
      for (uintlen_t v = 0; v < nv; ++v)
        st[v] = {0, 0, true};
      bool any_pred = false;
      for (const auto &pp : preds[b]) {
        const uintlen_t p = uintlen_t(pp);
        if (p >= nb || !touched[p])
          continue;
        any_pred = true;
        for (uintlen_t v = 0; v < nv; ++v)
          edge[v] = exit_[(size_t)p * nv + v];
        cb.refine_edge(p, b, edge);
        for (uintlen_t v = 0; v < nv; ++v)
          st[v] = opt_ns::iv_ns::iv_join(st[v], edge[v]);
      }
      if (!any_pred && b != entry_block)
        for (uintlen_t v = 0; v < nv; ++v)
          st[v] = {0, 0, true};
      if (b == entry_block)
        cb.entry_top(st);
      {
        bool diff = false;
        for (uintlen_t v = 0; v < nv; ++v)
          if (!opt_ns::iv_ns::iv_equal(st[v], entry[(size_t)b * nv + v])) {
            diff = true;
            break;
          }
        if (diff) {
          if (widening && b < widen_at.size() && widen_at[b]) {
            for (uintlen_t v = 0; v < nv; ++v) {
              iv_t &old = entry[(size_t)b * nv + v];
              if (st[v].bot)
                continue;
              if (st[v].lo < old.lo)
                st[v].lo = INT64_MIN;
              if (st[v].hi > old.hi)
                st[v].hi = INT64_MAX;
              if (st[v].lo > st[v].hi)
                st[v] = old;
            }
          }
          for (uintlen_t v = 0; v < nv; ++v)
            entry[(size_t)b * nv + v] = st[v];
          changed = true;
          std::printf("  r%-3u b%-2u v0=[%lld,%lld]%s v1=[%lld,%lld]%s%s\n",
                      round, b, (long long)st[0].lo, (long long)st[0].hi,
                      st[0].bot ? "B" : "", (long long)st[1].lo,
                      (long long)st[1].hi, st[1].bot ? "B" : "",
                      (widening && widen_at[b]) ? " W" : "");
        }
      }
      for (uintlen_t v = 0; v < nv; ++v)
        st[v] = entry[(size_t)b * nv + v];
      cb.transfer(b, st);
      for (uintlen_t v = 0; v < nv; ++v)
        exit_[(size_t)b * nv + v] = st[v];
      touched[b] = 1;
    }
    if (!changed)
      return true;
    ++round;
    if (!widening && round >= plain_cap)
      widening = true;
    if (widening && round >= plain_cap + widen_cap) {
      std::printf("  GAVE UP at round %u\n", round);
      return false;
    }
  }
}

int main(int argc, char **argv) {
  long seed = argc > 1 ? atol(argv[1]) : 407;
  std::mt19937_64 rng(uint64_t(seed) * 0x9E3779B97F4A7C15ull +
                      uint64_t(25) *
                          0xBF58476D1CE4E5B9ull + // comp idx of opt.ai
                      0xD1B54A32D192ED03ull);
  const ulen nb = 2 + kz_pick(rng, 5);
  std::vector<std::vector<ulen>> succs(nb);
  for (ulen b = 0; b < nb; ++b) {
    const int deg = int(kz_pick(rng, 3));
    for (int j = 0; j < deg; ++j)
      succs[b].push_back(1 + kz_pick(rng, nb - 1));
  }
  std::vector<std::vector<ulen>> preds(nb);
  for (ulen b = 0; b < nb; ++b)
    for (ulen s : succs[b])
      preds[s].push_back(b);
  const int64_t c = int64_t(rng() % 101) - 50;
  std::vector<int64_t> step(nb), ja(nb), jb(nb);
  std::vector<char> has_mark(nb, 0);
  std::vector<int64_t> rl(nb), rh(nb);
  std::vector<std::vector<char>> marked(nb, std::vector<char>(nb, 0));
  for (ulen b = 0; b < nb; ++b) {
    step[b] = int64_t(kz_pick(rng, 4));
    int64_t a = int64_t(rng() % 201) - 100;
    int64_t z = a + int64_t(kz_pick(rng, 20));
    ja[b] = a;
    jb[b] = z;
  }
  for (ulen p = 0; p < nb; ++p)
    for (ulen s : succs[p])
      if (kz_chance(rng, 25)) {
        marked[p][s] = 1;
        const int64_t mid = int64_t(rng() % 401) - 200;
        rl[p] = mid - int64_t(rng() % 50);
        rh[p] = mid + int64_t(rng() % 50);
        if (rl[p] > rh[p])
          std::swap(rl[p], rh[p]);
      }
  const ulen plain_cap = 4 + kz_pick(rng, 5);
  const ulen widen_cap = 20 + kz_pick(rng, 20);
  const ulen narrow = 1 + kz_pick(rng, 3);
  const CB cb{c, &step, &ja, &jb, &rl, &rh, &marked};
  std::printf("seed %ld: nb=%u c=%lld plain=%u wcap=%u\n", seed, nb,
              (long long)c, plain_cap, widen_cap);
  for (ulen b = 0; b < nb; ++b) {
    std::printf("b%u: step=%lld ja=%lld jb=%lld succs=[", b, (long long)step[b],
                (long long)ja[b], (long long)jb[b]);
    for (auto s : succs[b])
      std::printf("%u%s", s, marked[b][s] ? "m" : "");
    std::printf("] rl=%lld rh=%lld preds=[", (long long)rl[b],
                (long long)rh[b]);
    for (auto q : preds[b])
      std::printf("%u ", q);
    std::printf("]\n");
  }
  std::vector<char> widen_all(nb, 1);
  std::printf("--- widen-everywhere, NO-REFINE trace:\n");
  auto marked0 = marked;
  for (auto &row : marked0)
    std::fill(row.begin(), row.end(), 0);
  const CB cb0{c, &step, &ja, &jb, &rl, &rh, &marked0};
  bool conv = trace_solve(nb, 2, preds, widen_all, 0, plain_cap, widen_cap,
                          narrow, cb0);
  std::printf("converged: %d\n", int(conv));
  return 0;
}
