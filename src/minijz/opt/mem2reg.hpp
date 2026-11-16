#pragma once
// === mem2reg.hpp ========================================
// mem2reg: array memory -> SSA registers + dead-store/dead-alloc
// elimination for never-read arrays.
//
// An `array(L)` with constant L whose every access is through a handle
// that provably names ONLY that allocation, with a constant in-range
// index, does not need memory at all: slot j of the array becomes a
// pseudo-variable (init: Const 0 right after the kept Alloc; store: a
// versioned def; load: a use of the reaching version), phis placed at
// the iterated dominance frontier of each slot's def blocks and
// renaming by dominator walk -- full SSA, so every later pass sees
// plain registers. An array that is never READ is dead regardless of
// index shape: its stores are deleted and Len(h) folds to L.
//
// PIPELINE POSITION. Runs early (the scalar/loop passes then see plain
// registers, not arena traffic); gated by the toggles (lift_ok = the
// mem2reg toggle, delalloc_ok) and the program-wide stkarr_ok gate
// from prog_handle_gates (deleting an allocation shifts every later
// arena handle -- observable through handle VALUES and Len-of-obscure
// -- so under that gate dead allocs are pure weight). Bkpt-observed
// arrays stay in the arena (the debugger must be able to dump their
// contents).
//
// This file is the slim driver (the code-quality continuation's
// cdfold/lcf/unroll/fold/unswitch_bif/taildup treatment, round
// seven): the pass context (the alias tables, the classification,
// the slot machinery, the renaming state), the classification gates,
// the alias-driven pre-checks, the reflow rebuild and the table
// replay live in mem2reg_impl.hpp -- the phase list, the 9d4 slot
// theory, the stale-loop-array hazard and the byte-identical
// contract are documented there; what stays here is the phase
// sequence itself, top to bottom. The SSA core is the generic kernel
// (ir/ssa_generic.hpp): placement, renaming and the rebuild's layout
// math are index-space; this pass contributes only the IR knowledge.
#include "../base/pass_cfg.hpp"
#include "../ir/instr.hpp"
#include "../ir/ssa_generic.hpp"
#include "analysis/alias.hpp"
#include "analysis/an.hpp"
#include "mem2reg_impl.hpp" // M2RCtx + the per-phase support layer

constexpr bool opt_mem2reg(Fn &fn, const An &A, OptStats &os, bool lift_ok,
                           bool delalloc_ok) {
  M2RCtx X{fn, A, lift_ok, delalloc_ok};
  if (!m2r_scan(fn, X))
    return false; // empty fn / no vregs / no allocations
  if (!X.lift_ok)
    return m2r_sweep_dead_allocs(fn, X, os); // lifting off: sweep only
  m2r_key_consts(fn, X);   // the sole-def Const index keys
  m2r_classify(fn, X);     // liftable / dead / neither, the budget spent
  if (!X.any_lift && !X.any_dead)
    return m2r_sweep_dead_allocs(fn, X, os); // nothing classifiable
  m2r_check_dominance(fn, X); // the defensive pre-check, any_lift demoted
  if (!X.any_lift && !X.any_dead)
    return m2r_sweep_dead_allocs(fn, X, os);
  m2r_slot_tables(fn, X); // slot_base + the per-slot def blocks
  m2r_place(fn, X);       // the kernel's phi placement, ordinal offsets
  m2r_rebuild(fn, X, os); // slot phis + init consts + dead-store drop
  m2r_gather_events(fn, X); // the Load/Store/init event stream
  if (!m2r_rename(X))
    return false; // an unrouted load: the rebuild stands (the
                  // documented asymmetry -- nothing else mutated)
  m2r_replay(fn, X); // the walk's vreg mints + the rewrites
  (void)m2r_sweep_dead_allocs(fn, X, os); // post-rewrite: the lifted
                                          // handles are now unused
  return true;
}
