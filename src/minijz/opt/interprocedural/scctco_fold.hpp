#pragma once
// === scctco_fold.hpp ========================================
// the scctco residue fold (countdown-by-c -> n & (c-1) etc.).
//
// ALGORITHM: scctco_fold(f, H, nparams, ...) runs right after the
// collapse, on the pristine shape: collect H's phis (the LAST is
// the trip; vphis[0] = V); split preds[H] into root (D) + latch;
// scctco_affine proves the latch value Vl is V - c (0 < c <= 8, a
// power of two); reach/back mark the loop proper, which must be
// pure; every exit cbr arm out of the back region must be [consts,
// ret const] under cond Eq(affine(V), const) -- residue r = konst
// - delta -- and exit_val[r] == r for EVERY r in 0..c-1; the exact
// trap threshold T is found by simulate() (binary search over n,
// monotone); emit appends guard (V0 < 0 || V0 >= T -> trapb) + the
// fold block (ret V0 & (c-1); c == 1: ret 0), and scctco_retarget
// points D (br H) at the guard (FOLD).
//
// DATA STRUCTURES: ScctcoAff{ok, is_const, delta, konst} (resolved
// by scctco_affine); exit_val[c] (residue r -> returned constant);
// def_of/reach/back; simulate (val/setv/read); z0/KC/g1/g2/gc, FOLD.
//
// THEORY: after N = floor(V0/c) trips the phi holds V0 - N*c, so
// the exit for residue r fires iff V0 mod c == r; every r in 0..c-1
// returns the constant r, so the loop computes V0 mod c -- and for
// power-of-two c that is exactly V0 & (c-1), the strength reduction
// of V0 % c. The guard must trap on precisely the inputs the trip
// counter would have: the body is pure and every run trips within
// 512 edges, so the binary-searched simulation finds the exact
// first-trapping T, and `V0 < 0 || V0 >= T` jumps to the SAME trap
// the collapse built; unmatched shapes keep their loop.
//
// DOWNSTREAM: the folded body is a few instrs, so the plain inliner
// pulls it into callers and range analysis deletes dead guards.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "callgraph.hpp"
#include "scctco_fold_impl.hpp" // ScctcoAff + SfCtx + the phase layer

// ---- the residue fold (the slim driver; round nine) -------------------------
// The support layer -- the affine resolver (ScctcoAff +
// scctco_affine), the fold context SfCtx, and the phase functions
// (the phi collection, the latch split, the carried pair, the def
// map, the step gate, the region marks, the purity gate, the exit
// scan with its one SILENT bail, the threshold simulation + binary
// search, the emission) -- lives in scctco_fold_impl.hpp, with the
// bail-print ledger (the dbg-gated message strings, observable under
// --debug) and the byte-identical contract documented there. What
// stays here is the phase sequence itself, top to bottom; every
// false return below pairs with that phase's own bail print.

// Recognize the count-down residue loop on the collapsed member and fold
// it to  cap-guard + V0 & (c-1).  H is the loop header (block 1).
constexpr void scctco_fold(Fn &f, uint32_t H, uint32_t nparams, OptStats &os,
                           uint32_t trapb, uint8_t dbg) {
  SfCtx C{f, H, nparams, trapb, os, dbg};
  if (!sf_collect_phis(f, C)) // [V, trip] exactly; nparams == 1
    return;
  if (!sf_latch_ok(f, C)) // root = D (block 0) + the one latch
    return;
  if (!sf_carried_vals(f, C)) // V0 (root edge) + Vl (latch edge)
    return;
  sf_build_def_of(f, C); // dst -> instr index (the affine input)
  if (!sf_step_ok(f, C)) // Vl = V - c; 0 < c <= 8; c a power of two
    return;
  sf_mark_regions(f, C);   // reach (fwd from H) + back (rev, gated)
  if (!sf_loop_pure(f, C)) // no side effects in the loop proper
    return;
  if (!sf_scan_exits(f, C)) // [consts, ret const] exits; v_r == r
    return;
  if (!sf_threshold(f, C)) // the exact first-trapping T (binary search)
    return;
  sf_emit(f, C); // guard + ret V0 & (c-1); D retargeted; the stat
}
