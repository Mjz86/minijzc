#pragma once
// === scctco.hpp ========================================
// SCC tail-call optimization: collapse intra-SCC tail calls into
// loops (helpers, per-member collapse, the driver).
//
// ALGORITHM: opt_scc_tailcall builds the call graph, then per SCC
// (<= 8 members, main excluded, total <= 6000 instrs, every internal
// edge a tail call with matching arg count) snapshots the members
// (snap/snap_of) and runs scctco_member, which prepends D (block 0:
// t0/k1/z0) + H (block 1), drains the internal tail sites one per
// round (iter_cap): a self site becomes a back edge (promote/
// add_backedge), a sibling site inlines the sibling's snapshot once;
// then the 3b fixup rotates raw pass-through params, fn.resucc()
// runs, and scctco_fold sees the pristine shape.
//
// DATA STRUCTURES: ScctcoHead{entry, root_pred, root_vals, phis,
// tip_pred, tip_vals, cb, ce, trip_src, trip_phi, phi_vals}, the
// per-member merge heads (heads[]); trapb (the shared 1/0 trap
// block); t0/k1/z0, D's consts (511/1/0); T_cur + trip_read.
//
// THEORY: a tail call has no continuation -- the caller returns the
// callee's value unchanged -- so swapping [Call;Ret] for a CFG edge
// into the callee's code preserves the function's semantics: the
// same value is produced (a self site feeds the parameter phis, so
// the params become loop-carried induction variables; a sibling
// site's inlined copy rets ARE the caller's rets), while the loop-
// carried trip counter (511 at entry, -1 per edge, trap at <0)
// reproduces the 512-frame TRAP exactly at the 513th virtual call.
// The counter runs DOWN so the guard bounds the phi from both
// sides at once: interval analysis converges in ~2 rounds, not 512.
//
// DOWNSTREAM: scctco_fold turns count-down-by-c loops into guard +
// V0 & (c-1) (is_odd -> n & 1); members stop being recursive, so
// the inliner pulls them in and range analysis deletes the guard.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "callgraph.hpp"
#include "scctco_fold.hpp" // the residue fold (sees scctco_retarget)
#include "scctco_impl.hpp" // the helpers + ScctcoCtx + the phase layer

// ============================================================================
// 9e1b. SCC tail-call optimization ("scctco")
// ============================================================================
// The inliner (9e2) never touches recursive functions; this pass fills that
// gap for TAIL recursion: an SCC of the call graph whose every internal
// edge is a tail call (`return f(..)` -- the call result flows straight out
// and nothing else reads it) is collapsed into intra-function loops:
//
//   fn is_even(n) { if (n == 0) { return 1; } return is_odd(n - 1); }
//   fn is_odd(n)  { if (n == 0) { return 0; } return is_even(n - 1); }
//
// is_odd becomes a single count-down-by-2 loop (is_even loses its only
// caller and dies), which the residue fold below rewrites to `n & 1`.
//
// Construction, per SCC member f (siblings are inlined from PRE-collapse
// snapshots, so every member collapses independently):
//   * a fresh entry D (block 0: trip=0, the cap constant, br H) and loop
//     header H (block 1: parameter phis, inserted at the first back edge)
//     are prepended; every read of a parameter is rewritten to its phi, so
//     the parameters become the loop-carried induction variables
//   * a SELF tail site `return f(args)` becomes a back edge into H feeding
//     the parameter phis; a second/third site merges through a chain of
//     2-pred phi blocks (binary search: both recursive arms -> one loop)
//   * a SIBLING tail site inlines the sibling's snapshot body ONCE (a tail
//     site has no continuation, so the callee's `ret`s simply become the
//     caller's `ret`s); any further edge to the same sibling jumps back to
//     that one copy through parameter phis promoted in its entry block
//   => every SCC edge is exactly one CFG edge: mutual recursion is a
//      trivial loop; self recursion with several arms is one loop with
//      several exits (nothing is duplicated beyond one copy per sibling).
//
// FRAME-PARITY GUARD: the language caps recursion at 512 frames -> TRAP in
// every backend, so each collapsed edge increments a loop-carried trip
// counter (threaded through every merge phi, sibling copies included: a
// copy's sites read the trip ARRIVING at its entry, which is the root
// site's incremented value) and traps past 512: recursion deep enough to
// trap still traps -- exactly at the 513th virtual call when the function
// is entered at frame depth 1 (a deeper entry can differ by the caller's
// frame count; the alternative, no guard, would turn guaranteed traps into
// step-budget timeouts).
//
// RESIDUE FOLD: right after the collapse (while the shape is pristine) a
// loop whose only parameter phi counts down by a constant step c, whose
// body is pure (no Print/Load/Store/Alloc/Len/Input/Call/Div/Mod) and
// whose exits are `V == r` returning the constant r for EVERY residue
// r in 0..c-1, is replaced by
//     if (V0 < 0 || V0 >= 512*c) -> TRAP   (the cap, now a range check)
//     ret V0 & (c-1)                        (c a power of two: strength
//                                            reduction of V0 % c)
// which is what turns is_odd above into `guard + n & 1`. Loops that do not
// match (binary search: two induction variables) simply stay loops -- TCO
// without the closed form. The folded body is tiny, so the plain inliner
// then pulls it into callers and tier-0 range analysis deletes the guard
// whenever V0's interval proves it dead.

// ---- collapse one SCC member (the slim driver; round nine) ------------------
// The support layer -- the tail-call predicate (scctco_is_tail), the
// block-editing quartet (scctco_prepend/set_block/retarget/rewrite,
// the operand leaf st_rw_one), the merge-head record ScctcoHead, the
// per-member context ScctcoCtx, and the phase functions (the D+H
// prepend; promote/add_backedge; the site drain with its tri-state
// exits; the sibling snapshot inline; the pass-through rotation) --
// lives in scctco_impl.hpp, with the observable-order ledger (the
// vreg mint orders) and the byte-identical contract documented
// there. What stays here is the phase sequence itself, top to bottom.

constexpr bool scctco_member(Fn &f, uint32_t self_fn,
                             const std::vector<Fn> &snap,
                             const std::vector<uint32_t> &snap_of, uint32_t nfn,
                             OptStats &os, uint8_t dbg) {
  if (!st_gate_ok(f))
    return false; // <= 6 params + a phi-free entry
  ScctcoCtx C{f, snap, snap_of, nfn, self_fn};
  st_prepend_dh(f, C); // D (block 0: t0/k1/z0) + H (block 1); shift +2
  const st_drain_t d = st_drain_sites(f, C); // one tail site per round
  if (d == st_drain_t::bail_e)
    return false; // a non-tail internal site surfaced mid-drain
  if (d == st_drain_t::capped_e)
    return C.changed;         // the iter bound: remaining sites stay calls
  st_rotate_raw_params(f, C); // 3b: pass-through params read the phis
  f.resucc();
  if (dbg >= 2)
    print_fn(f, "[scctco] collapsed (pre-fold)", {});
  scctco_fold(f, 1, C.nparams, os, C.trapb, dbg);
  f.resucc();
  return C.changed;
}

// runtime-only tracer for the scctco driver (never reached on the
// constexpr path: cfg.debug is 0 there)
static void scctco_dbg_rt(uint8_t dbg, const char *what, long a, long b) {
  if (dbg < 1)
    return;
  std::printf("  [scctco] %s %ld %ld\n", what, a, b);
}

// ---- the SCC driver ---------------------------------------------------------

// Collapse every SCC of the call graph whose internal edges are all tail
// calls into loops; re-optimize each collapsed member (cheap tiers + lcf,
// unroll waits for the final round, exactly like the inliner's re-fold).
constexpr bool opt_scc_tailcall(std::vector<Fn> &fns,
                                std::vector<OptStats> &ostats,
                                const std::vector<std::string_view> &names,
                                PassCfg cfg) {
  const uint32_t n = (uint32_t)fns.size();
  if (n < 2)
    return false;
  CallGraph G = build_callgraph(fns);
  bool any = false;
  for (uint32_t s = 0; s < G.nscc; ++s) {
    const uint32_t b = G.scc_begin[s];
    const uint32_t e =
        (s + 1 < G.nscc) ? G.scc_begin[s + 1] : (uint32_t)G.members.size();
    if (e <= b)
      continue;
    std::vector<uint32_t> mem(G.members.begin() + b, G.members.begin() + e);
    if (mem.size() > 8)
      continue; // small SCCs only
    bool internal = false;
    for (uint32_t f : mem)
      for (ulen c : G.adj[f])
        if ((uint32_t)c < n && G.scc_of[(uint32_t)c] == s)
          internal = true;
    if (!internal)
      continue;
    // preconditions: every internal edge is a tail call with matching arg
    // count; total size bounded; main is never a member
    uint64_t total = 0;
    bool ok = true;
    for (uint32_t f : mem) {
      if (f == 0) {
        ok = false;
        break;
      }
      total += fns[f].code.size();
      for (uint32_t blk = 0; blk < fns[f].nblocks() && ok; ++blk)
        for (uint32_t i = fns[f].blocks[blk].begin; i < fns[f].blocks[blk].end;
             ++i) {
          const Instr &I = fns[f].code[i];
          if (I.k != K::Call)
            continue;
          uint32_t g = (uint32_t)I.c;
          if (g >= n || G.scc_of[g] != s)
            continue; // external edge
          if (!scctco_is_tail(fns[f], blk, i) ||
              I.args.size() != fns[g].nparams) {
            if (cfg.debug >= 1)
              std::printf("  [scctco] non-tail internal edge: f%u blk%u\n",
                          (unsigned)f, (unsigned)blk);
            ok = false;
            break;
          }
        }
    }
    if (!ok || total > 6000) {
      if (cfg.debug >= 1)
        scctco_dbg_rt(cfg.debug, "skip scc (ok,total)", (long)ok, (long)total);
      continue;
    }
    // pre-collapse snapshots + fn -> snapshot map
    std::vector<Fn> snap;
    std::vector<uint32_t> snap_of(n, NONE);
    for (uint32_t f : mem) {
      snap_of[f] = (uint32_t)snap.size();
      snap.push_back(fns[f]);
    }
    for (uint32_t f : mem) {
      // stats land on row 0 (<main>): collapsed members are routinely dead
      // (their only callers were inside the SCC), and eliminate_dead_fns
      // drops their stats rows with them
      if (!scctco_member(fns[f], f, snap, snap_of, n, ostats[0], cfg.debug)) {
        if (cfg.debug >= 1)
          scctco_dbg_rt(cfg.debug, "member refused", (long)f, 0);
        continue;
      }
      ostats[0].scctco++;
      any = true;
      // NOTE: no re-fold here (unlike the inliner's post-inline re-fold).
      // The collapsed body is left for the inliner rounds below (it is no
      // longer recursive, so single-use members inline into their callers,
      // which re-fold THOSE) and for optimize_prog's final full optimize,
      // which cleans every member. Skipping the intermediate full optimize
      // also keeps the constexpr (static_assert) build's retained
      // evaluation state -- two whole analysis+fixpoint rounds per SCC --
      // from bloating the compile-time memory budget.
    }
  }
  return any;
}
