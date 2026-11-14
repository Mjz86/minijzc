#pragma once
// === unswitch_bif.hpp ========================================
// BODY-IF unswitching (checkpoint 19, README wishlist task 3's "the
// natural next step"): the classic LLVM "trivial" unswitch of an
// invariant INNER branch. The loop's header test varies per iteration
// (a real while-condition), but a body branch bif: cbr(INV) A/B with
// INV SSA-proven loop-invariant, both arms in-loop and each entered
// only through bif, is versioned instead:
//     pre: br h   ->   pre: cbr(INV) h / h'
//     the original loop  = the INV-true  version: bif: br A (B dies)
//     the full clone     = the INV-false version: bif': br B' (A' dies)
// BOTH versions keep running the loop (the header test stays live in
// each) — sound because the original picked the SAME arm every
// iteration: INV is an SSA value, it cannot change mid-loop.
//
// Every exit edge (the header's exit arm INCLUDED — no zero-iteration
// edge is replaced here, both versions run the loop) gets a
// VERSION-MERGE block VM: preds {exit source, its clone}, one phi per
// loop-defined value read at or through the exit target —
// phi(v @ source, fresh v @ clone) — then br to the target. That
// holds the <=2-preds invariant the 2-arg phi representation needs:
// the target's preds become its original outside preds + one VM per
// exit edge, and it had at most two preds to begin with. The dead
// arms (B in the true version, A' in the clone) and their merge phis
// degenerate under unreach + copyprop in the same tier round, the
// way the checkpoint-17/18 abandoned loop bodies do.
//
// Outside reads of loop-defined values are routed to their merged
// value by the DELIVERED walk: the value that leaves the loop on some
// exit edge is merged at that edge's VM, and reads downstream get it
// by following their preds back to the VMs:
//   * a PURE ARM (an outside block whose only pred is an in-loop exit
//     source: break arms, a single-exit loop's target X) reads the
//     VM's phi directly — every path there passes that one VM;
//   * a single-pred chain off one of those passes the value up;
//   * a MERGE (2 preds: e.g. X entered by the header exit AND a break
//     arm) whose values arrive per-pred gets a fresh phi at the merge
//     itself — one arg per post-pred (the VM for an in-loop pred, the
//     recursively delivered value for an outside pred) — the SSA
//     builder left these as PLAIN reads (the header phi dominates the
//     whole post-loop region, so no phi existed), and versioning
//     breaks that dominance, so the phi is born here.
// Every value the walk touches is collected FIRST: a read at (or
// keyed to) block b collects v into vm_vals[t] for every exit target
// t that reaches b through outside blocks — the reverse-reachability
// closure, so every VM on every path carries every value it must. The
// walk itself is validated (cycle-guarded, sources checked) BEFORE
// the commit's first edit: a bail leaves the fn untouched.
//
// This file is the slim driver (the code-quality continuation's
// cdfold/lcf/unroll/fold treatment, round five): the gates, the
// outside-use scan, the routes validator and the whole commit tail
// (clone, version rewrites, VM blocks, retargets, guard, delivered
// walk, rewire) live in unswitch_bif_impl.hpp — the phase list, the
// BifCtx data-structure inventory and the delivered-walk theory
// (validate-then-edit) are documented there; what stays here is the
// phase sequence itself, top to bottom.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/phi_norm.hpp"
#include "../analysis/an.hpp"
#include "loop_shape.hpp"
#include "unswitch_shape.hpp"
#include "unswitch_bif_impl.hpp" // BifCtx + the per-phase support layer

constexpr bool unswitch_body_if(Fn &fn, const An &A, OptStats &os,
                                const LoopScan &S, const std::vector<char> &inL,
                                uint32_t h, const LoopShape &sh,
                                const BifShape &bs) {
  BifCtx X{fn, S, inL, bs, h, sh.pre, S.nb, S.nv};
  if (!bif_preheader_ok(fn, sh.pre, h))
    return false; // the preheader must be a dedicated `br h' block
  if (!bif_growth_ok(fn, inL, S.nb))
    return false; // growth cap: 1..120 in-loop instrs
  if (!exit_inventory(fn, inL, S.nb, NONE, NONE, sh.pre, h, X.exits))
    return false;      // primary NONE: the header's exit arm is a regular exit
  if (X.exits.empty()) // sh.ok guarantees the header's exit arm exists
    return false;
  bif_mark_loopdefs(X); // loopdef: v defined inside the loop
  bif_mark_arms(X);     // arm_edge: pure arms -> their in-loop pred
  if (!bif_scan_outside_uses(X))
    return false; // vm_vals + reads collected; a stray read bails
  if (!bif_routes_all_ok(X))
    return false; // validated BEFORE the commit's first edit
  bif_commit(X, os);
  return true; // one loop per call: the driver re-analyzes
}
