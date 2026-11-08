#pragma once
// === fuzz_run.hpp ========================================
// the differential fuzz driver (FuzzStats + run_fuzz), now with
// random pass configs and failure auto-save.

#include "minijz/base/pass_cfg.hpp"
#include "minijz/prog/program.hpp"
#include "minijz/prog/runners.hpp"
#include "minijz/runtime/astrun.hpp"
#include "minijz/back/bytecode.hpp"
#include "minijz/base/native_run.hpp"
#include "minijz/apps/fuzz_gen.hpp"

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
  long constcalls{}, deadallocs{}, tces{}, cdfolds{}, ifconvs{};
  long scctco_total{}, scctco_fold_total{};
  long sccp_consts{}, sccp_brs{}, ld_fwds{}, ld_cses{}, rangecmps{};
  long cmpfacts{}, callcses{};
  long div_progs{}, mat_progs{}, aparam_progs{};
};

static void run_fuzz(long n, const std::string &cc_in, long fuzz_native,
                     const std::string &out_dir, long from = 1,
                     bool random_configs = false,bool mjz_fuzz_dumps =false) {
  std::printf("=== differential fuzz: oracle vs SSA vs optimized vs destroyed "
              "vs VM (rotating -O configs%s) ===\n",
              random_configs ? " + RANDOM pass configs" : "");
  FuzzStats F{};
  long mismatches = 0;
  const std::string cc = cc_in.empty() ? find_cc() : cc_in;
  std::string ndir{};
  if (fuzz_native > 0 && !cc.empty()) {
    ndir = out_dir + "/fuzz_native";
    std::system(("mkdir -p '" + ndir + "'").c_str());
  }
  // (ported from the Branchedmain monolith) deterministic per-seed PRNG for
  // the random pass configs: xorshift, no state outside the loop
  auto pick32 = [](uint32_t &rng) -> uint32_t {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  };
  for (long seed = from; seed < from + n; ++seed) {
    GenCtx g = gen_program((uint32_t)seed);
    const std::string &src = g.src;
    //std::getenv("MJZ_FUZZ_DUMP") is not recommended
    if ( mjz_fuzz_dumps&& seed >= from + n - 2) {
      std::fprintf(stderr, "=== SEED %ld ===\n%s\n", seed, src.c_str());
      std::string tape{};
      for (val_t v : g.ins) tape += std::to_string(v) + " ";
      std::fprintf(stderr, "=== TAPE %ld: %s===\n", seed, tape.c_str());
    }
    // rotate the pass configuration: every run exercises a different slice
    // of the pipeline (defaults are level 3 + register allocation)
    PassCfg cfg{};
    cfg.set_level(3);
    cfg.fp_limit = 8; // fuzz programs are pathological: cap fixpoint depth
    switch (seed % 11) {
    case 0: cfg.p_scctco = false; break;   // no SCC tail-call collapse
    case 1: cfg.p_lcf = false; break;      // no closed forms / simulators
    case 2: cfg.p_unroll = false; break;   // no unrolling
    case 3: cfg.p_mem2reg = false; break;  // no array promotion
    case 4: cfg.p_trapelim = false; break; // no range/alias trap proofs
    case 5: cfg.set_level(2); break;       // mid pipeline
    case 6: cfg.set_level(1); break;       // cheap tier only
    case 7: cfg.p_cse = false; cfg.p_licm = false; cfg.p_reassoc = false;
      cfg.p_divmagic = false; break;       // scalar opts off
    case 8: cfg.p_tailrec = false; break;  // no tail-call elimination
    case 9: cfg.p_cdfold = false; cfg.p_ifconv = false; break; // no
                                                   // countdown fold /
                                                   // if-conv
    case 10: cfg.p_sccp = false; cfg.p_ldelim = false;
      cfg.p_callcse = false; break;        // no new-portable-opts slice
    }                                      // (SCCP/ldelim/callcse)
    // (ported from Branchedmain) --fuzz-cfg=random: additionally disable a
    // random 1..4 of the 23 toggles and pick a random tier, exposing pass
    // INTERACTION bugs the fixed rotation never reaches
    if (random_configs) {
      uint32_t rng = (uint32_t)seed * 2654435761u + 0x9e3779b9u;
      (void)pick32(rng);
      cfg.set_level(1u + pick32(rng) % 3u);
      if (cfg.level >= 2) cfg.fp_limit = 8;
      uint32_t ndis = 1u + pick32(rng) % 4u;
      for (uint32_t d = 0; d < ndis; ++d)
        (void)apply_pass_flag(cfg, pick32(rng) % PASS_TOGGLE_COUNT, false);
    }
    Program P = compile(src, cfg);
    if (!P.ok) {
      std::printf("FUZZ COMPILE ERROR seed %ld: %s at %u\nsource:\n%s\n",
                  seed, P.error ? P.error : "?", (unsigned)P.errpos,
                  src.c_str());
      CHECK(false, "fuzz program must compile");
      continue;
    }
    if (src.find("array(") != std::string::npos) F.arr_progs++;
    if (P.uses_input) F.in_progs++;
    if (src.find("fn ") != std::string::npos) F.fn_progs++;
    if (g.has_brk_cont) F.brk_progs++;
    if (g.has_div) F.div_progs++;
    if (g.has_mat) F.mat_progs++;
    if (g.has_aparam) F.aparam_progs++;
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
    for (const FnUnit &U : P.fns) F.phis += U.sstats.phis;
    bool bad = !same_run(oracle, plain) || !same_run(oracle, optimized);
    if (!oracle.timeout) { // step-limited runners only compared when finite
      bad |= !same_run(oracle, lowered);
      bad |= !same_run(oracle, vm);
    }
    if (bad) {
      ++mismatches;
      std::printf("MISMATCH seed %ld\nsource:\n%s\n", seed, src.c_str());
      std::printf("flags   : %s\n", describe_cfg(cfg).c_str());
      // (ported from Branchedmain) auto-save the failure: .mjz + .flags +
      // .in next to the output dir, so --shrink can reduce it and a fresh
      // session can reproduce it exactly
      {
        std::string stem = out_dir + "/fuzz_fail_" + std::to_string(seed);
        write_file(stem + ".mjz", src);
        write_file(stem + ".flags",
                   "# repro: ./minijz " + stem + ".mjz --stdin=" + stem +
                       ".in " + describe_cfg(cfg) + "\n");
        std::string t{};
        for (val_t v : g.ins) t += std::to_string(v) + "\n";
        write_file(stem + ".in", t);
        std::printf("saved   : %s.mjz / .flags / .in (shrink: ./minijz "
                    "--shrink=%s.mjz)\n",
                    stem.c_str(), stem.c_str());
      }
      std::printf("oracle : trapped=%d timeout=%d out=", oracle.trapped,
                  oracle.timeout);
      for (val_t v : oracle.out) std::printf("%lld ", (long long)v);
      std::printf("\nssa    : trapped=%d timeout=%d out=", plain.trapped,
                  plain.timeout);
      for (val_t v : plain.out) std::printf("%lld ", (long long)v);
      std::printf("\nopt    : trapped=%d timeout=%d out=", optimized.trapped,
                  optimized.timeout);
      for (val_t v : optimized.out) std::printf("%lld ", (long long)v);
      std::printf("\nlower  : trapped=%d timeout=%d out=", lowered.trapped,
                  lowered.timeout);
      for (val_t v : lowered.out) std::printf("%lld ", (long long)v);
      std::printf("\nvm     : trapped=%d timeout=%d out=", vm.trapped,
                  vm.timeout);
      for (val_t v : vm.out) std::printf("%lld ", (long long)v);
      std::printf("\n");
      CHECK(false, "fuzz: all five runners must agree");
    }
    // --- native parity sweep (--fuzz-native): build C+asm at -O0/-O2 and
    // compare their output against the oracle, tape and all. This is the
    // end-to-end gate for the emission plan (layout, immediates, fusion,
    // dropped zero-init) — any of those going wrong shows up here.
    if (fuzz_native > 0 && !cc.empty() && !oracle.timeout) {
      --fuzz_native;
      std::string stem = ndir + "/fz" + std::to_string(seed);
      std::string c_path = stem + ".c", s_path = stem + ".s";
      write_file(c_path, emit_c(fns_of(P, 2), P.fn_names));
      write_file(s_path, emit_x64(fns_of(P, 2), P.fn_names));
      std::string tape = stem + ".in";
      {
        std::string t{};
        for (val_t v : g.ins) t += std::to_string(v) + "\n";
        write_file(tape, t);
      }
      std::string ref_text = text_of(oracle);
      const char *tags[2] = {"c", "asm"};
      for (int ti = 0; ti < 2; ++ti)
        for (const char *lvl : {"0", "2"}) {
          std::string exe =
              stem + "." + tags[ti] + ".O" + lvl + ".exe";
          std::string srcp = ti ? s_path : c_path;
          std::string build =
              "timeout 60 " + cc + " -w -O" + lvl + " " + srcp + " -o " + exe;
          if (std::system(build.c_str()) != 0) {
            std::printf("NATIVE BUILD FAILED seed %ld (%s -O%s)\n", seed,
                        tags[ti], lvl);
            CHECK(false, "fuzz-native build");
            ++mismatches;
            continue;
          }
          std::string cap = stem + ".out";
          std::string out_text{};
          int rc = run_capture("timeout 20 " + exe, out_text, cap, tape);
          bool timed_out =
              (rc != 0 && (((unsigned)rc >> 8) == 124 ||
                           ((unsigned)rc >> 8) == 137));
          if (timed_out) out_text += "TIMEOUT\n";
          if (out_text != ref_text) {
            ++mismatches;
            std::printf("NATIVE MISMATCH seed %ld (%s -O%s)\nsource:\n%s\n"
                        "expected: %s\ngot     : %s\n",
                        seed, tags[ti], lvl, src.c_str(), ref_text.c_str(),
                        out_text.c_str());
            CHECK(false, "fuzz-native: native output must match oracle");
          }
        }
      F.native_checked++;
    }
  }
  std::printf("fuzz: %ld programs | mismatches: %ld | timeouts: %ld | "
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
              "native-checked (C+asm x -O0/-O2): %ld\n"
              "  vocabulary: w/ div/mod: %ld | w/ 2-D arrays: %ld | "
              "w/ array params: %ld\n",
              F.cases, mismatches, F.timeouts, F.traps, F.phis, F.dce_instrs,
              F.licm_hoists, F.arr_progs, F.in_progs, F.fn_progs, F.brk_progs,
              F.inlined_total, F.unrolled_total, F.lifted, F.dead_stores,
              F.constcalls, F.deadallocs, F.tces, F.cdfolds, F.ifconvs,
              F.gensims, F.traps_killed, F.scctco_total, F.scctco_fold_total,
              F.sccp_consts, F.sccp_brs, F.ld_fwds, F.ld_cses, F.rangecmps,
              F.cmpfacts, F.callcses,
              F.native_checked, F.div_progs, F.mat_progs, F.aparam_progs);
  CHECK(mismatches == 0, "fuzz must be clean");
}
