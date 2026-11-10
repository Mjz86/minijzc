#pragma once
// === fuzz_run.hpp ========================================
// the differential fuzz driver (FuzzStats + run_fuzz), now with
// random pass configs, failure auto-save, live progress reporting,
// a stall watchdog, and multi-worker parallel fuzzing (--fuzz-jobs).
//
// Determinism: every seed is compiled and checked independently, so
// worker scheduling cannot change the result set; only the ORDER in
// which mismatch reports reach stdout can interleave (each report is
// printed atomically under the fuzz mutex).

#include "../base/pass_cfg.hpp"
#include "../prog/program.hpp"
#include "../prog/runners.hpp"
#include "../runtime/astrun.hpp"
#include "../back/bytecode.hpp"
#include "../base/native_run.hpp"
#include "fuzz_gen.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

// ============================================================================
// 15. differential fuzzer: random programs, five-way agreement
//     (oracle vs SSA vs optimized vs destroyed vs bytecode VM).
//     Vocabulary: lets (scalar / flat array / 2-D array), runtime-index
//     loads & stores (counter-mod and normalized double-mod), len() of
//     arrays and scalars, input() from a fixed per-seed tape, fn calls
//     (constant, expression, and array-handle arguments), division and
//     modulo by positive AND negative constants, short-circuit && / ||
//     division guards (guarded and deliberately inverted), unary !, all
//     six comparisons, if with and without else, up- and down-counting
//     whiles with compound conditions, guarded break/continue, self / mutual
//     / tail-recursive / 3-member-SCC functions, and array params that cross
//     frames. Whole programs opt in to the coverage modes that disable
//     program-wide gates (2-D arrays, array params, runtime indices) and to
//     deliberate trap bait (OOB accesses, /0, inverted guards), so the rest
//     keep full lifting / dead-array / const-call coverage.
// ============================================================================
struct FuzzStats {
  long cases{}, timeouts{}, traps{}, licm_hoists{}, dce_instrs{}, phis{};
  long arr_progs{}, in_progs{}, native_checked{};
  long fn_progs{}, inlined_total{}, unrolled_total{};
  long brk_progs{}, lifted{}, dead_stores{}, gensims{}, traps_killed{};
  long bkpt_progs{}, bkpts{}; // observation-point vocabulary (new seeds)
  long constcalls{}, deadallocs{}, tces{}, cdfolds{}, ifconvs{};
  long scctco_total{}, scctco_fold_total{};
  long sccp_consts{}, sccp_brs{}, ld_fwds{}, ld_cses{}, rangecmps{};
  long cmpfacts{}, callcses{};
  long deadargs{};
  long fnspecs{}, icf{};
  long jthreads{}, argpromos{}, gvns{}, unswitches{}, lsimps{};
  long div_progs{}, mat_progs{}, aparam_progs{};
  long gvn_progs{}, gvn_baits{}, unsw_progs{}, unsw_baits{};
  long cc_progs{}, cc_baits{};   // compound-cond unswitch bait (>= 13331)
  long bif_progs{}, bif_baits{}; // body-if unswitch bait (>= 13631)
};
static void fuzz_stats_add(FuzzStats &a, const FuzzStats &b) {
  a.cases += b.cases;
  a.timeouts += b.timeouts;
  a.traps += b.traps;
  a.licm_hoists += b.licm_hoists;
  a.dce_instrs += b.dce_instrs;
  a.phis += b.phis;
  a.arr_progs += b.arr_progs;
  a.in_progs += b.in_progs;
  a.native_checked += b.native_checked;
  a.fn_progs += b.fn_progs;
  a.inlined_total += b.inlined_total;
  a.unrolled_total += b.unrolled_total;
  a.brk_progs += b.brk_progs;
  a.bkpt_progs += b.bkpt_progs;
  a.bkpts += b.bkpts;
  a.lifted += b.lifted;
  a.dead_stores += b.dead_stores;
  a.gensims += b.gensims;
  a.traps_killed += b.traps_killed;
  a.constcalls += b.constcalls;
  a.deadallocs += b.deadallocs;
  a.tces += b.tces;
  a.cdfolds += b.cdfolds;
  a.ifconvs += b.ifconvs;
  a.scctco_total += b.scctco_total;
  a.scctco_fold_total += b.scctco_fold_total;
  a.sccp_consts += b.sccp_consts;
  a.sccp_brs += b.sccp_brs;
  a.ld_fwds += b.ld_fwds;
  a.ld_cses += b.ld_cses;
  a.rangecmps += b.rangecmps;
  a.cmpfacts += b.cmpfacts;
  a.callcses += b.callcses;
  a.deadargs += b.deadargs;
  a.fnspecs += b.fnspecs;
  a.icf += b.icf;
  a.jthreads += b.jthreads;
  a.gvns += b.gvns;
  a.unswitches += b.unswitches;
  a.lsimps += b.lsimps;
  a.argpromos += b.argpromos;
  a.div_progs += b.div_progs;
  a.mat_progs += b.mat_progs;
  a.aparam_progs += b.aparam_progs;
  a.gvn_progs += b.gvn_progs;
  a.gvn_baits += b.gvn_baits;
  a.unsw_progs += b.unsw_progs;
  a.unsw_baits += b.unsw_baits;
  a.cc_progs += b.cc_progs;
  a.cc_baits += b.cc_baits;
  a.bif_progs += b.bif_progs;
  a.bif_baits += b.bif_baits;
}

// how many workers --fuzz should use when the flag was not given:
// selftest output must stay byte-identical -> serial; a plain fuzz run
// parallelizes over the cores (capped at 4: the workers are memory-hungry
// compilers, and g++ boxes OOM under naive core-count parallelism)
long default_fuzz_jobs(bool selftest) {
  if (selftest)
    return 1;
  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0)
    hw = 1;
  return (long)(hw < 4 ? hw : 4);
}

// shared state of one run_fuzz() invocation
struct FuzzCtx {
  // immutable inputs
  std::string cc{};
  std::string ndir{};
  std::string out_dir{};
  long from{1}, n{0};
  bool random_configs{false}, dump{false};
  long jobs{1};
  // work queue + counters (atomics: workers steal seeds and report)
  std::atomic<long> next_seed{1};
  std::atomic<long> done{0};
  std::atomic<long> mismatches{0};
  std::atomic<long> native_left{0};
  // in-flight bookkeeping for the stall watchdog: worker id -> seed (0: idle)
  enum { MAX_WORKERS = 64 };
  std::atomic<long> inflight[MAX_WORKERS]{};
  // all stdout/stderr emission and g_checks/g_fails accounting
  std::mutex mu{};
  FuzzStats total{};
};

// one seed's pass configuration (rotation + optional random extra toggles)
static PassCfg fuzz_cfg_for(long seed, bool random_configs) {
  PassCfg cfg{};
  cfg.set_level(3);
  cfg.fp_limit = 8; // fuzz programs are pathological: cap fixpoint depth
  switch (seed % 17) {
  case 0:
    cfg.p_scctco = false;
    break; // no SCC tail-call collapse
  case 1:
    cfg.p_lcf = false;
    break; // no closed forms / simulators
  case 2:
    cfg.p_unroll = false;
    break; // no unrolling
  case 3:
    cfg.p_mem2reg = false;
    break; // no array promotion
  case 4:
    cfg.p_trapelim = false;
    break; // no range/alias trap proofs
  case 5:
    cfg.set_level(2);
    break; // mid pipeline
  case 6:
    cfg.set_level(1);
    break; // cheap tier only
  case 7:
    cfg.p_cse = false;
    cfg.p_licm = false;
    cfg.p_reassoc = false;
    cfg.p_divmagic = false;
    break; // scalar opts off
  case 8:
    cfg.p_tailrec = false;
    break; // no tail-call elimination
  case 9:
    cfg.p_cdfold = false;
    cfg.p_ifconv = false;
    break; // no countdown fold / if-conv
  case 10:
    cfg.p_sccp = false;
    cfg.p_ldelim = false;
    cfg.p_callcse = false;
    break; // no new-portable-opts slice (SCCP/ldelim/callcse)
  case 11:
    cfg.p_deadargs = false;
    break; // no interprocedural dead-argument elimination
  case 12:
    cfg.p_fnspec = false;
    break; // no function specialization (const-arg clones)
  case 13:
    cfg.p_icf = false;
    break; // no identical code folding
  case 14:
    cfg.p_argpromo = false;
    break; // no argument promotion (array params stay handles)
  case 15:
    cfg.p_jthread = false;
    break; // no jump threading (elbow branches stay)
  case 16:
    cfg.p_gvn = false;
    break; // no value numbering (duplicate consts/phis stay)
  }
  // seeds in the unswitch window additionally rotate the two new
  // passes off (seed % 19 in {17, 18}); every older seed keeps its
  // exact old config, so committed fixtures stay byte-identical
  if (seed >= (long)MJZ_UNSWITCH_SEED_FLOOR) {
    switch (seed % 19) {
    case 17:
      cfg.p_unswitch = false;
      break; // no loop unswitching
    case 18:
      cfg.p_lsimp = false;
      break; // no loop canonicalization
    }
  }
  // (ported from Branchedmain) --fuzz-cfg=random: additionally disable a
  // random 1..4 of the 31 toggles and pick a random tier, exposing pass
  // INTERACTION bugs the fixed rotation never reaches
  if (random_configs) {
    auto pick32 = [](uint32_t &rng) -> uint32_t {
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      return rng;
    };
    uint32_t rng = (uint32_t)seed * 2654435761u + 0x9e3779b9u;
    (void)pick32(rng);
    cfg.set_level(1u + pick32(rng) % 3u);
    if (cfg.level >= 2)
      cfg.fp_limit = 8;
    uint32_t ndis = 1u + pick32(rng) % 4u;
    for (uint32_t d = 0; d < ndis; ++d)
      (void)apply_pass_flag(cfg, pick32(rng) % PASS_TOGGLE_COUNT, false);
  }
  return cfg;
}

// compile + run + compare ONE seed; all reporting goes through ctx.mu.
// returns false only on a hard mismatch (counted in ctx.mismatches).
static bool fuzz_one(long seed, FuzzCtx &ctx, FuzzStats &F) {
  const auto t_seed0 = std::chrono::steady_clock::now();
  GenCtx g = gen_program((uint32_t)seed);
  const std::string &src = g.src;
  if (ctx.dump && seed >= ctx.from + ctx.n - 2) {
    std::lock_guard<std::mutex> lk(ctx.mu);
    std::fprintf(stderr, "=== SEED %ld ===\n%s\n", seed, src.c_str());
    std::string tape{};
    for (val_t v : g.ins)
      tape += std::to_string(v) + " ";
    std::fprintf(stderr, "=== TAPE %ld: %s===\n", seed, tape.c_str());
  }
  PassCfg cfg = fuzz_cfg_for(seed, ctx.random_configs);
  Program P = compile(src, cfg);
  if (!P.ok) {
    std::lock_guard<std::mutex> lk(ctx.mu);
    std::printf("FUZZ COMPILE ERROR seed %ld: %s at %u\nsource:\n%s\n", seed,
                P.error ? P.error : "?", (unsigned)P.errpos, src.c_str());
    ++g_checks;
    ++g_fails;
    std::fflush(stdout);
    return false;
  }
  if (src.find("array(") != std::string::npos)
    F.arr_progs++;
  if (P.uses_input)
    F.in_progs++;
  if (src.find("fn ") != std::string::npos)
    F.fn_progs++;
  if (g.has_brk_cont)
    F.brk_progs++;
  if (g.n_bkpts > 0) {
    F.bkpt_progs++;
    F.bkpts += g.n_bkpts;
  }
  if (g.has_div)
    F.div_progs++;
  if (g.has_mat)
    F.mat_progs++;
  if (g.has_aparam)
    F.aparam_progs++;
  if (g.n_gvnbait > 0) {
    F.gvn_progs++;
    F.gvn_baits += g.n_gvnbait;
  }
  if (g.n_unswbait > 0) {
    F.unsw_progs++;
    F.unsw_baits += g.n_unswbait;
  }
  if (g.n_ccbait > 0) {
    F.cc_progs++;
    F.cc_baits += g.n_ccbait;
  }
  if (g.n_bifbait > 0) {
    F.bif_progs++;
    F.bif_baits += g.n_bifbait;
  }
  {
    OptStats os = sum_ostats(P);
    F.inlined_total += os.inlines;
    F.unrolled_total += os.unrolls;
    F.lifted += os.mem2reg_lifts;
    F.dead_stores += os.mem2reg_dses;
    F.constcalls += os.constcalls;
    F.deadallocs += os.deadallocs;
    F.tces += os.tces + os.tce_merges;
    F.cdfolds += os.cdfolds;
    F.ifconvs += os.ifconvs;
    F.gensims += os.lcf_gensims;
    F.traps_killed += os.traps_killed;
    F.scctco_total += os.scctco;
    F.scctco_fold_total += os.scctco_folds;
    F.sccp_consts += os.sccp_consts;
    F.sccp_brs += os.sccp_brs;
    F.ld_fwds += os.ld_fwds;
    F.ld_cses += os.ld_cses;
    F.rangecmps += os.rangecmps;
    F.cmpfacts += os.cmpfacts;
    F.callcses += os.callcses;
    F.deadargs += os.deadargs;
    F.fnspecs += os.fnspecs;
    F.icf += os.icf;
    F.jthreads += os.jthreads;
    F.argpromos += os.argpromos;
    F.gvns += os.gvns;
    F.unswitches += os.unswitches;
    F.lsimps += os.lsimps;
  }
  RunOut oracle = run_ast(P.P.ast, P.P.root, 200000, g.ins);
  RunOut plain = run_prog(fns_of(P, 0), 200000, g.ins);
  RunOut optimized = run_prog(fns_of(P, 1), 200000, g.ins);
  RunOut lowered = run_prog(fns_of(P, 2), 200000, g.ins);
  std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
  RunOut vm = vm_run(bc, 2000000, g.ins);
  F.cases++;
  F.timeouts += oracle.timeout ? 1 : 0;
  F.traps += oracle.trapped ? 1 : 0;
  F.licm_hoists += sum_ostats(P).hoisted;
  F.dce_instrs += sum_ostats(P).dce;
  for (const FnUnit &U : P.fns)
    F.phis += U.sstats.phis;
  bool bad = !same_run(oracle, plain) || !same_run(oracle, optimized);
  if (!oracle.timeout) { // step-limited runners only compared when finite
    bad |= !same_run(oracle, lowered);
    bad |= !same_run(oracle, vm);
  }
  if (bad) {
    std::lock_guard<std::mutex> lk(ctx.mu);
    ctx.mismatches.fetch_add(1);
    std::printf("MISMATCH seed %ld\nsource:\n%s\n", seed, src.c_str());
    std::printf("flags   : %s\n", describe_cfg(cfg).c_str());
    // (ported from Branchedmain) auto-save the failure: .mjz + .flags +
    // .in next to the output dir, so --shrink can reduce it and a fresh
    // session can reproduce it exactly
    {
      std::string stem = ctx.out_dir + "/fuzz_fail_" + std::to_string(seed);
      write_file(stem + ".mjz", src);
      write_file(stem + ".flags", "# repro: ./minijz " + stem +
                                      ".mjz --stdin=" + stem + ".in " +
                                      describe_cfg(cfg) + "\n");
      std::string t{};
      for (val_t v : g.ins)
        t += std::to_string(v) + "\n";
      write_file(stem + ".in", t);
      std::printf("saved   : %s.mjz / .flags / .in (shrink: ./minijz "
                  "--shrink=%s.mjz)\n",
                  stem.c_str(), stem.c_str());
    }
    std::printf("oracle : trapped=%d timeout=%d out=", oracle.trapped,
                oracle.timeout);
    for (val_t v : oracle.out)
      std::printf("%lld ", (long long)v);
    std::printf("\nssa    : trapped=%d timeout=%d out=", plain.trapped,
                plain.timeout);
    for (val_t v : plain.out)
      std::printf("%lld ", (long long)v);
    std::printf("\nopt    : trapped=%d timeout=%d out=", optimized.trapped,
                optimized.timeout);
    for (val_t v : optimized.out)
      std::printf("%lld ", (long long)v);
    std::printf("\nlower  : trapped=%d timeout=%d out=", lowered.trapped,
                lowered.timeout);
    for (val_t v : lowered.out)
      std::printf("%lld ", (long long)v);
    std::printf("\nvm     : trapped=%d timeout=%d out=", vm.trapped,
                vm.timeout);
    for (val_t v : vm.out)
      std::printf("%lld ", (long long)v);
    std::printf("\n");
    ++g_checks;
    ++g_fails;
    std::fflush(stdout);
  }
  // --- native parity sweep (--fuzz-native): build C+asm at -O0/-O2 and
  // compare their output against the oracle, tape and all. This is the
  // end-to-end gate for the emission plan (layout, immediates, fusion,
  // dropped zero-init) — any of those going wrong shows up here.
  if (ctx.native_left.load(std::memory_order_relaxed) > 0 && !ctx.cc.empty() &&
      !oracle.timeout) {
    bool won = false;
    long nb = ctx.native_left.load(std::memory_order_acquire);
    while (nb > 0)
      if (ctx.native_left.compare_exchange_weak(nb, nb - 1,
                                                std::memory_order_acquire)) {
        won = true;
        break;
      }
    if (won) {
      std::string stem = ctx.ndir + "/fz" + std::to_string(seed);
      std::string c_path = stem + ".c", s_path = stem + ".s";
      write_file(c_path, emit_c(fns_of(P, 2), P.fn_names, P.src));
      write_file(s_path, emit_x64(fns_of(P, 2), P.fn_names, P.src));
      std::string tape = stem + ".in";
      {
        std::string t{};
        for (val_t v : g.ins)
          t += std::to_string(v) + "\n";
        write_file(tape, t);
      }
      std::string ref_text = text_of(oracle);
      const char *tags[2] = {"c", "asm"};
      for (int ti = 0; ti < 2; ++ti)
        for (const char *lvl : {"0", "2"}) {
          std::string exe = stem + "." + tags[ti] + ".O" + lvl + ".exe";
          std::string srcp = ti ? s_path : c_path;
          std::string build = mjz_timeout(60, ctx.cc + " -w -O" + lvl + " " +
                                                  srcp + " -o " + exe);
          if (std::system(build.c_str()) != 0) {
            std::lock_guard<std::mutex> lk(ctx.mu);
            std::printf("NATIVE BUILD FAILED seed %ld (%s -O%s)\n", seed,
                        tags[ti], lvl);
            ++g_checks;
            ++g_fails;
            ctx.mismatches.fetch_add(1);
            std::fflush(stdout);
            continue;
          }
          std::string cap = stem + ".out";
          std::string out_text{};
          int rc = run_capture(mjz_timeout(20, exe), out_text, cap, tape);
          bool timed_out = (rc != 0 && (((unsigned)rc >> 8) == 124 ||
                                        ((unsigned)rc >> 8) == 137));
          if (timed_out)
            out_text += "TIMEOUT\n";
          if (out_text != ref_text) {
            std::lock_guard<std::mutex> lk(ctx.mu);
            ctx.mismatches.fetch_add(1);
            std::printf("NATIVE MISMATCH seed %ld (%s -O%s)\nsource:\n%s\n"
                        "expected: %s\ngot     : %s\n",
                        seed, tags[ti], lvl, src.c_str(), ref_text.c_str(),
                        out_text.c_str());
            ++g_checks;
            ++g_fails;
            std::fflush(stdout);
          }
        }
      F.native_checked++;
    }
  }
  // live progress: a single seed must never be silent for long. This is
  // the fix for "--fuzz=1000 seems to not halt": the run now reports
  // continuously, and any seed slower than 5s is named on stderr.
  double dt =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t_seed0)
          .count();
  if (dt > 5.0) {
    std::lock_guard<std::mutex> lk(ctx.mu);
    std::fprintf(stderr, "[fuzz] note: seed %ld took %.1fs\n", seed, dt);
    std::fflush(stderr);
  }
  return !bad;
}

// the final one-line-per-stat summary (unchanged format; battery-visible)
static void fuzz_report(const FuzzStats &F, long mismatches, long jobs,
                        double secs) {
  std::printf(
      "fuzz: %ld programs | mismatches: %ld | timeouts: %ld | "
      "traps: %ld\n  phis placed: %ld | instrs DCE'd: %ld | "
      "LICM hoists: %ld | programs w/ arrays: %ld | w/ input(): %ld"
      " | w/ fns: %ld | w/ break/continue: %ld\n  inlined: %ld | "
      "unrolled: %ld | arrays lifted: %ld | dead stores: %ld | "
      "const-called: %ld | dead allocs: %ld | tailrec'd: %ld | "
      "cdfolds: %ld | ifconv'd: %ld | "
      "general-sim folds: %ld | traps proven impossible: %ld | "
      "SCC-TCO collapses: %ld (folds: %ld) | "
      "SCCP consts: %ld (brs: %ld) | load fwd/CSE: %ld/%ld | "
      "range-cmps: %ld | cmp-facts: %ld | call-CSE: %ld | "
      "dead-args: %ld | fnspec: %ld | icf: %ld | "
      "jthreads: %ld | argpromos: %ld | gvns: %ld | unswitches: %ld | "
      "lsimps: %ld | "
      "native-checked (C+asm x -O0/-O2): %ld\n"
      "  vocabulary: w/ div/mod: %ld | w/ 2-D arrays: %ld | "
      "w/ array params: %ld | w/ breakpoints: %ld (%ld pts) | "
      "w/ shadow merges: %ld (%ld pairs) | w/ unswitch bait: %ld (%ld loops) | "
      "w/ compound-cond bait: %ld (%ld loops)\n"
      "  w/ body-if bait: %ld (%ld loops)\n"
      "  wall: %.1fs on %ld job%s\n",
      F.cases, mismatches, F.timeouts, F.traps, F.phis, F.dce_instrs,
      F.licm_hoists, F.arr_progs, F.in_progs, F.fn_progs, F.brk_progs,
      F.inlined_total, F.unrolled_total, F.lifted, F.dead_stores, F.constcalls,
      F.deadallocs, F.tces, F.cdfolds, F.ifconvs, F.gensims, F.traps_killed,
      F.scctco_total, F.scctco_fold_total, F.sccp_consts, F.sccp_brs, F.ld_fwds,
      F.ld_cses, F.rangecmps, F.cmpfacts, F.callcses, F.deadargs, F.fnspecs,
      F.icf, F.jthreads, F.argpromos, F.gvns, F.unswitches, F.lsimps,
      F.native_checked, F.div_progs, F.mat_progs, F.aparam_progs, F.bkpt_progs,
      F.bkpts, F.gvn_progs, F.gvn_baits, F.unsw_progs, F.unsw_baits, F.cc_progs,
      F.cc_baits, F.bif_progs, F.bif_baits, secs, jobs, jobs == 1 ? "" : "s");
  CHECK(mismatches == 0, "fuzz must be clean");
}

void run_fuzz(long n, const std::string &cc_in, long fuzz_native,
              const std::string &out_dir, long from = 1,
              bool random_configs = false, bool mjz_fuzz_dumps = false,
              long jobs = 1) {
  const auto t0 = std::chrono::steady_clock::now();
  std::printf("=== differential fuzz: oracle vs SSA vs optimized vs destroyed "
              "vs VM (rotating -O configs%s) ===\n",
              random_configs ? " + RANDOM pass configs" : "");
  std::fflush(stdout);
  FuzzCtx ctx{};
  ctx.cc = cc_in.empty() ? find_cc() : cc_in;
  ctx.out_dir = out_dir;
  ctx.from = from;
  ctx.n = n;
  ctx.random_configs = random_configs;
  ctx.dump = mjz_fuzz_dumps;
  ctx.jobs = jobs < 1 ? 1 : jobs;
  ctx.next_seed.store(from);
  ctx.native_left.store(fuzz_native);
  if (fuzz_native > 0 && !ctx.cc.empty()) {
    ctx.ndir = out_dir + "/fuzz_native";
    // portable mkdir -p (cmd.exe has no -p; C++ filesystem has both)
    std::error_code ec{};
    std::filesystem::create_directories(ctx.ndir, ec);
  }
  long report_every = n / 40;
  if (report_every < 25)
    report_every = 25;
  if (ctx.jobs <= 1) {
    // ---- serial: identical semantics to the historical driver ----------
    FuzzStats F{};
    for (long seed = from; seed < from + n; ++seed) {
      (void)fuzz_one(seed, ctx, F);
      long d = ctx.done.fetch_add(1) + 1;
      if (d % report_every == 0 || d == n) {
        double el =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                .count();
        std::fprintf(stderr,
                     "[fuzz %ld/%ld] %.0fs elapsed, eta %.0fs, "
                     "mismatches %ld\n",
                     d, n, el, el / d * (n - d), ctx.mismatches.load());
        std::fflush(stderr);
      }
    }
    double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    fuzz_report(F, ctx.mismatches.load(), 1, secs);
    return;
  }
  // ---- parallel: workers steal seeds from an atomic queue; a watchdog
  // names the in-flight seeds if progress stalls (so a pathological seed
  // is immediately identifiable instead of looking like a hang) ----------
  if (ctx.jobs > FuzzCtx::MAX_WORKERS)
    ctx.jobs = FuzzCtx::MAX_WORKERS;
  std::fprintf(stderr, "[fuzz] %ld seeds on %ld jobs (from %ld)\n", n, ctx.jobs,
               from);
  std::atomic<bool> stop_watchdog{false};
  long last_done = -1;
  std::thread watchdog([&ctx, &stop_watchdog, &last_done]() {
    long stall = 0;
    while (!stop_watchdog.load()) {
      for (int i = 0; i < 20; ++i) { // 20 x 0.5s = 10s stall window
        if (stop_watchdog.load())
          return;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
      long d = ctx.done.load();
      if (d == last_done) {
        ++stall;
        std::fprintf(stderr,
                     "[fuzz] WATCHDOG: no seed finished for %lds (done=%ld/"
                     "%ld); in flight:",
                     stall * 10, d, ctx.n);
        for (long w = 0; w < ctx.jobs; ++w) {
          long s = ctx.inflight[w].load();
          if (s)
            std::fprintf(stderr, " seed %ld", s);
        }
        std::fprintf(stderr,
                     " — if this persists, bisect with --fuzz-from=<seed>\n");
        std::fflush(stderr);
      } else {
        stall = 0;
      }
      last_done = d;
    }
  });
  std::vector<std::thread> workers{};
  std::vector<FuzzStats> per(ctx.jobs);
  for (long w = 0; w < ctx.jobs; ++w) {
    workers.emplace_back([w, &ctx, &per, &report_every, &t0, n]() {
      FuzzStats &F = per[w];
      for (;;) {
        long seed = ctx.next_seed.fetch_add(1);
        if (seed >= ctx.from + ctx.n)
          break;
        ctx.inflight[w].store(seed);
        (void)fuzz_one(seed, ctx, F);
        ctx.inflight[w].store(0);
        long d = ctx.done.fetch_add(1) + 1;
        if (w == 0 && (d % report_every == 0 || d == n)) {
          double el = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
          std::fprintf(stderr,
                       "[fuzz %ld/%ld] %.0fs elapsed, eta %.0fs, "
                       "mismatches %ld\n",
                       d, n, el, el / d * (n - d), ctx.mismatches.load());
          std::fflush(stderr);
        }
      }
    });
  }
  for (std::thread &t : workers)
    t.join();
  stop_watchdog.store(true);
  watchdog.join();
  FuzzStats F{};
  for (const FuzzStats &p : per)
    fuzz_stats_add(F, p);
  double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  fuzz_report(F, ctx.mismatches.load(), ctx.jobs, secs);
}
