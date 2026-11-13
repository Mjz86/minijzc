#pragma once
// === driver.hpp ========================================
// the pass manager: tiered fixpoint scheduling, -O presets.
//
// THEORY (phase ordering, and why a fixpoint at all):
//   * WHY ITERATE. Every pass here exposes work for another: sccp
//     proves constants fold can use; fold collapses branches which
//     unreach then deletes; splice merges blocks so CSE sees longer
//     runs; unroll materializes constants for the next sccp round.
//     No single ordering is a fixpoint for all programs, so each
//     tier loops `cap` rounds (fp-limit, default 24) until a full
//     round changes nothing -- Kleene-style iteration to the least
//     fixed point of the composition, in practice 2-4 rounds.
//   * THE TIERS (cheap-first, so expensive passes see maximally
//     simplified input):
//       tier 0 (all levels): local simplification to ITS fixpoint
//         -- sccp, fold, copyprop, canon, cse, gvn, unreach,
//            splice, dce (each round; normalize_phis between).
//       tier 1 (level >= 2): mem2reg, ldelim, taildup,
//         trapelim+jthread (ONE shared range analysis per call --
//         the cost note below), licm, lsimp, divmagic, reassoc,
//         ifconv; a tier-0 round runs after each tier-1 round.
//       tier 2 (level >= 3): the expensive CFG rewrites -- and
//         their ORDER IS LOAD-BEARING:
//           unswitch BEFORE unroll (the versioned true-arm loses
//             its exit edge, so unroll/lcf skip it anyway);
//           unroll BEFORE lcf (unrolling reshapes the CFG lcf
//             then scans);
//           rotate LAST (unswitch/unroll/lcf need the test-first
//             shape; the rotated header ends in Br -- invisible
//             to their shape matchers, see the inline note).
//   * FRESHNESS CONTRACT. Every dominance pass gets a fresh
//     analyze() -- and each pass guards on A.n == fn.nblocks()
//     (a stale An from a different block count is rejected).
//     The per-tier tier0() re-runs after restructuring exist
//     because restructuring EXPOSES opportunities (dead arms,
//     new constants, new elbows).

#include "../ir/instr.hpp"
#include "optimize_api.hpp" // the entry declaration (light)
#include "../base/pass_cfg.hpp"
#include "analysis/an.hpp"
#include "analysis/intervals.hpp"
#include "analysis/trapelim.hpp"
#include "mem2reg.hpp"
#include "scalar/sccp.hpp"
#include "scalar/fold.hpp"
#include "scalar/copyprop.hpp"
#include "scalar/canon.hpp"
#include "scalar/cse.hpp"
#include "scalar/gvn.hpp"
#include "scalar/unreach.hpp"
#include "scalar/splice.hpp"
#include "scalar/jthread.hpp"
#include "scalar/dce.hpp"
#include "scalar/ifconv.hpp"
#include "scalar/ldelim.hpp"
#include "scalar/taildup.hpp"
#include "loop/licm.hpp"
#include "loop/unroll.hpp"
#include "loop/lsimp.hpp"
#include "loop/unswitch.hpp"
#include "loop/lcf.hpp"
#include "loop/lcf_rt.hpp"
#include "loop/rotate.hpp"
#include "interprocedural/constcall.hpp"
#include "linear/reassoc.hpp"
#include "linear/divmagic.hpp"
#include "interprocedural/callgraph.hpp"
#include "interprocedural/cdfold.hpp"

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
#ifndef MJZ_DBG_NO_SCCP
      if (cfg.p_sccp) {
        c = opt_sccp(fn, os, cfg.is_main); // conditional + loop-carried consts
        dbg_pass(cfg, fn, "sccp", c);
        ch |= c;
      }
#endif
      c = opt_constprop(fn, os, cfg.is_main); // fold/identities/branch folds
      dbg_pass(cfg, fn, "fold", c);
      ch |= c;
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_COPYPROP
      if (cfg.p_copyprop) {
        c = opt_copyprop(fn, os); // copies -> uses of the source
        dbg_pass(cfg, fn, "copyprop", c);
        ch |= c;
      }
#endif
      normalize_phis(fn);
      if (cfg.p_canon) {
        c = opt_canon(fn, os); // canonical forms + NOT-of-compare
        dbg_pass(cfg, fn, "canon", c);
        ch |= c;
      }
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_CSE
      if (cfg.p_cse) {
        An A = analyze(fn);
        c = opt_cse(fn, A, os); // dominating recomputation -> reuse
        dbg_pass(cfg, fn, "cse", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_GVN
      if (cfg.p_gvn) {
        An A = analyze(fn);
        c = opt_gvn(fn, A, os); // value numbering: consts + phi congruence
        dbg_pass(cfg, fn, "gvn", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (cfg.p_unreach) {
        c = opt_remove_unreachable(fn, os); // dead blocks + phi degeneration
        dbg_pass(cfg, fn, "unreach", c);
        ch |= c;
      }
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_SPLICE
      if (cfg.p_splice) {
        c = opt_splice(fn, os); // collapse single-pred/succ jump chains
        dbg_pass(cfg, fn, "splice", c);
        ch |= c;
      }
#endif
      normalize_phis(fn);
      if (cfg.p_dce) {
        c = opt_dce(fn, os);
        dbg_pass(cfg, fn, "dce", c);
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
  // trapelim and jthread run ONCE per optimize() call and SHARE one range
  // analysis (the caller's): the interval state is the most expensive
  // thing in the tier — re-running it every fixpoint round of every tier
  // re-analysis made pathological inputs ~20x slower for no additional
  // proofs, and a SECOND analysis per call pushed the constexpr proofs'
  // peak memory over the edge. Both passes' facts are monotone; the tier-2
  // post-round re-run catches restructuring-exposed proofs/elbows.
  bool trapelim_ran = false;
  bool jthread_ran = false;
  if (cfg.level >= 2) {
    for (uint32_t outer = 0; outer < cap; ++outer) {
      bool ch = false, c = false;
      if (cfg.p_mem2reg) {
        An A = analyze(fn);
        c = opt_mem2reg(fn, A, os, cfg.lift_ok, cfg.stkarr_ok);
        dbg_pass(cfg, fn, "mem2reg", c);
        ch |= c;
        normalize_phis(fn);
      }
      if (cfg.p_ldelim) {
        An A = analyze(fn);
        c = opt_ldelim(fn, A, os); // fwd/CSE leftover array loads
        dbg_pass(cfg, fn, "ldelim", c);
        ch |= c;
        normalize_phis(fn);
      }
      if (cfg.p_taildup) {
        An A = analyze(fn);
        c = opt_taildup(fn, A, os); // small 2-pred tails into their arms
        dbg_pass(cfg, fn, "taildup", c);
        ch |= c;
        normalize_phis(fn);
      }
      {
        const bool run_te = cfg.p_trapelim && (outer == 0 || !trapelim_ran);
        const bool run_jt = cfg.p_jthread && (outer == 0 || !jthread_ran);
        if (run_te || run_jt) { // one analysis for both passes
          An A = analyze(fn);
          Ranges RA = range_analyze(fn, A);
          if (run_te) {
            c = opt_trapelim(fn, A, os, RA); // ranges + aliasing -> nt
            dbg_pass(cfg, fn, "trapelim", c);
            trapelim_ran = true;
            ch |= c;
            normalize_phis(fn);
          }
          if (run_jt) {
            c = opt_jthread(fn, A, RA, os); // per-edge elbow threading
            dbg_pass(cfg, fn, "jthread", c);
            jthread_ran = true;
            ch |= c;
            normalize_phis(fn);
          }
        }
      }
#ifndef MJZ_DBG_NO_LICM
      if (cfg.p_licm) {
        An A = analyze(fn);
        c = opt_licm(fn, A, os); // hoist invariants to the preheader
        dbg_pass(cfg, fn, "licm", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (cfg.p_lsimp) {
        An A = analyze(fn);
        c = opt_lsimp(fn, A, os); // trivial loop-carried phis -> entry vals
        dbg_pass(cfg, fn, "lsimp", c);
        ch |= c;
        normalize_phis(fn);
      }
#ifndef MJZ_DBG_NO_DIVMAGIC
      if (cfg.p_divmagic) {
        c = opt_divmagic(fn, os); // /constant -> mulhi+shift
        dbg_pass(cfg, fn, "divmagic", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_REASSOC
      if (cfg.p_reassoc) {
        c = opt_reassoc(fn, os); // canonicalize +/-/*c chains (feeds LCF)
        dbg_pass(cfg, fn, "reassoc", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (cfg.p_ifconv) { // provably-binary cbr selects -> arithmetic
        c = opt_ifconv(fn, os);
        dbg_pass(cfg, fn, "ifconv", c);
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
      if (cfg.p_unswitch) {
        An A0 = analyze(fn); // version loops on invariant header conditions
        bool c0 = opt_unswitch(fn, A0, os); // BEFORE unroll: the versioned
        dbg_pass(cfg, fn, "unswitch", c0);  // true-arm loses its exit edge,
        ch |= c0;                           // so unroll/lcf skip it anyway
        normalize_phis(fn);
      }
#ifndef MJZ_DBG_NO_UNROLL
      if (cfg.p_unroll && cfg.unroll) {
        An A = analyze(fn);
        bool c = opt_unroll(fn, A, os, cfg.unroll);
        dbg_pass(cfg, fn, "unroll", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_LCF
      if (cfg.p_lcf) {
        An A2 = analyze(fn);          // unrolling may have reshaped the CFG
        bool c = opt_lcf(fn, A2, os); // closed forms / simulators
        dbg_pass(cfg, fn, "lcf", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (cfg.p_cdfold) { // TCE countdown loops -> exact closed form
        An A3 = analyze(fn);
        bool c = opt_cdfold(fn, A3, os);
        dbg_pass(cfg, fn, "cdfold", c);
        ch |= c;
        normalize_phis(fn);
      }
      if (cfg.p_rotate) {    // test-first -> test-last: LAST loop consumer
        An A4 = analyze(fn); // (unswitch/unroll/lcf need the test-first
        bool c = opt_rotate(fn, A4, os); // shape; the rotated header ends
        dbg_pass(cfg, fn, "rotate", c);  // in Br, invisible to them)
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
        if (cfg.p_ldelim) {
          An A = analyze(fn);
          c2 |= opt_ldelim(fn, A, os);
          normalize_phis(fn);
        }
        if ((cfg.p_trapelim || cfg.p_jthread) && t2round < 2) {
          // restructuring-exposed proofs/elbows; one shared analysis
          An A = analyze(fn);
          Ranges RA = range_analyze(fn, A);
          if (cfg.p_trapelim)
            c2 |= opt_trapelim(fn, A, os, RA);
          if (cfg.p_jthread)
            c2 |= opt_jthread(fn, A, RA, os);
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
