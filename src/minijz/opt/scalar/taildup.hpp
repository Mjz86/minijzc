#pragma once
// === taildup.hpp ========================================
// tail duplication (README wishlist: "Tail Duplication") — the degenerate
// guarded unswitch of the checkpoint-19 notes. A SMALL block T shared by
// exactly two predecessors (the classic post-if merge inside a loop body,
// or any 2-pred join) is duplicated into both of them, so each arm of the
// branch that feeds T falls straight through to the shared continuation:
//
//     A: ... br T          A: ... [T-body with A's phi args] <T's term>
//     B: ... br T    ==>   B: ... [T-body with B's phi args] <T's term>
//     T: [phis] body       (T becomes unreachable; unreach removes it)
//         br U
//
// The merge block AND its jump disappear from every path; T's phis do not
// duplicate — they MOVE (each continuation U of T gets one phi per escaping
// value, keyed to the two arms), and the per-arm copies of T's arithmetic
// see each arm's local state, so tier-0 folding can pre-resolve them (the
// correlated-merge case: `if (c) x=1 else x=9; y=x+5` duplicates y=x+5 into
// both arms, where it folds to 6 and 14 — the merge then selects between
// two constants). In-loop merges work too: the merge's value feeding the
// loop header's phi through the post-arm join gets a fresh phi BORN at the
// join (the checkpoint-19 delivered-walk discipline, one nesting level per
// re-merge).
//
// Soundness envelope:
//   * preds(T) == {P0, P1}, each pred ending in a DEDICATED `br T`
//     (an unconditional jump — conditional preds would need edge blocks
//     this IR does not have; they stay unduplicated).
//   * T is not a loop header: a header's duplication into its own latch
//     arms is loop rotation proper — landed in checkpoint 22
//     (opt/loop/rotate.hpp), which runs LAST among the loop consumers
//     precisely so this tree's test-first consumers get their chance
//     first.
//   * T's body is <= TAILDUP_BODY_MAX instrs of Const/Copy/Bin/Print (a
//     may-trap Div/Mod is fine — the trap ran once per path before and
//     its copy runs once per path after; a Print printed once per path
//     before and its copy prints once after).
//   * T's terminator is Br/Cbr (a continuation to re-key) or Exit/Ret
//     (the fn tail duplicated into both arms — no routing). For Br/Cbr,
//     every successor U must have preds == {T} exactly: after the
//     duplication U's preds are exactly {P0, P1}, holding the <=2-preds
//     invariant the 2-arg phi representation needs. (A merge whose
//     continuation is the loop HEADER stays unduplicated: the header
//     keeps its {pre, latch} preds, the merge is the latch — deleting it
//     would only re-create it as the join. The survivors of tier-0's
//     splice are exactly the fn tails, the cbr continuations — a
//     following if — and those.)
//   * values DEFINED in T (its phis and body dsts) that escape T are read
//     only where a supplier phi dominates the reader: the continuation
//     U0/U1's routing phi dominates its region; a 2-pred re-merge whose
//     arms arrive from different suppliers gets a phi BORN there (one
//     arg per pred, each arg the pred's supplier); single-pred chains
//     pass their supplier down. Anything else (a join of >2 preds, a
//     cycle among unresolved blocks) bails the candidate.
// One duplication per call; the fixpoint driver re-analyzes.
//
// This file is the slim driver (the code-quality continuation's
// cdfold/lcf/unroll/fold/unswitch_bif treatment, round six): the
// candidate gates, the routing fixpoint, the outside-use scan and the
// whole commit tail (the fresh per-copy vregs, the routing phis, the
// born phis, the two copies, the rewire) live in taildup_impl.hpp —
// the phase list, the TdCtx data-structure inventory, the placement
// theory (the route fixpoint is a mini SSA construction) and the
// byte-identical contract are documented there; what stays here is
// the phase sequence itself, top to bottom.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "taildup_impl.hpp" // TdCtx + the per-phase support layer

constexpr bool opt_taildup(Fn &fn, const An &A, OptStats &os) {
  TdCtx X{fn, A};
  if (!td_scan(fn, A, X))
    return false; // stale analysis / empty fn / no vregs
  for (uint32_t T = 0; T < X.nb; ++T) {
    X.T = T;
    if (!td_preds_ok(fn, A, X))
      continue; // not a 2-dedicated-pred join: keep it
    if (!td_content_ok(fn, X))
      continue; // phis/body/term outside the envelope
    if (!td_continuations(X))
      continue; // the term's target is out of range
    if (!td_succs_ok(fn, X))
      continue; // a continuation is not T-private / opens with a phi
    td_escape_inventory(fn, X); // defv + the per-copy slots
    td_route_seed(A, X);        // the dominance seeding
    td_route_fixpoint(fn, X);   // chains, merges, the born sites
    if (!td_outside_uses_ok(fn, X))
      continue; // a T-defined value read where no supplier phi may live
    td_commit(fn, X, os); // validated: the commit never fails
    return true;          // one duplication per call: the driver re-analyzes
  }
  return false;
}
