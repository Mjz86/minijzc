#pragma once
// === unswitch.hpp ========================================
// loop unswitching (README wishlist: "loop unswitching/versioning") —
// the driver. Three shapes, one `unswitch` toggle (#30, tier 2, before
// unroll so the versioned true arm loses its exit edge and unroll/lcf
// skip it anyway):
//   TRIVIAL (checkpoint 17): a loop whose HEADER condition is
//   SSA-proven loop-invariant (defined outside, dominating the
//   preheader) is versioned on it —
//       pre: br h          ->   pre: cbr(cond) --> H' (true) / X (false)
//       h:   cbr(cond) body/X      H':  br body'   (the test is known true)
//   The false path jumps straight to the header's exit target (zero
//   iterations); the true path is a full fresh-vreg clone with the
//   test gone. Internal `break` edges are kept: every exit block's
//   phis are rekeyed from the original in-loop pred to its clone, and
//   the primary (zero-iteration) edge's args become the false-side
//   pred carrying each header phi's ENTRY value.
//   COMPOUND (checkpoint 18): `while (a && b)` / `while (a || b)`
//   spreads the test over a diamond (header a, elbow b, TT/FF, the
//   merge whose phi+cbr IS the exit edge). When BOTH leaf tests are
//   invariant the whole guard chain is versioned as one; the true
//   clone bypasses the diamond straight into the body and the merge's
//   exit edge becomes the zero-iteration edge. The diamond blocks must
//   be pure (Const/Copy/Bin, nt on Div/Mod): the false path skips them.
//   BODY-IF (checkpoint 19): the header test is a REAL while-condition
//   but a body branch on an invariant condition versions the loop —
//   the classic LLVM "trivial unswitch"; BOTH versions keep running
//   the loop and every exit (the header's arm included) gets a
//   version-merge block. The scan + commit live in unswitch_bif.hpp
//   (the slim driver) + unswitch_bif_impl.hpp (the support layer);
//   the shape matchers and the clone machinery all three paths share
//   live in unswitch_shape.hpp.
// Outside uses for the trivial/compound paths are classified by
// false-path reachability F (blocks reachable from X without entering
// the loop): a header phi read there must see its preheader value, so
// a merge phi is inserted at X; any other loop-defined value read there
// cannot exist (SSA dominance) and bails the loop. Reads outside F are
// only reachable after the loop ran (the clone is the only live
// version): they are remapped to the clones. Ret/Exit inside the clone
// keep their (remapped) operands; Bkpt observations clone with it, so
// observation parity is preserved (the false path executes zero body
// instructions, exactly like the original with the condition false).
// ---- ALGORITHM --------------------------------------------------------
// Per header h of S.headers: in_loop_flags; path selection via
// loop_shape_of (header test NOT invariant -> unswitch_bif's
// body-if path); trivial (primary = h, bypass = sh.body) or
// compound (compound_shape_of: primary = mm). Gates: cond_invariant
// per guard; dedicated `br h' preheader; growth cap 1..120;
// exit_inventory; header_phis; region F (worklist from X); the
// outside-use safety scan. Commit: guards g2/fx/t1 first; then
// alloc_clone + emit_loop_clone; pre becomes the guard cbr; the
// merge phis at X (phi_final); the rewire sweep; recompute_preds.
// ---- DATA STRUCTURES --------------------------------------------------
// S (LoopScan defi/defb/kc/kv), inL, sh/cs/bs, guard1/guard2, exits
// (ExitEdge), hphis (HPhi), F, loopdef, read_on_false/phi_final,
// the CloneMap C (bmap/fresh), entry_pred/false_pred, x_side_pred.
// ---- THEORY: the versioning argument ----------------------------------
// cond_invariant proves the guard an SSA value defined outside, not
// a constant, dominating the preheader: an SSA value cannot change
// mid-loop, so the original resolved the header test the SAME way
// every iteration -- duplicating the loop under each arm of that
// invariant branch is exact. The false arm runs zero body
// instructions (Bkpt observations clone with it: parity); the true
// arm is a fresh-vreg clone with the test gone. Outside uses: a
// header phi read in F takes its preheader value (merge phi at X:
// pre_arg zero-iteration side, C.fresh after the loop ran); a
// non-phi loop value read in F is impossible under SSA dominance
// (bail); reads outside F happen only after the loop ran, remapping.
// ---- WHAT IT ENABLES --------------------------------------------------
// Tier 2 runs it BEFORE unroll: the versioned true arm loses its
// exit edge, so unroll/lcf skip it anyway; the abandoned side dies
// under unreach + copyprop in the same tier round.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/phi_norm.hpp"
#include "../analysis/an.hpp"
#include "loop_shape.hpp"
#include "unswitch_bif.hpp"
#include "unswitch_impl.hpp"
#include "unswitch_shape.hpp"

constexpr bool opt_unswitch(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const LoopScan S = loop_scan(fn, A);
  if (!S.nv)
    return false;
  for (uint32_t h : S.headers) {
    UsCtx U{};
    in_loop_flags(fn, A, h, U.inL);
    // ---- path selection ---------------------------------------------
    const LoopShape sh = loop_shape_of(fn, A, U.inL, h);
    if (sh.ok && !cond_invariant(fn, A, U.inL, S, sh.pre,
                                 fn.code[fn.blocks[h].end - 1].a)) {
      // the header test varies per iteration -- but an invariant INNER
      // branch still versions the loop (both versions keep running)
      const BifShape bs =
          body_if_shape_of(fn, A, U.inL, S, h, sh.pre, sh.latch);
      if (bs.ok && unswitch_body_if(fn, A, os, S, U.inL, h, sh, bs))
        return true; // one loop per call: the driver re-analyzes
      continue;      // not trivial (cond varies), not body-if: skip
    }
    // ---- the shape pick + the shared gates (impl header) ------------
    if (!us_shape_ok(fn, A, S, h, sh, U))
      continue;
    // ---- exit inventory + header phis --------------------------------
    if (!us_exits_ok(fn, S, h, U))
      continue;
    // ---- the false-path region F + loopdef ---------------------------
    us_false_region(fn, S, U);
    // ---- the outside-use safety scan ---------------------------------
    if (!us_outside_uses_ok(fn, S, U))
      continue;
    // ================= commit =================
    os.unswitches++;
    us_guard_blocks(fn, U); // BEFORE the clone map: stable clone ids
    const CloneMap C = alloc_clone(fn, S, U.inL);
    emit_loop_clone(fn, S, U.inL, C, h, U.pre, U.entry_pred, U.bypass);
    us_emit_guards(fn, U, C, h);
    us_merge_phis(fn, U, C);
    us_rewire(fn, S, U, C);
    fn.recompute_preds();
    return true; // one loop per call: the driver re-analyzes
  }
  return false;
}
