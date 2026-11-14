#pragma once
// === fold.hpp ========================================
// constant folding/propagation, branch folding, algebraic identities.
// This file is the slim driver (the code-quality continuation's
// cdfold/lcf/unroll treatment, round four): the def-index map, the
// per-round known-const tables, the compare-domain facts, the
// algebraic-identity families and the per-kind handlers live in
// fold_impl.hpp; what stays here is the 12-round fixpoint loop
// itself -- the phase list reads top to bottom.
//
// THEORY (local folding, and what it is NOT):
//   This is the LOCAL tier-0 fold: it propagates values a single
//   def-site analysis can see -- every Const def makes its users
//   foldable, with NO control-flow reasoning (that is SCCP's job:
//   the conditional/loop-carried constants). Per round it
//   re-collects known/kval (SSA: one def per vreg, so a Const
//   def binds its value unambiguously) and iterates to a 12-round
//   fixpoint:
//     * Copy of a known const -> Const.
//     * Bin on known consts -> the wrapped value (Const), with
//       the trap discipline of config.hpp (a would-be trap keeps
//       the op: traps are observable).
//     * ALGEBRAIC IDENTITIES -> Copy-of-operand (os.copies) or
//       Const (os.folds): annihilator/identity operand patterns,
//       the two-def identities via the def-index map (0-(0-x)),
//       and the COMPARE-DOMAIN FACTS: a compare result is exactly
//       0/1, which decides (cmp)&1, (cmp)%2 (no trap), (cmp)>>k,
//       (cmp)^1 = NOT cmp -- cmp_def/inv_cmp in fold_impl.hpp.
//     * K::Depth in main -> Const 1 (main's frame count).
//     * BRANCH FOLDING: Cbr with a known cond -> Br toward the
//       taken arm (os.brs_folded); identical arms -> Br
//       (os.cbr_same); the dead arm becomes unreachable code for
//       the unreach pass.
//   Everything here is semantics-preserving by the one-def-per-
//   vreg SSA property: a rewrite of the def rewrites the value
//   every dominated use sees.
//
//   THE FIXPOINT AND os.iters: the exact round structure -- table
//   rebuild, full scan, quiet-round exit, the 12 cap -- is part of
//   the observable behavior: this pass's return value drives the
//   pipeline driver's fixpoint (driver.hpp counts os.iters), so
//   the driver below is preserved to the character (the full
//   argument: fold_impl.hpp's header).

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "fold_impl.hpp" // FoldCtx + the per-phase helpers

// ============================================================================
// 9. optimizer (all on SSA; analyses from the lib are recomputed on demand)
// ============================================================================

// --- 9a. constant folding / propagation / branch folding / identities -------
constexpr bool opt_constprop(Fn &fn, OptStats &os, bool is_main = false) {
  FoldCtx S = fold_ctx(fn, os); // def-index once + the per-round tables
  bool changed_any = false;
  for (int round = 0; round < 12; ++round) { // iterate to fixpoint
    if (!fold_round(S, is_main))
      break; // a quiet round: nothing this pass can see is left
    changed_any = true;
  }
  return changed_any;
}

// --- 9a2. copy propagation
// ----------------------------------------------------
