#pragma once
// === rotate.hpp ========================================
// loop rotation proper (README wishlist: "loop rotation" — the latch merge
// taildup deliberately left alone, checkpoint 20). The canonical test-first
// loop becomes test-last with a one-shot guard:
//
//   pre:  ... br h              pre:  ... [h-body w/ entry args]
//                                          cbr(guard) h/X      <- the guard
//   h:    [phis (pre,latch)]    h:    [phis (pre,latch) UNCHANGED]
//          [h-body]                    [+ phi_v(pre: guard_v,
//          cbr(cond) body/X               latch: latch_v) per h-body
//                                      def the loop reads]
//                                     br body
//   body: ... br latch          body: ... br latch   (moved-def reads
//                                                        -> phi_v)
//   latch: ... br h             latch: ... [h-body w/ back-edge args]
//                                          cbr(g') h/X         <- the
//   X:    (preds {h}) ...                              edge test
//                                X:    [xphi(pre: entry value,
//                                         latch: back-edge value) per
//                                         after-loop read] ...
//
// The header phis keep their exact keys AND args — pre still enters h and
// the latch still branches back, so nothing re-keys. The whole non-phi
// h-body MOVES (never copies-in-place): every side effect keeps its exact
// count and order — k trips means guard 1 + latch k = the original k+1
// header runs, so Prints, Inputs, Calls, Div/Mod traps and Bkpt
// observations all fire k+1 times in both forms (the guard copy IS the 1st
// header run; the latch copy of iteration i IS the (i+1)-th; nothing sits
// between a latch end and the next header top). Per-iteration VALUE
// consumers read the moved defs through fresh header phis carrying
// (guard copy, latch copy) — exactly the (pre, latch) pattern the original
// phis use — and the after-loop region reads them through fresh exit phis
// carrying (entry value, back-edge value) — the same pair each header phi
// merges, relocated past the loop (the exit phi of an h-phi v literally
// has v's (pre_arg, latch_arg) pair).
//
// The rotation runs LAST among the loop consumers (tier 2, after cdfold):
// unswitch / unroll / lcf all require the test-first shape, so they get
// their chance first and the rotated loop (header ends in Br) is simply
// invisible to them afterwards — one loop per call, the fixpoint driver
// re-analyzes.
//
// Soundness envelope (the strict unroll/lcf family):
//   * the canonical shape (loop_shape_of): unique pre + latch, header Cbr
//     with one arm in (body) and one arm out (X); latch != h (no
//     self-loop).
//   * pre ends in a DEDICATED `br h` (an unconditional jump).
//   * the ONLY exit edge is the header's (exit_inventory empty: no
//     `break`s, no inner exits) and preds(X) == {h} exactly.
//   * all of h's non-phi instrs move together, h-body <= ROT_BODY_MAX
//     (it duplicates into the guard AND the latch — 2x growth cap).
//   * outside uses of loop-defined values: only header phis and h-body
//     defs (the HDEF set — the only values that exist on a 0-trip path)
//     and only in F (the region reachable from X loop-free); anything
//     else bails (SSA dominance forbids it — belt and braces).
//   * the guard reads only values that dominate pre (h-phis' pre_args,
//     defs dominating h which necessarily dominate pre, and the guard's
//     own earlier copies) — the checkpoint-20 lcf-rt lesson (a guard
//     reading an in-loop def, use-before-def) is structurally impossible
//     here: every h-body read is an h-phi, an earlier h-body def, or a
//     def that dominates the preheader.
//
// This file is the slim driver (the code-quality continuation, round
// eight -- the cdfold/lcf/unroll/fold/unswitch_bif/taildup/mem2reg
// treatment): the per-loop context (the shape tables, the def/read
// classification, the commit's value maps), the candidate gates, the
// read classification and the commit tail (the fresh-vreg mint order,
// the code-vector rebuild's per-block arms, the successor rewire) live
// in rotate_impl.hpp -- the phase list, the observable-order ledger and
// the byte-identical contract are documented there; what stays here is
// the phase sequence itself, top to bottom.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "loop_shape.hpp"
#include "rotate_impl.hpp"    // RotCtx + the per-phase support layer
#include "unswitch_shape.hpp" // exit_inventory

constexpr bool opt_rotate(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const LoopScan S = loop_scan(fn, A);
  if (!S.nv)
    return false;
  for (uint32_t h : S.headers) {
    RotCtx R{fn, A, S, h};
    rot_prepare(fn, R);               // inL: the loop membership flags
    if (!rot_shape_ok(fn, R) ||       // the canonical shape + the
                                      // dedicated preheader
        !rot_exit_ok(fn, R) ||        // the only-exit + X-single-pred
                                      // + X-phi-free gates
        !rot_header_phis_ok(fn, R) || // h's phis keyed (pre, latch)
        !rot_collect_hbody(fn, R))    // the saved terminator + the
                                      // non-phi body + the cap
      continue;
    rot_mark_def_sets(fn, R);       // the HDEF set + loopdef
    rot_after_region(fn, R);        // F: reachable from X loop-free
    if (!rot_classify_reads(fn, R)) // loop_read / f_read + the
                                    // outside-use safety scan
      continue;
    // ================= commit =================
    rot_commit(fn, R, os); // vregs in order, the rebuild, the rewire
    return true;           // one rotation per call: the driver
                           // re-analyzes
  }
  return false;
}
