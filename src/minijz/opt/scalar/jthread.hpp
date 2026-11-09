#pragma once
// === jthread.hpp ========================================
// jump threading: per-edge resolution of "elbow" branches.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "../analysis/intervals.hpp"

// --- 9b3. JUMP THREADING -------------------------------------------------
// A branch edge into an "elbow" block -- a block that is nothing but phis,
// one comparison and a conditional branch -- can be redirected straight to
// the elbow's taken successor when the comparison is already decided ON
// THAT EDGE. The range analysis's per-block entry states hull over ALL
// incoming edges, so a comparison decided per-edge only (x = 1 along one
// arm, x = 9 along the other: hull [1,9], undecided) is invisible to
// trapelim's hull-based folding and to SCCP. This pass re-derives the
// SOURCE block's exit state, refines it along the specific out-edge (the
// source branch's own comparison holds as a fact on its taken edge), walks
// the elbow's phis to the incoming values, and threads the edge.
//
// Soundness envelope (v1):
// * the elbow body is exactly phis + Consts + optionally the single-use
//   comparison feeding its Cbr (the comparison may also be defined in a
//   dominator; then the body is phis + Consts + Cbr); no other instruction
//   may be present;
// * every vreg DEFINED inside the elbow is used only INSIDE the elbow:
//   the threaded path skips those defs, so nothing outside may read them;
//   (defs in dominators stay available -- a dominator of the elbow also
//   dominates the source block, so its instrs ran on the threaded path);
// * the taken target carries NO phis: this IR keys phi args by pred id
//   with exactly two slots, and the threaded edge would add a third pred;
// * the taken target is not the source block itself (self-edges would
//   fabricate a cycle the ranges did not prove terminating);
// * decisions are all made on the PRISTINE analysis and applied afterwards
//   in one batch: a redirect cannot change any block's instruction list,
//   and the state on OTHER edges is unaffected (the skipped defs are
//   elbow-local and dead on the threaded path -- the phi-less target
//   guarantee makes the redirect invisible downstream);
// * after the sweep, tier-0's unreach degenerates elbow phis whose pred
//   edge went away, and splice/dce collapse whatever is left of the elbow.
constexpr bool opt_jthread(Fn &fn, const An &A, const Ranges &RA,
                           OptStats &os) {
  if (!RA.ok)
    return false;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nb || !nv || fn.code.empty())
    return false;
  std::vector<uint32_t> defi(nv, NONE), raw(nv, 0);
  for (uint32_t i = 0; i < fn.code.size(); ++i) {
    const Instr &I = fn.code[i];
    if (I.a != NONE && I.a < nv)
      raw[I.a]++;
    if (I.b2 != NONE && I.b2 < nv)
      raw[I.b2]++;
    if (I.c2 != NONE && I.c2 < nv)
      raw[I.c2]++;
    for (uint32_t v : I.args)
      if (v != NONE && v < nv)
        raw[v]++;
    if (I.dst != NONE && I.dst < nv)
      defi[I.dst] = i;
  }
  // the single-use, single-def comparison behind a Cbr condition (the same
  // predicate range_analyze uses for its branch refinement)
  auto is_cmp_def = [&](uint32_t c) -> uint32_t {
    if (c == NONE || c >= nv)
      return NONE;
    if (raw[c] != 1 || defi[c] == NONE)
      return NONE;
    const Instr &D = fn.code[defi[c]];
    if (D.k != K::Bin)
      return NONE;
    if (D.b == B::Lt || D.b == B::Le || D.b == B::Gt || D.b == B::Ge ||
        D.b == B::Eq || D.b == B::Ne)
      return defi[c];
    return NONE;
  };
  struct Th {
    uint32_t a, slot, tgt;
  };
  std::vector<Th> ths{};
  std::vector<Iv> st(nv), st2(nv);
  for (uint32_t a = 0; a < nb; ++a) {
    const uint32_t ab = fn.blocks[a].begin, ae = fn.blocks[a].end;
    if (ae <= ab)
      continue;
    const Instr &TA = fn.code[ae - 1];
    const bool is_cbr = (TA.k == K::Cbr);
    if (!is_cbr && TA.k != K::Br)
      continue; // Exit-terminated block
    if (is_cbr && fn.succ0(a) == fn.succ1(a))
      continue; // both arms equal: tier-0 folds that
    // exit state of a (block-entry phi rule already applied by RA)
    for (uint32_t v = 0; v < nv; ++v)
      st[v] = RA.entry[(size_t)a * nv + v];
    for (uint32_t i = ab; i < ae; ++i)
      iv_transfer(fn, i, nv, st);
    const uint32_t nslots = is_cbr ? 2 : 1;
    for (uint32_t slot = 0; slot < nslots; ++slot) {
      const uint32_t b = slot ? fn.succ1(a) : fn.succ0(a);
      if (b == NONE || b == a || b >= nb)
        continue;
      // ---- elbow shape: phis + optional local cmp def + Cbr --------------
      const uint32_t bb = fn.blocks[b].begin, be = fn.blocks[b].end;
      if (be <= bb)
        continue;
      const Instr &TB = fn.code[be - 1];
      if (TB.k != K::Cbr || TB.t == NONE || TB.f == NONE)
        continue;
      if (TB.t == b || TB.f == b)
        continue; // degenerate self-targets
      const uint32_t d = is_cmp_def(TB.a);
      if (d == NONE)
        continue;
      const Instr &D = fn.code[d];
      bool body_ok = true;
      for (uint32_t i = bb; i + 1 < be; ++i) { // everything but the Cbr
        const Instr &I = fn.code[i];
        if (I.k == K::Phi || I.k == K::Const || i == d)
          continue; // phis, materialized comparison constants, the compare
        body_ok = false;
        break;
      }
      if (!body_ok)
        continue;
      // ---- decide the comparison on THIS edge ----------------------------
      st2 = st;
      if (is_cbr) { // the source branch's comparison holds as a fact
        const uint32_t dc = is_cmp_def(TA.a);
        if (dc != NONE) {
          const Instr &DC = fn.code[dc];
          Iv x = (DC.a != NONE && DC.a < nv) ? st2[DC.a]
                                             : Iv{INT64_MIN, INT64_MAX, false};
          Iv y = (DC.b2 != NONE && DC.b2 < nv)
                     ? st2[DC.b2]
                     : Iv{INT64_MIN, INT64_MAX, false};
          iv_refine(x, y, DC.b, slot == 0);
          if (DC.a != NONE && DC.a < nv)
            st2[DC.a] = x;
          if (DC.b2 != NONE && DC.b2 < nv)
            st2[DC.b2] = y;
        }
      }
      // operand -> interval on this edge: an outer vreg keeps its exit
      // interval (a def that dominates the elbow but is not inside it also
      // dominates the source block, so the value on the edge is exactly the
      // exit state's); an elbow phi resolves to its a-edge incoming vreg;
      // an elbow-local Const is its own interval; anything else defined in
      // the elbow is too deep for v1
      auto op_iv = [&](uint32_t v, Iv &out) -> bool {
        if (v == NONE || v >= nv)
          return false;
        const uint32_t di = defi[v];
        if (di == NONE || di < bb || di >= be || di == d) {
          out = st2[v]; // param / dominator def / the comparison itself
          return true;
        }
        const Instr &P = fn.code[di];
        if (P.k == K::Const) {
          out = {(val_t)P.c, (val_t)P.c, false};
          return true;
        }
        if (P.k == K::Phi) {
          uint32_t arg = NONE; // the incoming value along a -> b
          if (P.t == a && P.a != NONE)
            arg = P.a;
          else if (P.f == a && P.b2 != NONE)
            arg = P.b2;
          if (arg == NONE || arg >= nv)
            return false; // a not a phi pred slot (cannot happen in SSA)
          out = st2[arg]; // defined in/before a: valid in the exit state
          return true;
        }
        return false; // elbow-local non-const non-phi def
      };
      Iv xi{}, yi{};
      if (!op_iv(D.a, xi) || !op_iv(D.b2, yi))
        continue;
      val_t out{};
      if (!iv_decide(D.b, xi, yi, out))
        continue;
      const uint32_t tgt = out ? TB.t : TB.f;
      if (tgt == NONE || tgt == b || tgt == a || tgt >= nb)
        continue;
      // ---- guards on the taken target ------------------------------------
      bool tgt_phi = false;
      for (uint32_t i = fn.blocks[tgt].begin; i < fn.blocks[tgt].end; ++i)
        if (fn.code[i].k == K::Phi) {
          tgt_phi = true;
          break;
        }
      if (tgt_phi)
        continue; // a third pred would be unrepresentable in the phis
      bool already_pred = false;
      for (uint32_t q : fn.preds[tgt])
        if (q == a) {
          already_pred = true;
          break;
        }
      if (already_pred)
        continue; // phi-value ambiguity on the existing edge
      // ---- outside-use check: elbow defs must die with the elbow ---------
      bool leak = false;
      for (uint32_t i = 0; i < fn.code.size() && !leak; ++i) {
        if (i >= bb && i < be)
          continue; // uses inside the elbow are fine
        const Instr &I = fn.code[i];
        auto chk = [&](uint32_t v) {
          if (v == NONE || v >= nv)
            return;
          const uint32_t di = defi[v];
          if (di != NONE && di >= bb && di < be)
            leak = true;
        };
        chk(I.a);
        chk(I.b2);
        chk(I.c2);
        for (uint32_t v : I.args)
          chk(v);
      }
      if (leak)
        continue;
      ths.push_back({a, slot, tgt});
    }
  }
  if (ths.empty())
    return false;
  for (const Th &t : ths) {
    Instr &TA = fn.code[fn.blocks[t.a].end - 1];
    if (t.slot == 0) {
      TA.t = t.tgt;
      fn.succ[2 * t.a] = t.tgt; // succ is the canonical CFG: keep it in
    } else { //   lockstep with the terminator (unreach/splice/an read it)
      TA.f = t.tgt;
      fn.succ[2 * t.a + 1] = t.tgt;
    }
    os.jthreads++;
  }
  fn.recompute_preds();
  return true;
}
