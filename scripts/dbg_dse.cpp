// debug: replay fz_dse's generation for one seed and dump the
// kernel vs the spec verdicts, store by store.
#include "src/minijz.hpp"
#include "src/minijz/apps/fuzz_base.hpp"
#include <algorithm>
#include <cstdio>

using namespace mjz;

static void dump_one(long seed, int comp) {
  std::mt19937_64 rng(uint64_t(seed) * 0x9E3779B97F4A7C15ull +
                      uint64_t(comp) * 0xBF58476D1CE4E5B9ull +
                      0xD1B54A32D192ED03ull);
  using opt_ns::dse_break_v;
  using opt_ns::dse_event_t;
  using opt_ns::dse_load_v;
  using opt_ns::dse_store_v;
  const ulen nblocks = 1 + kz_pick(rng, 4);
  std::vector<std::vector<dse_event_t>> evs(nblocks);
  std::vector<ulen> store_at{};
  ulen ord = 0;
  for (ulen b = 0; b < nblocks; ++b) {
    const ulen n = kz_pick(rng, 13);
    for (ulen j = 0; j < n; ++j) {
      dse_event_t e{};
      const int roll = int(kz_pick(rng, 100));
      if (roll < 50) {
        e.kind = dse_store_v;
        e.group = kz_pick(rng, 2);
        e.key = kz_pick(rng, 3);
        e.val = kz_pick(rng, 3);
        store_at.push_back(ord);
      } else if (roll < 75) {
        e.kind = dse_load_v;
        e.group = kz_pick(rng, 2);
        e.key = kz_pick(rng, 3);
      } else {
        e.kind = dse_break_v;
      }
      evs[b].push_back(e);
      ++ord;
    }
  }
  const auto dead = opt_ns::calculate_dead_stores<V>(evs);
  // the spec reference (same as the fuzzer)
  std::vector<char> want_same(ord, 0), want_over(ord, 0);
  {
    ulen o = 0;
    for (const auto &row : evs)
      for (ulen j = 0; j < row.size(); ++j, ++o) {
        if (row[j].kind != dse_store_v)
          continue;
        bool have_p = false;
        opt_ns::dse_event_t p{};
        for (ulen k = j; k-- > 0;) {
          if (row[k].kind == dse_break_v)
            break;
          if (row[k].kind == dse_store_v && row[k].group == row[j].group &&
              row[k].key == row[j].key) {
            have_p = true;
            p = row[k];
            break;
          }
        }
        if (have_p && p.val == row[j].val) {
          want_same[o] = 1;
          continue;
        }
        for (ulen k = j + 1; k < row.size(); ++k) {
          if (row[k].kind == dse_break_v)
            break;
          if (row[k].kind == dse_load_v && row[k].group == row[j].group &&
              row[k].key == row[j].key)
            break;
          if (row[k].kind == dse_store_v && row[k].group == row[j].group &&
              row[k].key == row[j].key) {
            if (row[k].val != row[j].val)
              want_over[o] = 1;
            break;
          }
        }
      }
  }
  std::vector<char> got(ord, 0);
  std::vector<uintlen_t> rule(ord, 99);
  for (const auto &d : dead) {
    got[d.site] = 1;
    rule[d.site] = d.rule;
  }
  // dump
  ulen o = 0;
  for (ulen b = 0; b < evs.size(); ++b) {
    std::printf("row %llu:\n", (unsigned long long)b);
    for (const auto &e : evs[b]) {
      const char *kn = e.kind == dse_store_v
                           ? "store"
                           : (e.kind == dse_load_v ? "load" : "BREAK");
      std::printf("  [%3llu] %-5s g=%llu k=%llu v=%llu", (unsigned long long)o,
                  kn, (unsigned long long)e.group, (unsigned long long)e.key,
                  (unsigned long long)e.val);
      if (e.kind == dse_store_v)
        std::printf("   kernel=%s spec=%s",
                    got[o]
                        ? (rule[o] == opt_ns::dse_overwrite_v ? "OVERWRITE-dead"
                                                              : "SAME-dead")
                        : "alive",
                    want_over[o] ? "OVERWRITE-dead"
                                 : (want_same[o] ? "SAME-dead" : "alive"));
      std::printf("\n");
      ++o;
    }
  }
}

int main() {
  // opt.dse is component index 30 in KZ_NAMES (0-based)
  dump_one(1, 33);
  return 0;
}
