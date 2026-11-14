#pragma once
// === taildup_impl.hpp ========================================
// the TAIL-DUPLICATION SUPPORT layer, split out of taildup.hpp (the
// code-quality continuation, round six -- the cdfold/lcf/unroll/
// fold/unswitch_bif treatment): the pass context (the scan tables +
// the commit state the former 419-line monolith held in locals), the
// candidate gates (the two dedicated preds, the content envelope,
// the private continuations), the routing fixpoint (route[] + the
// born sites), the outside-use scan, and the commit tail (the fresh
// per-copy vregs, the continuations' routing phis, the born phis at
// the re-merges, the two substituted copies, the rewire sweep).
// The slim driver -- the phase list -- stays in taildup.hpp.
//
// ALGORITHM (the phase list; every order below is the original's):
//   scan    the pass-entry guards: the analysis is fresh, the fn has
//           blocks and a nonempty vreg universe (nb/nv recorded).
//   preds   per candidate T: preds(T) == {P0, P1}, distinct,
//           in range, neither T; T not a loop header (that is
//           rotation's, rotate.hpp runs last); both preds end in a
//           DEDICATED `br T' (td_dedicated_br: Br to T, succ0 == T,
//           succ1 == NONE -- a conditional pred would need edge
//           blocks this IR does not have).
//   content T's phis collected keyed (P0,P1) (td_phi_arg: the
//           t-slot or f-slot arg; both keys present, both in range);
//           the body <= TAILDUP_BODY_MAX of Const/Copy/Bin/Print
//           (a may-trap Div/Mod never reaches the body case -- it is
//           a Bin but the cap, not the kind list, is what excludes
//           the oversized; a Print prints once per path before and
//           once after); the terminator Br/Cbr/Exit/Ret (Br/Cbr: a
//           continuation to re-key; Exit/Ret: the fn tail duplicated
//           into both arms, no routing). tterm saved BEFORE any
//           edit (the edits shift code indices).
//   conts   the continuation ids from the SAVED tterm (Br: one;
//           Cbr: two, a degenerate two-arm cbr collapses to one;
//           Exit/Ret: none); each must be a block other than
//           T/P0/P1 whose ONLY pred is T and whose first instr is
//           not a Phi (single-pred phis would tangle the insert).
//   escape  the T-defined values: phi dsts (their per-copy values
//           are the keyed args) then body dsts in block order (their
//           per-copy values are fresh vregs, allocated at commit).
//   route   WHERE a block reads T-defined values from: 0 (U0's
//           routing phi), 1 (U1's), 2+k (the k-th born site's phi),
//           -1 (unroutable). Seeded by dominance (exactly one
//           continuation dominating b -> its phi; T/P0/P1 -> -1),
//           then a forward fixpoint in block order: a single-pred
//           chain passes its pred's supplier down; a 2-pred merge
//           whose preds share a supplier reads it; one whose preds
//           arrive from DIFFERENT suppliers gets a fresh phi BORN
//           there (one arg per pred, each arg the pred's own
//           supplier). The loop-latch feedback edge (the merge value
//           feeding the loop header's phi through the post-arm
//           join) is exactly the born-site shape -- it is what
//           makes in-loop merges duplicable.
//   uses    every outside use of a T-defined value must be
//           routable: a plain use reads where ITS block reads from,
//           a phi arg where the KEYED PRED reads from. Each use
//           marks its need down the supplier chain (td_mark_need:
//           the uphis and every born phi on the route, recursively
//           -- a born phi's args read its preds' suppliers). A use
//           at an unroutable block bails the candidate BEFORE the
//           commit's first edit: the fn stays untouched.
//   commit  os.taildups first; fresh vregs for the body defs
//           (local0 before local1 per def, defv order); the
//           continuations' routing phis (phi(local0[di] @ P0,
//           local1[di] @ P1) per needed defv, U0 then U1, di order,
//           prepended at U's top); the born phis (site order =
//           creation order, so a site's args only reference EARLIER
//           sites' phis and the uphis; bvals order within a site,
//           prepended at W's top); the two copies (side 0 = P0
//           first: T's body+terminator with every T-defined operand
//           and def substituted to the side's per-copy value, phis
//           skipped -- they resolve to the keyed args and do not
//           copy -- replacing P's `br T' terminator, the body
//           sliding in before it); the rewire sweep (every outside
//           use -> its route's phi); recompute_preds.
//
// DATA STRUCTURES (TdCtx, what the former monolith held in locals):
//   fn/A          the rewrite target, the caller's analysis
//   T/P0/P1       the candidate: the shared tail + its two preds
//   U0/U1         the continuations (NONE = none)
//   nb/nv         the PASS-ENTRY block / vreg universe (the sub/rw
//                 guards read nv: a fresh vreg id >= nv is never a
//                 T-defined value, so the commit's own allocations
//                 are invisible to the substitution scans)
//   tterm         T's terminator, saved before the edits
//   phi_dst/a/b   T's phis, keyed (P0,P1)
//   defv          the T-defined value ids (phi dsts then body dsts)
//   local0/local1 the per-copy value of each defv (P0 / P1): the
//                 keyed args for phi dsts, fresh vregs for body dsts
//   route/bsites  the routing table + the born-phi re-merge sites
//   isdef         vreg -> defined by T (the defv membership test)
//   need0/need1   defv -> read through U0's / U1's routing phi
//   bvals         per born site: the defv indices it must merge
//   bad           the outside-use scan's bail flag
//   uphi0/uphi1   the continuations' routing-phi dsts per defv
//   bphi          per born site, per defv: the born phi's dst
//
// THEORY (why the duplication computes the same function):
//   * THE MERGE DISAPPEARS, THE VALUES SURVIVE. T ran once per path
//   with its phis selecting each pred's arg; after the duplication
//   each pred's copy runs with ITS arg baked in -- the same
//   arithmetic on the same values, one path at a time. The escaping
//   T-defined values (phi dsts and body dsts read outside) keep a
//   single definition per path: the two copies' local values merged
//   by the routing phis at the continuations (phi(local0 @ P0,
//   local1 @ P1)), exactly the two-way merge the original phi
//   performed -- one nesting level per re-merge, the checkpoint-19
//   delivered-walk discipline.
//   * THE ROUTE FIXPOINT IS A MINI SSA CONSTRUCTION. route[] is a
//   dominance-flavored placement: blocks under exactly one
//   continuation read its phi outright; the fixpoint then walks the
//   remaining blocks in RPO-ish block order, placing a fresh phi at
//   every 2-pred join of two DIFFERENT suppliers (the classic
//   iterated-dominance-frontier placement, one frontier level per
//   round, the round cap nb+1 the block-count bound of the chain).
//   A use at a block the fixpoint left unroutable (entry, dead-fed,
//   >2 preds, a cycle) has no legal phi home -- the candidate bails.
//   * VALIDATE, THEN EDIT. The outside-use scan proves every use
//   routable BEFORE os.taildups++ and the first new_anon_vreg: a
//   mid-commit bail would leave a half-duplicated fn (one arm's
//   copy landed, the other's terminator still `br T'). The commit
//   therefore never fails.
//   * GROWTH IS BOUNDED AND PAID BACK. The body duplicates into
//   BOTH preds (2x) before tier-0 folding shrinks it -- the cap
//   keeps the per-candidate growth <= 2*TAILDUP_BODY_MAX, and the
//   correlated-merge case (both phi args constant) folds to near-
//   nothing. One duplication per call: the fixpoint driver
//   re-analyzes, so the next candidate sees the folded arms.
//
// Byte-identical contract (what a refactor must NOT perturb):
//   * every pre-commit scan is bounded by the pass-entry nb/nv and
//   reads the pass-entry code (no edits before the commit). The
//   commit's block-shift loops use fn.nblocks() -- the original's
//   expression (this pass adds no blocks, so it equals nb, but the
//   expression is the original's).
//   * the content walk's if-chain ORDER is behavior: a Phi at the
//   last position takes the PHI branch (ok = false, `a phi cannot
//   terminate'), never the terminator branch; a non-phi at the last
//   position takes the terminator branch only because the phi test
//   is first. The body counter increments BEFORE the cap compare
//   (++body > TAILDUP_BODY_MAX).
//   * the content scan breaks on the FIRST failing instr; a phi
//   that fails the terminator check does not collect its args (the
//   original's `if (ok)' guard around the arg extraction).
//   * td_dedicated_br reads fn.code[fn.blocks[P].end - 1] UNGUARDED
//   — the original's read too (an empty pred block would underflow;
//   a block ending in a branch to T carries that branch, so the read
//   is safe exactly where the original was).
//   * td_succs_ok scans BOTH continuations fully -- no early break
//   after succs_ok goes false (the original had none).
//   * the route fixpoint: round cap <= nb (nb+1 rounds), inner scan
//   in block order, in-place visibility within a round (an earlier
//   block's resolution is visible to a later block the SAME round),
//   `no progress' breaks the round loop, the trailing sweep maps
//   every still-(-2) block to -1 (cycles among unresolved blocks).
//   * td_mark_need pushes r0 BEFORE r1 (the pop order sets the
//   bvals insertion order, which is the born-phi emission order);
//   the bvals dedup is the std::find containment test.
//   * the outside-use scan's b-loop tests !bad in its CONDITION;
//   the inner i-loop does not break on bad (the chks no-op once
//   bad is set -- `|| bad' in the guard).
//   * allocation order is observable (the register audit table
//   regenerates byte-identically only if it is preserved): body
//   locals local0-then-local1 per def in defv order; then the
//   routing phis U0's defs then U1's, di ascending, ONLY the
//   needed ones; then the born phis site by site, bvals order.
//   * the phi emissions use designated initializers in the
//   original's field order (.k .dst .a .b2 .t .f .c).
//   * the uphis prepend and the born-phis prepend are ONE helper
//   (td_prepend_phis) -- the two shift loops were textually
//   identical modulo names (at/k vs at/kc), provably the same
//   statement sequence. The copy install's shift is DIFFERENT and
//   stays separate: it replaces a terminator (erase+insert at
//   term_pos, signed begin > term_pos / end > term_pos compares,
//   x == P shifts only end) where the prepends shift begin >= at.
//   * td_sub_val folds the monolith's sub lambda and its inlined
//   dst rewrite: the same first-match scan with the same
//   !(v == NONE || v >= nv) guard (the dst form's `c.dst != NONE &&
//   c.dst < nv' is that guard negated) and the same one-substitution
//   stop (return vs break over an empty tail). The c2 substitution
//   skips Cbr/Br terminators (c2 would be a block id there).
//   * td_arg_of is the shared (r,di) -> phi-dst lookup: the born
//   phis' .a/.b2 and the rewire's assignment were the same
//   conditional chain. The rewire's `r < 0' silent return stays
//   (validated above: does not happen).
//   * the tail collection re-reads fn.blocks[T].begin/end fresh in
//   EACH side's iteration (the first side's install may shift T's
//   bounds when P0 sits before T -- side 1 must see the shifted
//   bounds). The rewire reads post-commit bounds; its b-loop bound
//   is the pass-entry nb.
//   * os.taildups++ is the commit's FIRST action, before any
//   allocation.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"

// the growth cap: the body duplicates into BOTH preds (2x) before
// tier-0 folding shrinks it — keep the per-candidate growth <= 12
// instrs; the correlated-merge case (both phi args constant) folds
// to near-nothing
inline constexpr uint32_t TAILDUP_BODY_MAX = 6;

// ---- the pass context (the former monolith's locals) ---------------------
struct TdCtx {
  Fn &fn;                      // the function under rewrite
  const An &A;                 // the caller's fresh analysis
  uint32_t T{}, P0{}, P1{};    // the candidate: the tail + its preds
  uint32_t U0{NONE}, U1{NONE}; // the continuations (NONE = none)
  uint32_t nb{}, nv{};         // pass-entry block / vreg universe
  Instr tterm{};               // T's terminator, saved pre-edit
  std::vector<uint32_t> phi_dst{}, phi_a{}, phi_b{}; // keyed (P0,P1)
  std::vector<uint32_t> defv{};                      // T-defined value ids
  std::vector<uint32_t> local0{}, local1{};          // per-copy value (P0/P1)
  std::vector<int> route{}; // block -> where it reads T-defs from
  struct Site {
    uint32_t W, p0, p1;
    int r0, r1; // the preds' supplier routes
  };
  std::vector<Site> bsites{};                 // the born-phi re-merge sites
  std::vector<char> isdef{};                  // vreg -> defined by T
  std::vector<char> need0{}, need1{};         // defv read via U0/U1's phi
  std::vector<std::vector<uint32_t>> bvals{}; // per site: defv idxs
  bool bad{};                                // the outside-use scan's bail flag
  std::vector<uint32_t> uphi0{}, uphi1{};    // routing phis per defv
  std::vector<std::vector<uint32_t>> bphi{}; // born phis per site
};

// ---- scan: the pass-entry guards ------------------------------------------
constexpr bool td_scan(Fn &fn, const An &A, TdCtx &X) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  X.nb = fn.nblocks();
  X.nv = fn.nvars + (uint32_t)fn.version_base.size();
  return X.nv != 0;
}

// ---- preds: the two (unconditional) predecessors ---------------------------
// a DEDICATED `br T': an unconditional jump whose only successor is T
constexpr bool td_dedicated_br(const Fn &fn, uint32_t P, uint32_t T) {
  const Instr &pt = fn.code[fn.blocks[P].end - 1];
  return pt.k == K::Br && pt.t == T && fn.succ0(P) == T && fn.succ1(P) == NONE;
}

constexpr bool td_preds_ok(const Fn &fn, const An &A, TdCtx &X) {
  if (fn.preds[X.T].size() != 2)
    return false;
  X.P0 = fn.preds[X.T][0];
  X.P1 = fn.preds[X.T][1];
  if (X.P0 == X.P1 || X.P0 == X.T || X.P1 == X.T || X.P0 >= X.nb ||
      X.P1 >= X.nb)
    return false;
  if (A.inner[X.T] == X.T)
    return false;       // a loop header's duplication is loop rotation
                        // (rotate.hpp) — deliberately not this pass
  bool preds_ok = true; // both checked, no early break (the original)
  for (uint32_t P : {X.P0, X.P1})
    if (!td_dedicated_br(fn, P, X.T))
      preds_ok = false;
  return preds_ok;
}

// ---- content: phis (both keys), small pure body, term ---------------------
// the phi arg keyed to pred P (the t-slot or the f-slot), NONE if absent
constexpr uint32_t td_phi_arg(const Instr &I, uint32_t P) {
  return (I.t == P) ? I.a : ((I.f == P) ? I.b2 : NONE);
}

// collect one of T's phis, keyed (P0,P1); ok = false when not keyed
constexpr void td_collect_phi(TdCtx &X, const Instr &I, bool &ok) {
  const uint32_t a0 = td_phi_arg(I, X.P0);
  const uint32_t a1 = td_phi_arg(I, X.P1);
  if (a0 == NONE || a1 == NONE || a0 >= X.nv || a1 >= X.nv) {
    ok = false; // not keyed to the two preds
    return;
  }
  X.phi_dst.push_back(I.dst);
  X.phi_a.push_back(a0);
  X.phi_b.push_back(a1);
}

constexpr bool td_content_ok(const Fn &fn, TdCtx &X) {
  const uint32_t T = X.T;
  if (fn.blocks[T].begin == fn.blocks[T].end)
    return false; // paranoia: empty blocks carry no terminator
  X.phi_dst.clear();
  X.phi_a.clear();
  X.phi_b.clear();
  uint32_t body = 0;
  bool ok = true;
  for (uint32_t i = fn.blocks[T].begin; i < fn.blocks[T].end; ++i) {
    const Instr &I = fn.code[i];
    if (I.k == K::Phi) {
      if (i + 1 == fn.blocks[T].end)
        ok = false; // a phi cannot terminate
      if (ok)
        td_collect_phi(X, I, ok);
    } else if (i + 1 == fn.blocks[T].end) { // terminator
      if (I.k != K::Br && I.k != K::Cbr && I.k != K::Exit && I.k != K::Ret)
        ok = false; // Br/Cbr: a continuation to re-key; Exit/Ret: the
                    // fn tail duplicated into both arms (no routing)
    } else if (I.k == K::Const || I.k == K::Copy || I.k == K::Bin ||
               I.k == K::Print) {
      if (++body > TAILDUP_BODY_MAX)
        ok = false; // growth cap
    } else
      ok = false; // calls/memory/observations stay shared
    if (!ok)
      break;
  }
  if (!ok)
    return false;
  X.tterm = fn.code[fn.blocks[T].end - 1];
  return true;
}

// ---- conts: the continuation(s) -------------------------------------------
// from the SAVED tterm; false = the target is out of range (bail)
constexpr bool td_continuations(TdCtx &X) {
  X.U0 = NONE;
  X.U1 = NONE; // Exit/Ret: no continuation
  if (X.tterm.k == K::Br) {
    X.U0 = X.tterm.t;
    return X.U0 != NONE && X.U0 < X.nb;
  }
  if (X.tterm.k == K::Cbr) {
    X.U0 = X.tterm.t;
    X.U1 = X.tterm.f;
    if (X.U0 == NONE || X.U0 >= X.nb || X.U1 == NONE || X.U1 >= X.nb)
      return false;
    if (X.U1 == X.U0)
      X.U1 = NONE; // degenerate two-arm cbr: one continuation
  }
  return true;
}

// each successor's ONLY pred must be T, and it must not open with a
// phi (single-pred phis would tangle the prepend); BOTH are scanned
// in full -- no early break (the original had none)
constexpr bool td_succs_ok(const Fn &fn, const TdCtx &X) {
  bool succs_ok = true;
  for (uint32_t U : {X.U0, X.U1}) {
    if (U == NONE)
      continue;
    if (U == X.T || U == X.P0 || U == X.P1 || U >= X.nb)
      succs_ok = false;
    else if (fn.preds[U].size() != 1 || fn.preds[U][0] != X.T)
      succs_ok = false;
    else if (fn.blocks[U].begin != fn.blocks[U].end &&
             fn.code[fn.blocks[U].begin].k == K::Phi)
      succs_ok = false; // single-pred phis would tangle the insert
  }
  return succs_ok;
}

// ---- escape: the escaping values and their per-copy slots ------------------
// phi dsts first (per-copy values: the keyed args), then body dsts
// in block order (per-copy values: fresh vregs, allocated at commit)
constexpr void td_escape_inventory(const Fn &fn, TdCtx &X) {
  X.defv.clear();
  X.local0.clear();
  X.local1.clear();
  for (size_t k = 0; k < X.phi_dst.size(); ++k) {
    X.defv.push_back(X.phi_dst[k]);
    X.local0.push_back(X.phi_a[k]);
    X.local1.push_back(X.phi_b[k]);
  }
  for (uint32_t i = fn.blocks[X.T].begin; i + 1 < fn.blocks[X.T].end; ++i)
    if (fn.code[i].dst != NONE && fn.code[i].k != K::Phi) {
      X.defv.push_back(fn.code[i].dst);
      X.local0.push_back(NONE); // filled once the fresh copy vregs exist
      X.local1.push_back(NONE);
    }
}

// ---- route: the supplier fixpoint ------------------------------------------
// seed: T and its preds are dead to the routing; a block dominated
// by exactly one continuation reads its phi (both -> unresolved, the
// pred fixpoint below decides; neither -> unresolved too)
constexpr void td_route_seed(const An &A, TdCtx &X) {
  X.route.assign(X.nb, -2);
  X.bsites.clear();
  for (uint32_t b = 0; b < X.nb; ++b) {
    if (b == X.T || b == X.P0 || b == X.P1) {
      X.route[b] = -1;
      continue;
    }
    const bool d0 = (X.U0 != NONE) && A.dominates(X.U0, b);
    const bool d1 = (X.U1 != NONE) && A.dominates(X.U1, b);
    if (d0 && !d1)
      X.route[b] = 0;
    else if (d1 && !d0)
      X.route[b] = 1;
  }
}

// a fresh phi is born at the re-merge, one arg per pred
constexpr void td_born_site(TdCtx &X, uint32_t b,
                            const std::vector<uint32_t> &ps) {
  const int r0 = X.route[ps[0]], r1 = X.route[ps[1]];
  X.bsites.push_back({b, ps[0], ps[1], r0, r1});
  X.route[b] = (int)(2 + X.bsites.size() - 1);
}

// one block's route resolution; true = progressed this block
constexpr bool td_route_step(const Fn &fn, TdCtx &X, uint32_t b) {
  if (X.route[b] != -2)
    return false;
  const std::vector<uint32_t> &ps = fn.preds[b];
  bool any_unres = false, any_dead = false;
  for (uint32_t p : ps) {
    if (X.route[p] == -2)
      any_unres = true;
    else if (X.route[p] < 0)
      any_dead = true;
  }
  if (any_unres)
    return false; // wait for the pred's round
  if (ps.empty() || any_dead || ps.size() > 2) {
    X.route[b] = -1; // entry / dead-fed / >2 preds: no phi possible
    return true;
  }
  if (ps.size() == 1) { // single-pred chain: the pred's supplier
    X.route[b] = X.route[ps[0]];
    return true;
  }
  if (X.route[ps[0]] == X.route[ps[1]]) { // both arms, one supplier
    X.route[b] = X.route[ps[0]];
    return true;
  }
  td_born_site(X, b, ps);
  return true;
}

constexpr void td_route_fixpoint(const Fn &fn, TdCtx &X) {
  for (uint32_t round = 0; round <= X.nb; ++round) {
    bool progressed = false;
    for (uint32_t b = 0; b < X.nb; ++b)
      progressed |= td_route_step(fn, X, b);
    if (!progressed)
      break;
  }
  for (int &r : X.route)
    if (r == -2)
      r = -1; // cycles among unresolved blocks: unroutable
}

// ---- uses: the outside-use scan --------------------------------------------
// walk a supplier route: mark the uphis / born phis it reads (a born
// phi's args read its preds' suppliers, recursively down to U0/U1's)
constexpr void td_mark_need(TdCtx &X, int r, uint32_t di) {
  std::vector<int> stack{r};
  while (!stack.empty()) {
    const int x = stack.back();
    stack.pop_back();
    if (x == 0) {
      X.need0[di] = 1;
      continue;
    }
    if (x == 1) {
      X.need1[di] = 1;
      continue;
    }
    const size_t k = (size_t)(x - 2);
    if (std::find(X.bvals[k].begin(), X.bvals[k].end(), di) == X.bvals[k].end())
      X.bvals[k].push_back(di);
    stack.push_back(X.bsites[k].r0);
    stack.push_back(X.bsites[k].r1);
  }
}

// one operand check: a plain use reads where ITS block reads from; a
// phi arg reads where the KEYED PRED reads from (the value holds at
// its end). An unroutable read sets bad (the candidate bails)
constexpr void td_check_use(TdCtx &X, uint32_t v, uint32_t key_pred, bool isphi,
                            uint32_t b) {
  if (v == NONE || v >= X.nv || !X.isdef[v] || X.bad)
    return;
  const int r = X.route[isphi ? key_pred : b];
  if (r < 0) {
    X.bad = true;
    return;
  }
  const size_t di =
      (size_t)(std::find(X.defv.begin(), X.defv.end(), v) - X.defv.begin());
  td_mark_need(X, r, (uint32_t)di);
}

constexpr bool td_outside_uses_ok(const Fn &fn, TdCtx &X) {
  X.isdef.assign(X.nv, 0);
  for (uint32_t v : X.defv)
    X.isdef[v] = 1;
  X.need0.assign(X.defv.size(), 0);
  X.need1.assign(X.defv.size(), 0);
  X.bvals.assign(X.bsites.size(), {});
  X.bad = false;
  for (uint32_t b = 0; b < X.nb && !X.bad; ++b) {
    if (b == X.T || b == X.P0 || b == X.P1)
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      const bool isphi = (I.k == K::Phi);
      if (isphi) { // arg a keyed pred t, arg b2 keyed pred f
        td_check_use(X, I.a, I.t, isphi, b);
        td_check_use(X, I.b2, I.f, isphi, b);
      } else {
        td_check_use(X, I.a, NONE, isphi, b);
        td_check_use(X, I.b2, NONE, isphi, b);
        td_check_use(X, I.c2, NONE, isphi, b);
        for (uint32_t av : I.args)
          td_check_use(X, av, NONE, isphi, b);
      }
    }
  }
  return !X.bad;
}

// ---- commit ----------------------------------------------------------------
// fresh vregs for the body defs' per-copy values (local0 then local1
// per def, defv order — the allocation order is observable)
constexpr void td_alloc_locals(Fn &fn, TdCtx &X) {
  for (size_t di = 0; di < X.defv.size(); ++di)
    if (X.local0[di] == NONE) { // a body def
      X.local0[di] = new_anon_vreg(fn, fn.base_of(X.defv[di]));
      X.local1[di] = new_anon_vreg(fn, fn.base_of(X.defv[di]));
    }
}

// prepend phis at B's top and shift every later block bound (the
// uphi and born-phi prepends shared this exact statement sequence)
constexpr void td_prepend_phis(Fn &fn, uint32_t B,
                               const std::vector<Instr> &phis) {
  const uint32_t at = fn.blocks[B].begin;
  fn.code.insert(fn.code.begin() + (long)at, phis.begin(), phis.end());
  const uint32_t k = (uint32_t)phis.size();
  for (uint32_t x = 0; x < fn.nblocks(); ++x) {
    if (x == B) {
      fn.blocks[x].end += k;
      continue;
    }
    if (fn.blocks[x].begin >= at)
      fn.blocks[x].begin += k;
    if (fn.blocks[x].end > at)
      fn.blocks[x].end += k;
  }
}

// the (r,di) -> phi-dst lookup shared by the born phis' args and the
// rewire's assignment (0 = U0's phi, 1 = U1's, 2+k = site k's)
constexpr uint32_t td_arg_of(const TdCtx &X, int r, uint32_t di) {
  return (r == 0) ? X.uphi0[di]
                  : ((r == 1) ? X.uphi1[di] : X.bphi[(size_t)(r - 2)][di]);
}

// per-continuation routing phis (U has no phis of its own: single-pred)
constexpr void td_emit_uphis(Fn &fn, TdCtx &X) {
  X.uphi0.assign(X.defv.size(), NONE);
  X.uphi1.assign(X.defv.size(), NONE);
  for (uint32_t U : {X.U0, X.U1}) {
    if (U == NONE)
      continue;
    std::vector<Instr> phis{};
    for (size_t di = 0; di < X.defv.size(); ++di) {
      if (!((U == X.U0) ? X.need0[di] : X.need1[di]))
        continue; // no reader routes through this continuation
      const uint32_t d = new_anon_vreg(fn, fn.base_of(X.defv[di]));
      if (U == X.U0)
        X.uphi0[di] = d;
      else
        X.uphi1[di] = d;
      phis.push_back({.k = K::Phi,
                      .dst = d,
                      .a = X.local0[di],
                      .b2 = X.local1[di],
                      .t = X.P0,
                      .f = X.P1,
                      .c = (int64_t)fn.base_of(X.defv[di])});
    }
    if (!phis.empty()) // prepend at U's top
      td_prepend_phis(fn, U, phis);
  }
}

// born phis at the re-merge sites (site order = creation order, so a
// site's args only reference EARLIER sites' phis and the uphis)
constexpr void td_emit_bphis(Fn &fn, TdCtx &X) {
  X.bphi.assign(X.bsites.size(), {});
  for (size_t k = 0; k < X.bsites.size(); ++k) {
    const TdCtx::Site &S = X.bsites[k];
    std::vector<Instr> phis{};
    X.bphi[k].assign(X.defv.size(), NONE);
    for (uint32_t di : X.bvals[k]) {
      const uint32_t d = new_anon_vreg(fn, fn.base_of(X.defv[di]));
      X.bphi[k][di] = d;
      phis.push_back({.k = K::Phi,
                      .dst = d,
                      .a = td_arg_of(X, S.r0, di),
                      .b2 = td_arg_of(X, S.r1, di),
                      .t = S.p0,
                      .f = S.p1,
                      .c = (int64_t)fn.base_of(X.defv[di])});
    }
    if (!phis.empty()) // prepend (W may keep phis of its own below)
      td_prepend_phis(fn, S.W, phis);
  }
}

// substitute a T-defined value id with this side's per-copy value
// (the monolith's sub lambda and its inlined dst rewrite were the
// same first-match scan under the same NONE/range guard: one helper)
constexpr void td_sub_val(const TdCtx &X, int side, uint32_t &v) {
  if (v == NONE || v >= X.nv)
    return;
  for (size_t di = 0; di < X.defv.size(); ++di)
    if (v == X.defv[di]) {
      v = side ? X.local1[di] : X.local0[di];
      return;
    }
}

// the substituted copy of T's body+terminator for one pred's arm;
// the bounds are re-read fresh per side (the first side's install
// may have shifted T's window when its pred sits before T)
constexpr void td_copy_tail(const Fn &fn, const TdCtx &X, int side,
                            std::vector<Instr> &tail) {
  for (uint32_t i = fn.blocks[X.T].begin; i < fn.blocks[X.T].end; ++i) {
    Instr c = fn.code[i];
    if (c.k == K::Phi)
      continue; // phis resolve to the keyed args, they do not copy
    td_sub_val(X, side, c.dst); // the copy's defs are its own
    td_sub_val(X, side, c.a);
    td_sub_val(X, side, c.b2);
    if (c.k != K::Cbr && c.k != K::Br && c.c2 != NONE)
      td_sub_val(X, side, c.c2);
    for (uint32_t &av : c.args)
      td_sub_val(X, side, av);
    tail.push_back(c);
  }
}

// replace P's terminator with the substituted terminator, the body
// sliding in before it; then P's successors are T's (the substituted
// terminator's targets)
constexpr void td_install_tail(Fn &fn, uint32_t P,
                               const std::vector<Instr> &tail) {
  const uint32_t term_pos = fn.blocks[P].end - 1;
  fn.code.erase(fn.code.begin() + (long)term_pos);
  fn.code.insert(fn.code.begin() + (long)term_pos, tail.begin(), tail.end());
  const int delta = (int)tail.size() - 1;
  for (uint32_t x = 0; x < fn.nblocks(); ++x) {
    if (x == P) {
      fn.blocks[x].end = (uint32_t)((int)fn.blocks[x].end + delta);
      continue;
    }
    if ((int)fn.blocks[x].begin > (int)term_pos)
      fn.blocks[x].begin = (uint32_t)((int)fn.blocks[x].begin + delta);
    if ((int)fn.blocks[x].end > (int)term_pos)
      fn.blocks[x].end = (uint32_t)((int)fn.blocks[x].end + delta);
  }
  const Instr &nt = fn.code[fn.blocks[P].end - 1];
  fn.succ[2 * P] = nt.t;
  fn.succ[2 * P + 1] = (nt.k == K::Cbr) ? nt.f : NONE;
}

constexpr void td_emit_copies(Fn &fn, const TdCtx &X) {
  for (int side = 0; side < 2; ++side) {
    const uint32_t P = side ? X.P1 : X.P0;
    std::vector<Instr> tail{};
    td_copy_tail(fn, X, side, tail);
    td_install_tail(fn, P, tail);
  }
}

// ---- reroute the outside uses ----------------------------------------------
constexpr void td_rewire_use(const TdCtx &X, uint32_t &v, uint32_t key_pred,
                             bool isphi, uint32_t b) {
  if (v == NONE || v >= X.nv || !X.isdef[v])
    return;
  const int r = X.route[isphi ? key_pred : b];
  if (r < 0)
    return; // validated above: does not happen
  for (size_t di = 0; di < X.defv.size(); ++di)
    if (v == X.defv[di]) {
      v = td_arg_of(X, r, di);
      return;
    }
}

constexpr void td_rewire_uses(Fn &fn, const TdCtx &X) {
  for (uint32_t b = 0; b < X.nb; ++b) {
    if (b == X.T || b == X.P0 || b == X.P1)
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      const bool isphi = (I.k == K::Phi);
      if (isphi) {
        td_rewire_use(X, I.a, I.t, isphi, b);
        td_rewire_use(X, I.b2, I.f, isphi, b);
      } else {
        td_rewire_use(X, I.a, NONE, isphi, b);
        td_rewire_use(X, I.b2, NONE, isphi, b);
        td_rewire_use(X, I.c2, NONE, isphi, b);
        for (uint32_t &av : I.args)
          td_rewire_use(X, av, NONE, isphi, b);
      }
    }
  }
}

// the commit: every route was proven, every allocation order is the
// original's; recompute_preds last
constexpr void td_commit(Fn &fn, TdCtx &X, OptStats &os) {
  os.taildups++;
  td_alloc_locals(fn, X); // fresh vregs for the body defs
  td_emit_uphis(fn, X);   // the continuations' routing phis
  td_emit_bphis(fn, X);   // the born phis at the re-merges
  td_emit_copies(fn, X);  // T's body+term into both preds
  td_rewire_uses(fn, X);  // the outside uses -> their suppliers
  fn.recompute_preds();
}
