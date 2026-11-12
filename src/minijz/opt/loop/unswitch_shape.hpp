#pragma once
// === unswitch_shape.hpp ========================================
// the shared loop-unswitching shape layer (checkpoint 19's split of
// unswitch.hpp, mirroring loop_shape.hpp / the lcf split): the
// invariant-condition gate, the exit-edge inventory, the clone
// infrastructure (block/vreg id allocation + the append-only full-loop
// clone emission every unswitch path shares), the compound
// short-circuit matcher (checkpoint 18, moved verbatim), and the
// checkpoint-19 body-if matcher (the classic LLVM "trivial" unswitch
// of an invariant INNER branch).
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/phi_norm.hpp"
#include "../analysis/an.hpp"
#include "loop_shape.hpp"

// ---- the invariant-condition gate ------------------------------------------
// shared by every unswitch path: cond defined OUTSIDE the loop (its def
// block not inL), not a constant (fold/sccp territory), and available at
// the preheader's end (defined in the preheader itself or dominating
// it — a def elsewhere outside the loop that does not dominate pre
// cannot be read by the versioned guard).
constexpr bool cond_invariant(const Fn &fn, const An &A,
                              const std::vector<char> &inL, const LoopScan &S,
                              uint32_t pre, uint32_t cond) {
  if (cond == NONE || cond >= S.nv || S.defi[cond] == NONE)
    return false;
  if (inL[S.defb[cond]])
    return false; // defined inside: not invariant
  if (fn.code[S.defi[cond]].k == K::Const)
    return false; // constant test: fold/sccp territory
  if (S.defb[cond] != pre && !A.dominates(S.defb[cond], pre))
    return false; // not available at the preheader's end
  return true;
}

// ---- the exit-edge inventory ------------------------------------------------
// every in-loop block -> outside-target edge. `primary` (the
// zero-iteration edge h->X / mm->X) is EXCLUDED by the trivial/compound
// paths — they replace it outright; the body-if path passes NONE (its
// header exit is a REGULAR exit edge: both versions keep running the
// loop, so the exit value is version-merged like any break's). Returns
// false on structural paranoia: an edge targeting pre/h (the versioned
// CFG would tangle their preds) or a degenerate both-arms-outside cbr.
struct ExitEdge {
  uint32_t from, to;
};
constexpr bool exit_inventory(const Fn &fn, const std::vector<char> &inL,
                              uint32_t nb, uint32_t primary, uint32_t X,
                              uint32_t pre, uint32_t h,
                              std::vector<ExitEdge> &exits) {
  for (uint32_t b = 0; b < nb; ++b) {
    if (!inL[b])
      continue;
    const uint32_t s0 = fn.succ0(b), s1 = fn.succ1(b);
    if (s0 != NONE && s1 != NONE && s0 == s1 && !inL[s0])
      return false; // degenerate cbr (cbr_same's territory)
    for (uint32_t s : {s0, s1}) {
      if (s == NONE || inL[s])
        continue;
      if (b == primary && s == X)
        continue; // the edge the caller replaces outright
      exits.push_back({b, s});
      if (s == pre || s == h)
        return false; // structurally impossible; paranoia
    }
  }
  return true;
}

// ---- the compound (short-circuit) condition shape --------------------------
// h: cbr(c1) A/B (both arms in-loop); one arm is the elbow E, the other
// goes straight to the 1-block (||) or the 0-block (&&); E: cbr(c2)
// TT/FF; TT/FF: [Const/Copy] br mm; mm: phi(1@TT, 0@FF) + cbr(phi)
// body/X. Nested short-circuits and non-canonical shapes bail.
struct CompShape {
  uint32_t pre{NONE}, latch{NONE}, E{NONE}, TT{NONE}, FF{NONE}, mm{NONE};
  uint32_t body{NONE}, X{NONE};
  uint32_t c1{NONE}, c2{NONE};
  bool ok{}, is_and{};
};
constexpr CompShape compound_shape_of(const Fn &fn, const An &A,
                                      const std::vector<char> &inL,
                                      const LoopScan &S, uint32_t h) {
  (void)A;
  const uint32_t nb = fn.nblocks(), nv = S.nv;
  CompShape cs{};
  uint32_t outside = 0, inside = 0;
  for (uint32_t p : fn.preds[h]) {
    if (inL[p]) {
      cs.latch = p;
      ++inside;
    } else {
      cs.pre = p;
      ++outside;
    }
  }
  if (inside != 1 || outside != 1 || cs.pre == NONE)
    return cs;
  if (fn.blocks[h].begin == fn.blocks[h].end)
    return cs;
  const Instr hterm = fn.code[fn.blocks[h].end - 1];
  if (hterm.k != K::Cbr)
    return cs;
  if (hterm.t == NONE || hterm.f == NONE || hterm.t >= nb || hterm.f >= nb ||
      !inL[hterm.t] || !inL[hterm.f])
    return cs; // the trivial path owns shapes with an outside arm
  cs.c1 = hterm.a;
  // h's own non-phi content must be pure (the false path skips it):
  // the invariant check on c1 already rejects condition evaluation in
  // the header, this only guards exotic leftovers.
  for (uint32_t i = fn.blocks[h].begin; i + 1 < fn.blocks[h].end; ++i) {
    const Instr &I = fn.code[i];
    if (I.k == K::Phi)
      continue;
    if (I.k != K::Const && I.k != K::Copy && I.k != K::Bin)
      return cs;
    if (I.k == K::Bin && (I.b == B::Div || I.b == B::Mod) && !I.nt)
      return cs;
  }
  // try each arm as the elbow E (the other arm is the direct feeder)
  for (int k = 0; k < 2; ++k) {
    const uint32_t E = k ? hterm.f : hterm.t;
    const uint32_t other = k ? hterm.t : hterm.f;
    if (E == other)
      return cs; // degenerate (cbr_same territory)
    if (fn.preds[E].size() != 1 || fn.preds[E][0] != h)
      continue;
    if (fn.blocks[E].begin == fn.blocks[E].end)
      continue;
    const Instr eterm = fn.code[fn.blocks[E].end - 1];
    if (eterm.k != K::Cbr)
      continue;
    const uint32_t TT = eterm.t, FF = eterm.f;
    if (TT == NONE || FF == NONE || TT == FF || TT >= nb || FF >= nb ||
        !inL[TT] || !inL[FF])
      continue;
    bool is_and = false;
    if (other == FF)
      is_and = true; // &&: h-false feeds the 0-block directly
    else if (other == TT)
      is_and = false; // ||: h-true feeds the 1-block directly
    else
      continue;
    // E's non-terminator content must be pure (same argument as h's)
    {
      bool ok = true;
      for (uint32_t i = fn.blocks[E].begin; i + 1 < fn.blocks[E].end && ok;
           ++i) {
        const Instr &I = fn.code[i];
        if (I.k != K::Const && I.k != K::Copy && I.k != K::Bin)
          ok = false;
        else if (I.k == K::Bin && (I.b == B::Div || I.b == B::Mod) && !I.nt)
          ok = false;
      }
      if (!ok)
        continue;
    }
    // TT/FF: Const/Copy only + br mm (both feed ONE merge), preds from
    // {h, E} only (&&: TT={E}, FF={h,E}; ||: TT={h,E}, FF={E})
    uint32_t mmT = NONE, mmF = NONE;
    {
      bool ok = true;
      for (int f = 0; f < 2 && ok; ++f) {
        const uint32_t F2 = f ? FF : TT;
        for (uint32_t p : fn.preds[F2])
          if (p != h && p != E)
            ok = false;
        for (uint32_t i = fn.blocks[F2].begin; i + 1 < fn.blocks[F2].end && ok;
             ++i)
          if (fn.code[i].k != K::Const && fn.code[i].k != K::Copy)
            ok = false;
        const Instr &t2 = fn.code[fn.blocks[F2].end - 1];
        if (t2.k != K::Br || fn.succ1(F2) != NONE)
          ok = false;
        if (ok)
          (f ? mmF : mmT) = t2.t;
      }
      if (!ok || mmT == NONE || mmT != mmF)
        continue; // not a two-feeder merge
    }
    const uint32_t mm = mmT;
    if (mm == NONE || mm >= nb || !inL[mm])
      continue;
    // mm: exactly the phi + the cbr; the phi materializes the boolean
    // (1 from TT, 0 from FF); the cbr tests it with one arm staying in
    // (body) and one leaving (X)
    if (fn.preds[mm].size() != 2 ||
        fn.blocks[mm].end - fn.blocks[mm].begin != 2)
      continue;
    const Instr &P = fn.code[fn.blocks[mm].begin];
    const Instr &MT = fn.code[fn.blocks[mm].end - 1];
    if (P.k != K::Phi || MT.k != K::Cbr || MT.a != P.dst)
      continue;
    const uint32_t argT = (P.t == TT) ? P.a : ((P.f == TT) ? P.b2 : NONE);
    const uint32_t argF = (P.t == FF) ? P.a : ((P.f == FF) ? P.b2 : NONE);
    if (argT == NONE || argF == NONE || argT >= nv || argF >= nv)
      continue;
    if (!S.kc[argT] || S.kv[argT] != 1 || !S.kc[argF] || S.kv[argF] != 0)
      continue; // the boolean materialization, nothing else
    uint32_t body = NONE, X = NONE;
    if (MT.t != NONE && MT.t < nb && inL[MT.t] && MT.f != NONE && MT.f < nb &&
        !inL[MT.f]) {
      body = MT.t;
      X = MT.f;
    } else if (MT.f != NONE && MT.f < nb && inL[MT.f] && MT.t != NONE &&
               MT.t < nb && !inL[MT.t]) {
      body = MT.f;
      X = MT.t;
    } else
      continue;
    if (body == h || X == cs.pre || X == h || inL[X])
      continue;
    // the clone bypasses the whole diamond straight into the body, so
    // the body must be entered ONLY through mm (no phis to rekey)
    if (fn.preds[body].size() != 1 || fn.preds[body][0] != mm)
      continue;
    cs.E = E;
    cs.TT = TT;
    cs.FF = FF;
    cs.mm = mm;
    cs.body = body;
    cs.X = X;
    cs.c2 = eterm.a;
    cs.is_and = is_and;
    cs.ok = true;
    return cs;
  }
  return cs;
}

// ---- the body-if (trivial unswitch) shape -----------------------------------
// an inner branch on an SSA-proven invariant condition: bif in the loop
// (not the header — that tests the while-condition — and not the latch —
// that branches back), bif's cbr(INV) A/B with BOTH arms in-loop and
// each arm entered ONLY through bif. Both versions keep running the
// loop, so nothing is skipped: the original with INV true ran arm A
// every iteration (INV is an SSA value — it cannot change), and the
// versioned true side is exactly that program; no purity gates needed
// beyond INV's invariance (each version executes its chosen arm's side
// effects per iteration, exactly like the original did).
struct BifShape {
  uint32_t bif{NONE}, A{NONE}, B{NONE}; // the branch, its two arms
  uint32_t cond{NONE};                  // the invariant condition
  bool ok{};
};
constexpr BifShape body_if_shape_of(const Fn &fn, const An &A,
                                    const std::vector<char> &inL,
                                    const LoopScan &S, uint32_t h, uint32_t pre,
                                    uint32_t latch) {
  BifShape bs{};
  const uint32_t nb = fn.nblocks();
  for (uint32_t b = 0; b < nb; ++b) {
    if (!inL[b] || b == h || b == latch)
      continue;
    if (fn.blocks[b].begin == fn.blocks[b].end)
      continue;
    const Instr t = fn.code[fn.blocks[b].end - 1];
    if (t.k != K::Cbr)
      continue;
    if (t.a == NONE || t.t == NONE || t.f == NONE || t.t >= nb || t.f >= nb)
      continue;
    if (!inL[t.t] || !inL[t.f] || t.t == t.f || t.t == b || t.f == b)
      continue; // both arms stay in the loop (an exit branch is not bif)
    // each arm entered ONLY through bif: rewriting bif to br A then
    // kills B outright (nothing else targets it), and the arms' merge
    // phis degenerate under unreach in the same tier round
    if (fn.preds[t.t].size() != 1 || fn.preds[t.t][0] != b)
      continue;
    if (fn.preds[t.f].size() != 1 || fn.preds[t.f][0] != b)
      continue;
    if (!cond_invariant(fn, A, inL, S, pre, t.a))
      continue; // the branch condition must be SSA-proven invariant
    bs.bif = b;
    bs.A = t.t;
    bs.B = t.f;
    bs.cond = t.a;
    bs.ok = true;
    return bs; // first match wins: one unswitch per call
  }
  return bs;
}

// ---- the clone infrastructure
// ------------------------------------------------ every unswitch path versions
// the loop with a full fresh-vreg clone: bmap (in-loop block -> clone id) and
// fresh (loop-defined vreg -> clone vreg) are allocated BEFORE any other new
// block so later ids (guards, version-merges) stay stable, and the emission is
// append-only (existing block ranges stay valid).
struct CloneMap {
  std::vector<uint32_t> bmap{};  // in-loop block -> clone block id
  std::vector<uint32_t> fresh{}; // loop-def vreg -> clone vreg (NONE: identity)
};
constexpr CloneMap alloc_clone(Fn &fn, const LoopScan &S,
                               const std::vector<char> &inL) {
  CloneMap C{};
  C.bmap.assign(S.nb, NONE);
  for (uint32_t b = 0; b < S.nb; ++b) {
    if (!inL[b])
      continue;
    C.bmap[b] = fn.nblocks();
    fn.blocks.push_back({});
    fn.succ.push_back(NONE);
    fn.succ.push_back(NONE);
  }
  C.fresh.assign(S.nv, NONE);
  for (uint32_t v = 0; v < S.nv; ++v)
    if (S.defi[v] != NONE && inL[S.defb[v]])
      C.fresh[v] = new_anon_vreg(fn, fn.base_of(v));
  return C;
}
constexpr uint32_t mapv(const CloneMap &C, uint32_t v) {
  if (v == NONE || v >= C.fresh.size())
    return v;
  return C.fresh[v] != NONE ? C.fresh[v] : v;
}
// emit the full clone (append-only). pre: the ORIGINAL preheader id
// (the key the header phis' entry edges carry). entry_pred: what those
// edges rekey to (the trivial/body-if shapes: pre itself — a no-op;
// the compound shape: the last guard block). bypass: the in-loop block
// the cloned header's terminator REPLACES its test with (the trivial
// shape's body arm / the compound shape's real body, past the whole
// condition diamond); NONE keeps the cloned header's Cbr (the body-if
// shape: both versions keep running the loop, so the while-test stays
// live in the clone too).
constexpr void emit_loop_clone(Fn &fn, const LoopScan &S,
                               const std::vector<char> &inL, const CloneMap &C,
                               uint32_t h, uint32_t pre, uint32_t entry_pred,
                               uint32_t bypass) {
  const uint32_t nb = S.nb, nv = S.nv;
  for (uint32_t b = 0; b < nb; ++b) {
    if (!inL[b])
      continue;
    const uint32_t B = C.bmap[b];
    fn.blocks[B].begin = (uint32_t)fn.code.size();
    uint32_t hbr_tgt = NONE; // the header clone's Br target
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      Instr c = I;
      if (c.dst != NONE && c.dst < nv)
        c.dst = C.fresh[c.dst];
      if (c.k == K::Phi) {
        c.a = mapv(C, c.a); // pre_arg stays (outside def), latch remaps
        c.b2 = mapv(C, c.b2);
        if (c.t != NONE && inL[c.t])
          c.t = C.bmap[c.t];
        if (c.f != NONE && inL[c.f])
          c.f = C.bmap[c.f];
        // the clone is entered through the LAST guard block (the
        // trivial/body-if shapes: pre itself, a no-op rekey)
        if (b == h) {
          if (c.t == pre)
            c.t = entry_pred;
          if (c.f == pre)
            c.f = entry_pred;
        }
      } else if (i + 1 == fn.blocks[b].end && (c.k == K::Br || c.k == K::Cbr)) {
        // terminator: in-loop targets -> clones; exits stay
        c.a = mapv(C, c.a);
        if (c.t != NONE && inL[c.t])
          c.t = C.bmap[c.t];
        if (c.k == K::Cbr && c.f != NONE && inL[c.f])
          c.f = C.bmap[c.f];
      } else {
        c.a = mapv(C, c.a);
        c.b2 = mapv(C, c.b2);
        if (c.c2 != NONE)
          c.c2 = mapv(C, c.c2);
        for (uint32_t &av : c.args)
          av = mapv(C, av);
      }
      // the cloned header's test is GONE for the versioning paths:
      // Br to the bypass target (the trivial shape's body arm; the
      // compound shape's real body, past the whole condition diamond).
      // The body-if path passes bypass == NONE and keeps the test.
      if (b == h && bypass != NONE && i + 1 == fn.blocks[b].end &&
          c.k == K::Cbr) {
        hbr_tgt = C.bmap[bypass]; // bypass is in-loop by construction
        c.k = K::Br;
        c.t = hbr_tgt;
        c.f = NONE;
        c.a = NONE;
      }
      fn.code.push_back(c);
    }
    fn.blocks[B].end = (uint32_t)fn.code.size();
    if (b == h && bypass != NONE) { // succ0 = the Br target; exit arm gone
      fn.succ[2 * B] = hbr_tgt;
      fn.succ[2 * B + 1] = NONE;
    } else {
      fn.succ[2 * B] = (fn.succ0(b) != NONE && inL[fn.succ0(b)])
                           ? C.bmap[fn.succ0(b)]
                           : fn.succ0(b);
      fn.succ[2 * B + 1] = (fn.succ1(b) != NONE && inL[fn.succ1(b)])
                               ? C.bmap[fn.succ1(b)]
                               : fn.succ1(b);
    }
  }
}
