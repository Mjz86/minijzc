#pragma once
// === lcf.hpp ========================================
// closed-form loop evaluation: affine recurrences -> series, exact
// compile-time simulation of non-affine bodies. This file is the slim
// driver (the code-quality continuation's cdfold treatment): scan the
// fn once, then walk the headers nest-first and try, per loop --
//   shape (edges/exit/arms/hphis/induction)  [lcf_impl.hpp]
//   no induction? the general simulator      [lcf_sim.hpp]
//   plan: bound/trip gates, safety, analysis, runtime versioning
//   emit: closed-form series or N-iteration simulation + rewire
// One loop per call; the fixpoint driver re-analyzes (inner loops
// fold first, enabling their parents). The heavyweight sub-algorithms
// were extracted at checkpoints 16/18 (lcf_sim.hpp, lcf_rt.hpp); the
// phase matchers, gates and emitters live in lcf_impl.hpp.
// ---- THE FAMILY --------------------------------------------------------
// loop_shape.hpp: the shared vocabulary (LoopScan, inL, HPhi, the
// canonical shape). lcf_impl.hpp: this pass's own scan + the
// per-phase matchers (lcf_shape), the plan gates (lcf_plan) and the
// emitters (lcf_emit_and_rewire). lcf_sim.hpp: lcf_sim_fold + the
// budgets. lcf_rt.hpp: lcf_rt_fold + LcfEvo.
// ---- THEORY -----------------------------------------------------------
// Every rewrite is EXACT: the series algebra is computed mod 2^64
// (wrapping is fine); the simulator runs the program's own trapping
// arithmetic and aborts on trap; runtime versioning fires the closed
// form only under a guard that proves exactness, else the original
// loop stays. One loop per call, headers nest-first: the fixpoint
// driver re-analyzes, so inner loops fold first, enabling parents.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "lcf_impl.hpp" // LcfScan/LcfLoop + the per-phase matchers

// --- 9d5. loop closed-form evaluation (constant trip counts) -----------------
// A counted loop with a constant trip count N whose body is pure scalar math
// (no memory, calls, prints, or inner control flow) does not need to run at
// all: every loop-carried variable x evolves as an AFFINE RECURRENCE
//     x_{k+1} = a*x_k + b*i_k + g      (i = the induction, i_k = i0 + d*k)
// whose closed form is a pair of compile-time constants (K1, K0):
//     x_N = K1 * x0 + K0,
//     K1 = a^N,  K0 = (C + B*(N-1))*S - B*T,  C = b*i0 + g,  B = b*d,
//     S(n) = sum_{j<n} a^j (geometric series),  T(n) = sum_{j<n} j*a^j
// — clang-style additive (a=1) AND geometric (a!=1) series, computed EXACTLY
// mod 2^64 by bit-doubling (no modular inverses: wrapping loops are fine).
// The loop is replaced by `x = K1*x0 + K0` in the preheader (x0 may be a
// runtime value!). Bodies that are not affine (x*x, mutual dependencies)
// fall back to exact compile-time SIMULATION of the N iterations (traps
// abort the rewrite and keep the loop). (LCF_TRIP_BUDGET / LCF_SIM_BUDGET
// live in lcf_sim.hpp: the simulator and the trip counters share them.)
constexpr bool opt_lcf(Fn &fn, const An &A, OptStats &os) {
  LcfScan S;
  if (!lcf_scan(fn, A, S))
    return false;
  for (uint32_t h : S.headers) {
    LcfLoop L;
    if (!lcf_shape(fn, A, S, h, L))
      continue;          // not a foldable loop shape: keep the loop
    if (L.ind == NONE) { // no counted induction: the general simulator
      if (lcf_sim_call(fn, S, L, os))
        return true;
      continue; // not general-simulable: try nothing else for this loop
    }
    switch (lcf_plan(fn, S, L, os)) {
    case LcfGo::Folded:
      return true; // zero-trip or runtime versioning folded it
    case LcfGo::Skip:
      continue; // a gate failed (or the sim trapped): keep the loop
    case LcfGo::Emit:
      break; // constant trip: emit the closed form / the simulation
    }
    if (lcf_emit_and_rewire(fn, S, L, os))
      return true; // one loop per call: the driver re-analyzes
  }
  return false;
}

// ============================================================================
