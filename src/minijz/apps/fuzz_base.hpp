#pragma once
// === fuzz_base.hpp ==========================================
// the component fuzzer's shared plumbing: the umbrella includes, the
// component-name table, the per-run stats, the failure recorder with
// its repro line, and the RNG trio. Both body files -- fuzz_kernels.hpp
// (the CFG/SSA/opt kernels) and fuzz_graphs.hpp (the graph library's
// algorithm layer) -- include this first and are each self-contained;
// the driver (run_kernel_fuzz) lives in fuzz_kernels.hpp, which pulls
// the graph bodies in below its own. #pragma once keeps the triangle
// acyclic: whoever is compiled first defines the plumbing exactly once.
#include "../config.hpp"
#include "../graph_lib.hpp"
#include "../ir/ssa_generic.hpp"
#include "../opt/opt_generic.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// plumbing: the component table, the per-seed checker, RNG helpers
// ---------------------------------------------------------------------------

static const char *const KZ_NAMES[] = {
    "gns.forests", "gns.dom", "gns.df", "gns.phis", "ssa.wrapper", "ssa.rename",
    "ssa.reflow", "ssa.chain", "opt.mark", "opt.vn", "opt.sccp", "opt.pcopy",
    "opt.split", "opt.reach", "opt.splice", "opt.inv", "opt.ren", "opt.trivphi",
    "opt.shape", "opt.exits", "opt.route", "opt.needs", "opt.alias", "opt.mem",
    "opt.iv", "opt.ai", "opt.key", "opt.leak", "opt.ipa", "opt.equi",
    "opt.liveness", "opt.trace", "opt.escape",
    // the graph library's algorithm layer (fuzz_graphs.hpp)
    "gns.invfam", "gns.scc", "gns.seq", "gns.dsu", "gns.search", "gns.dij",
    "gns.flow", "gns.stcut", "gns.mcf", "gns.gcut", "gns.dgcut", "gns.maxcut",
    "gns.gh", "gns.mst", "gns.match", "gns.blossom", "gns.bic", "gns.kcore",
    "gns.euler", "gns.tr", "gns.rmas", "gns.dist", "gns.meg", "gns.closure",
    "gns.pcover", "gns.rmasp"};
static constexpr int KZ_NCOMP = int(sizeof(KZ_NAMES) / sizeof(KZ_NAMES[0]));

struct KzStats {
  std::vector<long> checks{}, fails{}; // per component
  KzStats() : checks(KZ_NCOMP, 0), fails(KZ_NCOMP, 0) {}
  void add(const KzStats &o) {
    for (int i = 0; i < KZ_NCOMP; ++i) {
      checks[i] += o.checks[i];
      fails[i] += o.fails[i];
    }
  }
  long total_fails() const {
    long t = 0;
    for (long f : fails)
      t += f;
    return t;
  }
};

// one component's run on one seed: counts checks, prints failures (under
// the driver's mutex when parallel) with a full repro line
struct KzRec {
  int comp{};
  long seed{};
  KzStats *st{};
  std::mutex *mu{};
  bool ok(bool cond, const char *what) {
    ++st->checks[comp];
    if (!cond) {
      ++st->fails[comp];
      if (mu) {
        std::lock_guard<std::mutex> lk(*mu);
        std::printf("KERNEL FAIL %s seed %ld: %s (repro: --fuzz-kernels=1 "
                    "--fuzz-kernels-from=%ld --fuzz-kernel=%s)\n",
                    KZ_NAMES[comp], seed, what, seed, KZ_NAMES[comp]);
        std::fflush(stdout);
      } else {
        std::printf("KERNEL FAIL %s seed %ld: %s (repro: --fuzz-kernels=1 "
                    "--fuzz-kernels-from=%ld --fuzz-kernel=%s)\n",
                    KZ_NAMES[comp], seed, what, seed, KZ_NAMES[comp]);
        std::fflush(stdout);
      }
    }
    return cond;
  }
};

static inline ulen kz_pick(std::mt19937_64 &rng, ulen n) {
  return n ? ulen(rng() % n) : 0;
}
static inline bool kz_chance(std::mt19937_64 &rng, int pct) {
  return int(rng() % 100) < pct;
}
static inline ulen kz_none() { return ulen(-1); }
