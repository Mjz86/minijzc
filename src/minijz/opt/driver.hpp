#pragma once
// === driver.hpp ========================================
// the pass manager: tiered fixpoint scheduling, -O presets.

#include "minijz/ir/instr.hpp"
#include "minijz/opt/optimize_api.hpp" // the entry declaration (light)
#include "minijz/base/pass_cfg.hpp"
#include "minijz/opt/analysis/an.hpp"
#include "minijz/opt/analysis/intervals.hpp"
#include "minijz/opt/analysis/trapelim.hpp"
#include "minijz/opt/mem2reg.hpp"
#include "minijz/opt/scalar/fold.hpp"
#include "minijz/opt/scalar/copyprop.hpp"
#include "minijz/opt/scalar/canon.hpp"
#include "minijz/opt/scalar/cse.hpp"
#include "minijz/opt/scalar/unreach.hpp"
#include "minijz/opt/scalar/splice.hpp"
#include "minijz/opt/scalar/dce.hpp"
#include "minijz/opt/scalar/ifconv.hpp"
#include "minijz/opt/loop/licm.hpp"
#include "minijz/opt/loop/unroll.hpp"
#include "minijz/opt/loop/lcf.hpp"
#include "minijz/opt/loop/lcf_rt.hpp"
#include "minijz/opt/linear/reassoc.hpp"
#include "minijz/opt/linear/divmagic.hpp"
#include "minijz/opt/interprocedural/callgraph.hpp"
#include "minijz/opt/interprocedural/cdfold.hpp"

struct Optimized {
  Fn fn{}; // SSA after optimization
  OptStats os{};
  An an{}; // final analysis (post-opt)
};
constexpr bool optimize(Fn &fn, OptStats &os, An &final_an, PassCfg cfg) {
  if (!cfg.do_opt || cfg.level == 0) { // -O0: untouched IR, fresh analysis
    final_an = analyze(fn);
    return false;
  }
  bool any = false;
  const uint32_t cap = cfg.fp_limit ? cfg.fp_limit : 24;
  if (cfg.debug >= 1) {
    char nb[64] = "<fn>";
    if (!cfg.dbg_name.empty())
      std::snprintf(nb, sizeof nb, "%.*s", (int)cfg.dbg_name.size(),
                    cfg.dbg_name.data());
    std::printf("==== optimize %s (level %u, fp-limit %u) ====\n", nb,
                (unsigned)cfg.level, (unsigned)cap);
  }
  // ---- tier 0: cheap simplification to its fixpoint -----------------------
  // (each round must make progress; dominance passes see a fresh An)
  auto tier0 = [&]() -> bool {
    bool ch_any = false;
    for (uint32_t it = 0; it < cap; ++it) {
      bool ch = false, c = false;
      c = opt_constprop(fn, os, cfg.is_main); // fold/identities/branch folds
      dbg_pass(cfg.debug, fn, "fold", c);
      ch |= c;
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_COPYPROP
      if (cfg.p_copyprop) {
        c = opt_copyprop(fn, os); // copies -> uses of the source
        dbg_pass(cfg.debug, fn, "copyprop", c);
        ch |= c;
      }
#endif
      normalize_phis(fn);
      if (cfg.p_canon) {
        c = opt_canon(fn, os); // canonical forms + NOT-of-compare
        dbg_pass(cfg.debug, fn, "canon", c);
        ch |= c;
      }
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_CSE
      if (cfg.p_cse) {
        An A = analyze(fn);
        c = opt_cse(fn, A, os); // dominating recomputation -> reuse
        dbg_pass(cfg.debug, fn, "cse", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (cfg.p_unreach) {
        c = opt_remove_unreachable(fn, os); // dead blocks + phi degeneration
        dbg_pass(cfg.debug, fn, "unreach", c);
        ch |= c;
      }
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_SPLICE
      if (cfg.p_splice) {
        c = opt_splice(fn, os); // collapse single-pred/succ jump chains
        dbg_pass(cfg.debug, fn, "splice", c);
        ch |= c;
      }
#endif
      normalize_phis(fn);
      if (cfg.p_dce) {
        c = opt_dce(fn, os);
        dbg_pass(cfg.debug, fn, "dce", c);
        ch |= c;
      }
      normalize_phis(fn);
      os.iters = it + 1;
      if (!ch)
        break;
      ch_any = true;
    }
    return ch_any;
  };
  any |= tier0();
  // ---- tier 1: middle passes; cheap fixpoint after each round -------------
  // trapelim runs ONCE per optimize() call: its nt proofs are monotone (a
  // proof stays valid as the fn shrinks), and its interval analysis is the
  // most expensive thing in the tier — re-running it every fixpoint round
  // of every tier re-analysis made pathological inputs ~20x slower for no
  // additional proofs. Later rounds keep the flags; the tier-2 post-round
  // re-runs it once more (CFG restructuring can expose new proofs).
  bool trapelim_ran = false;
  if (cfg.level >= 2) {
    for (uint32_t outer = 0; outer < cap; ++outer) {
      bool ch = false, c = false;
      if (cfg.p_mem2reg) {
        An A = analyze(fn);
        c = opt_mem2reg(fn, A, os, cfg.lift_ok, cfg.stkarr_ok);
        dbg_pass(cfg.debug, fn, "mem2reg", c);
        ch |= c;
        normalize_phis(fn);
      }
      if (cfg.p_trapelim && (outer == 0 || !trapelim_ran)) {
        An A = analyze(fn);
        c = opt_trapelim(fn, A, os); // ranges + aliasing -> nt flags
        dbg_pass(cfg.debug, fn, "trapelim", c);
        trapelim_ran = true;
        ch |= c;
        normalize_phis(fn);
      }
#ifndef MJZ_DBG_NO_LICM
      if (cfg.p_licm) {
        An A = analyze(fn);
        c = opt_licm(fn, A, os); // hoist invariants to the preheader
        dbg_pass(cfg.debug, fn, "licm", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_DIVMAGIC
      if (cfg.p_divmagic) {
        c = opt_divmagic(fn, os); // /constant -> mulhi+shift
        dbg_pass(cfg.debug, fn, "divmagic", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_REASSOC
      if (cfg.p_reassoc) {
        c = opt_reassoc(fn, os); // canonicalize +/-/*c chains (feeds LCF)
        dbg_pass(cfg.debug, fn, "reassoc", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (cfg.p_ifconv) { // provably-binary cbr selects -> arithmetic
        c = opt_ifconv(fn, os);
        dbg_pass(cfg.debug, fn, "ifconv", c);
        ch |= c;
        normalize_phis(fn);
      }
      if (!ch)
        break;
      any = true;
      tier0();
    }
  }
  // ---- tier 2: expensive CFG restructuring, only after 0/1 fixpoint -------
  if (cfg.level >= 3) {
    uint32_t t2round = 0;
    for (uint32_t outer = 0; outer < cap; ++outer) {
      bool ch = false;
      ++t2round;
#ifndef MJZ_DBG_NO_UNROLL
      if (cfg.p_unroll && cfg.unroll) {
        An A = analyze(fn);
        bool c = opt_unroll(fn, A, os, cfg.unroll);
        dbg_pass(cfg.debug, fn, "unroll", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_LCF
      if (cfg.p_lcf) {
        An A2 = analyze(fn);          // unrolling may have reshaped the CFG
        bool c = opt_lcf(fn, A2, os); // closed forms / simulators
        dbg_pass(cfg.debug, fn, "lcf", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (cfg.p_cdfold) { // TCE countdown loops -> exact closed form
        An A3 = analyze(fn);
        bool c = opt_cdfold(fn, A3, os);
        dbg_pass(cfg.debug, fn, "cdfold", c);
        ch |= c;
        normalize_phis(fn);
      }
      if (!ch)
        break;
      any = true;
      tier0();
      // one tier-1 round: restructuring exposes new opportunities
      if (cfg.level >= 2) {
        bool c2 = false;
        if (cfg.p_mem2reg) {
          An A = analyze(fn);
          c2 |= opt_mem2reg(fn, A, os, cfg.lift_ok, cfg.stkarr_ok);
          normalize_phis(fn);
        }
        if (cfg.p_trapelim && t2round < 2) { // restructuring-exposed proofs
          An A = analyze(fn);
          c2 |= opt_trapelim(fn, A, os);
          normalize_phis(fn);
        }
        if (cfg.p_licm) {
          An A = analyze(fn);
          c2 |= opt_licm(fn, A, os);
          normalize_phis(fn);
        }
        if (cfg.p_divmagic) {
          c2 |= opt_divmagic(fn, os);
          normalize_phis(fn);
        }
        if (cfg.p_reassoc) {
          c2 |= opt_reassoc(fn, os);
          normalize_phis(fn);
        }
        if (cfg.p_cdfold) {
          An A3 = analyze(fn);
          c2 |= opt_cdfold(fn, A3, os);
          normalize_phis(fn);
        }
        if (cfg.p_ifconv) {
          c2 |= opt_ifconv(fn, os);
          normalize_phis(fn);
        }
        (void)c2;
        tier0();
      }
    }
  }
  normalize_phis(fn);
  final_an = analyze(fn);
  return any;
}

// --- 9f2. stack-allocation analysis (for the C backend) ---------------------
