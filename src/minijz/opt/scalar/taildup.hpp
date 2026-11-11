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
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"

// the growth cap: the body duplicates into BOTH preds (2x) before tier-0
// folding shrinks it — keep the per-candidate growth <= 12 instrs; the
// correlated-merge case (both phi args constant) folds to near-nothing
inline constexpr uint32_t TAILDUP_BODY_MAX = 6;

constexpr bool opt_taildup(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv)
    return false;
  for (uint32_t T = 0; T < nb; ++T) {
    // ---- the two (unconditional) predecessors --------------------------
    if (fn.preds[T].size() != 2)
      continue;
    const uint32_t P0 = fn.preds[T][0], P1 = fn.preds[T][1];
    if (P0 == P1 || P0 == T || P1 == T || P0 >= nb || P1 >= nb)
      continue;
    if (A.inner[T] == T)
      continue; // a loop header's duplication is loop rotation (see the
                // header comment) — deliberately not this pass
    bool preds_ok = true;
    for (uint32_t P : {P0, P1}) {
      const Instr &pt = fn.code[fn.blocks[P].end - 1];
      if (pt.k != K::Br || pt.t != T || fn.succ0(P) != T || fn.succ1(P) != NONE)
        preds_ok = false;
    }
    if (!preds_ok)
      continue;
    // ---- T's content: phis (both keys), small pure body, term -----------
    if (fn.blocks[T].begin == fn.blocks[T].end)
      continue; // paranoia: empty blocks carry no terminator
    std::vector<uint32_t> phi_dst{}, phi_a{}, phi_b{}; // keyed (P0,P1)
    uint32_t body = 0;
    bool ok = true;
    for (uint32_t i = fn.blocks[T].begin; i < fn.blocks[T].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Phi) {
        if (i + 1 == fn.blocks[T].end)
          ok = false; // a phi cannot terminate
        if (ok) {
          const uint32_t a0 = (I.t == P0) ? I.a : ((I.f == P0) ? I.b2 : NONE);
          const uint32_t a1 = (I.t == P1) ? I.a : ((I.f == P1) ? I.b2 : NONE);
          if (a0 == NONE || a1 == NONE || a0 >= nv || a1 >= nv)
            ok = false; // not keyed to the two preds
          else {
            phi_dst.push_back(I.dst);
            phi_a.push_back(a0);
            phi_b.push_back(a1);
          }
        }
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
      continue;
    const Instr tterm = fn.code[fn.blocks[T].end - 1];
    // ---- the continuation(s): each successor's ONLY pred must be T ------
    uint32_t U0 = NONE, U1 = NONE;
    if (tterm.k == K::Br) {
      U0 = tterm.t;
      if (U0 == NONE || U0 >= nb)
        continue;
    } else if (tterm.k == K::Cbr) {
      U0 = tterm.t;
      U1 = tterm.f;
      if (U0 == NONE || U0 >= nb || U1 == NONE || U1 >= nb)
        continue;
      if (U1 == U0)
        U1 = NONE; // degenerate two-arm cbr: one continuation
    } // Exit/Ret: no continuation (U0/U1 stay NONE)
    bool succs_ok = true;
    for (uint32_t U : {U0, U1}) {
      if (U == NONE)
        continue;
      if (U == T || U == P0 || U == P1 || U >= nb)
        succs_ok = false;
      else if (fn.preds[U].size() != 1 || fn.preds[U][0] != T)
        succs_ok = false;
      else if (fn.blocks[U].begin != fn.blocks[U].end &&
               fn.code[fn.blocks[U].begin].k == K::Phi)
        succs_ok = false; // single-pred phis would tangle the insert
    }
    if (!succs_ok)
      continue;
    // ---- the escaping values and their routing --------------------------
    // T-defined values: phi dsts (per-copy values: the keyed args) and
    // body dsts (per-copy values: fresh vregs allocated below).
    std::vector<uint32_t> defv{};             // T-defined value ids
    std::vector<uint32_t> local0{}, local1{}; // per-copy value (P0 / P1)
    for (size_t k = 0; k < phi_dst.size(); ++k) {
      defv.push_back(phi_dst[k]);
      local0.push_back(phi_a[k]);
      local1.push_back(phi_b[k]);
    }
    for (uint32_t i = fn.blocks[T].begin; i + 1 < fn.blocks[T].end; ++i)
      if (fn.code[i].dst != NONE && fn.code[i].k != K::Phi) {
        defv.push_back(fn.code[i].dst);
        local0.push_back(NONE); // filled once the fresh copy vregs exist
        local1.push_back(NONE);
      }
    // route[b]: WHERE a block reads T-defined values from — 0 (U0's
    // routing phi), 1 (U1's), 2+k (the k-th born site's phi), -1
    // (unroutable: a use there bails the candidate). Computed by
    // dominance plus a forward fixpoint: a block dominated by exactly
    // one continuation reads its phi; a single-pred chain passes its
    // pred's supplier down; a 2-pred merge whose preds share a supplier
    // reads it, and one whose preds arrive from DIFFERENT suppliers gets
    // a fresh phi BORN there (one arg per pred, each arg the pred's own
    // supplier). The loop-latch feedback edge (the merge value feeding
    // the loop header's phi through the post-arm join) is exactly the
    // born-site shape — it is what makes in-loop merges duplicable.
    struct BSite {
      uint32_t W, p0, p1;
      int r0, r1; // the preds' supplier routes
    };
    std::vector<BSite> bsites{};
    std::vector<int> route(nb, -2);
    for (uint32_t b = 0; b < nb; ++b) {
      if (b == T || b == P0 || b == P1) {
        route[b] = -1;
        continue;
      }
      const bool d0 = (U0 != NONE) && A.dominates(U0, b);
      const bool d1 = (U1 != NONE) && A.dominates(U1, b);
      if (d0 && !d1)
        route[b] = 0;
      else if (d1 && !d0)
        route[b] = 1;
    }
    for (uint32_t round = 0; round <= nb; ++round) {
      bool progressed = false;
      for (uint32_t b = 0; b < nb; ++b) {
        if (route[b] != -2)
          continue;
        const std::vector<uint32_t> &ps = fn.preds[b];
        bool any_unres = false, any_dead = false;
        for (uint32_t p : ps) {
          if (route[p] == -2)
            any_unres = true;
          else if (route[p] < 0)
            any_dead = true;
        }
        if (any_unres)
          continue; // wait for the pred's round
        if (ps.empty() || any_dead || ps.size() > 2) {
          route[b] = -1; // entry / dead-fed / >2 preds: no phi possible
          progressed = true;
          continue;
        }
        if (ps.size() == 1) { // single-pred chain: the pred's supplier
          route[b] = route[ps[0]];
          progressed = true;
          continue;
        }
        if (route[ps[0]] == route[ps[1]]) { // both arms, one supplier
          route[b] = route[ps[0]];
          progressed = true;
          continue;
        }
        // a fresh phi is born at the re-merge, one arg per pred
        const int r0 = route[ps[0]], r1 = route[ps[1]];
        bsites.push_back({b, ps[0], ps[1], r0, r1});
        route[b] = (int)(2 + bsites.size() - 1);
        progressed = true;
      }
      if (!progressed)
        break;
    }
    for (int &r : route)
      if (r == -2)
        r = -1; // cycles among unresolved blocks: unroutable
    // ---- scan every outside use of T-defined values ---------------------
    std::vector<char> isdef(nv, 0);
    for (uint32_t v : defv)
      isdef[v] = 1;
    std::vector<char> need0(defv.size(), 0), need1(defv.size(), 0);
    std::vector<std::vector<uint32_t>> bvals{}; // per site: defv indices
    bvals.resize(bsites.size());
    // walk a supplier route: mark the uphis / born phis it reads (a born
    // phi's args read its preds' suppliers, recursively down to U0/U1's)
    auto mark_need = [&](int r, uint32_t di) {
      std::vector<int> stack{r};
      while (!stack.empty()) {
        const int x = stack.back();
        stack.pop_back();
        if (x == 0) {
          need0[di] = 1;
          continue;
        }
        if (x == 1) {
          need1[di] = 1;
          continue;
        }
        const size_t k = (size_t)(x - 2);
        if (std::find(bvals[k].begin(), bvals[k].end(), di) == bvals[k].end())
          bvals[k].push_back(di);
        stack.push_back(bsites[k].r0);
        stack.push_back(bsites[k].r1);
      }
    };
    bool bad = false;
    for (uint32_t b = 0; b < nb && !bad; ++b) {
      if (b == T || b == P0 || b == P1)
        continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        const bool isphi = (I.k == K::Phi);
        // a plain use reads where ITS block reads from; a phi arg reads
        // where the KEYED PRED reads from (the value holds at its end)
        auto chk = [&](uint32_t v, uint32_t key_pred) {
          if (v == NONE || v >= nv || !isdef[v] || bad)
            return;
          const int r = route[isphi ? key_pred : b];
          if (r < 0) {
            bad = true;
            return;
          }
          const size_t di =
              (size_t)(std::find(defv.begin(), defv.end(), v) - defv.begin());
          mark_need(r, (uint32_t)di);
        };
        if (isphi) { // arg a keyed pred t, arg b2 keyed pred f
          chk(I.a, I.t);
          chk(I.b2, I.f);
        } else {
          chk(I.a, NONE);
          chk(I.b2, NONE);
          chk(I.c2, NONE);
          for (uint32_t av : I.args)
            chk(av, NONE);
        }
      }
    }
    if (bad)
      continue;
    // ================= commit =================
    os.taildups++;
    // fresh vregs for the body defs' per-copy values
    for (size_t di = 0; di < defv.size(); ++di)
      if (local0[di] == NONE) { // a body def
        local0[di] = new_anon_vreg(fn, fn.base_of(defv[di]));
        local1[di] = new_anon_vreg(fn, fn.base_of(defv[di]));
      }
    // per-continuation routing phis (U has no phis of its own: single-pred)
    std::vector<uint32_t> uphi0(defv.size(), NONE), uphi1(defv.size(), NONE);
    for (uint32_t U : {U0, U1}) {
      if (U == NONE)
        continue;
      std::vector<Instr> phis{};
      for (size_t di = 0; di < defv.size(); ++di) {
        if (!((U == U0) ? need0[di] : need1[di]))
          continue; // no reader routes through this continuation
        const uint32_t d = new_anon_vreg(fn, fn.base_of(defv[di]));
        if (U == U0)
          uphi0[di] = d;
        else
          uphi1[di] = d;
        phis.push_back({.k = K::Phi,
                        .dst = d,
                        .a = local0[di],
                        .b2 = local1[di],
                        .t = P0,
                        .f = P1,
                        .c = (int64_t)fn.base_of(defv[di])});
      }
      if (!phis.empty()) { // prepend at U's top
        const uint32_t at = fn.blocks[U].begin;
        fn.code.insert(fn.code.begin() + (long)at, phis.begin(), phis.end());
        const uint32_t k = (uint32_t)phis.size();
        for (uint32_t x = 0; x < fn.nblocks(); ++x) {
          if (x == U) {
            fn.blocks[x].end += k;
            continue;
          }
          if (fn.blocks[x].begin >= at)
            fn.blocks[x].begin += k;
          if (fn.blocks[x].end > at)
            fn.blocks[x].end += k;
        }
      }
    }
    // born phis at the re-merge sites (site order = creation order, so a
    // site's args only reference EARLIER sites' phis and the uphis)
    std::vector<std::vector<uint32_t>> bphi(bsites.size());
    for (size_t k = 0; k < bsites.size(); ++k) {
      const BSite &S = bsites[k];
      std::vector<Instr> phis{};
      bphi[k].assign(defv.size(), NONE);
      for (uint32_t di : bvals[k]) {
        const uint32_t d = new_anon_vreg(fn, fn.base_of(defv[di]));
        bphi[k][di] = d;
        phis.push_back(
            {.k = K::Phi,
             .dst = d,
             .a = (S.r0 == 0) ? uphi0[di]
                              : ((S.r0 == 1) ? uphi1[di]
                                             : bphi[(size_t)(S.r0 - 2)][di]),
             .b2 = (S.r1 == 0) ? uphi0[di]
                               : ((S.r1 == 1) ? uphi1[di]
                                              : bphi[(size_t)(S.r1 - 2)][di]),
             .t = S.p0,
             .f = S.p1,
             .c = (int64_t)fn.base_of(defv[di])});
      }
      if (!phis.empty()) { // prepend (W may keep phis of its own below)
        const uint32_t at = fn.blocks[S.W].begin;
        fn.code.insert(fn.code.begin() + (long)at, phis.begin(), phis.end());
        const uint32_t kc = (uint32_t)phis.size();
        for (uint32_t x = 0; x < fn.nblocks(); ++x) {
          if (x == S.W) {
            fn.blocks[x].end += kc;
            continue;
          }
          if (fn.blocks[x].begin >= at)
            fn.blocks[x].begin += kc;
          if (fn.blocks[x].end > at)
            fn.blocks[x].end += kc;
        }
      }
    }
    // the two copies: substitute phi dsts -> keyed args, body dsts -> the
    // per-copy fresh vregs; P's `br T` terminator is REPLACED by T's
    // terminator (operands substituted), the body slides in before it.
    for (int side = 0; side < 2; ++side) {
      const uint32_t P = side ? P1 : P0;
      std::vector<Instr> tail{};
      for (uint32_t i = fn.blocks[T].begin; i < fn.blocks[T].end; ++i) {
        Instr c = fn.code[i];
        if (c.k == K::Phi)
          continue; // phis resolve to the keyed args, they do not copy
        auto sub = [&](uint32_t &v) {
          if (v == NONE || v >= nv)
            return;
          for (size_t di = 0; di < defv.size(); ++di)
            if (v == defv[di]) {
              v = side ? local1[di] : local0[di];
              return;
            }
        };
        if (c.dst != NONE && c.dst < nv) // the copy's defs are its own
          for (size_t di = 0; di < defv.size(); ++di)
            if (c.dst == defv[di]) {
              c.dst = side ? local1[di] : local0[di];
              break;
            }
        sub(c.a);
        sub(c.b2);
        if (c.k != K::Cbr && c.k != K::Br && c.c2 != NONE)
          sub(c.c2);
        for (uint32_t &av : c.args)
          sub(av);
        tail.push_back(c);
      }
      // replace P's terminator with the substituted terminator, body before
      const uint32_t term_pos = fn.blocks[P].end - 1;
      fn.code.erase(fn.code.begin() + (long)term_pos);
      fn.code.insert(fn.code.begin() + (long)term_pos, tail.begin(),
                     tail.end());
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
      // P's successors are T's (the substituted terminator's targets)
      const Instr &nt = fn.code[fn.blocks[P].end - 1];
      fn.succ[2 * P] = nt.t;
      fn.succ[2 * P + 1] = (nt.k == K::Cbr) ? nt.f : NONE;
    }
    // ---- reroute the outside uses --------------------------------------
    for (uint32_t b = 0; b < nb; ++b) {
      if (b == T || b == P0 || b == P1)
        continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        const bool isphi = (I.k == K::Phi);
        auto rw = [&](uint32_t &v, uint32_t key_pred) {
          if (v == NONE || v >= nv || !isdef[v])
            return;
          const int r = route[isphi ? key_pred : b];
          if (r < 0)
            return; // validated above: does not happen
          for (size_t di = 0; di < defv.size(); ++di)
            if (v == defv[di]) {
              v = (r == 0)   ? uphi0[di]
                  : (r == 1) ? uphi1[di]
                             : bphi[(size_t)(r - 2)][di];
              return;
            }
        };
        if (isphi) {
          rw(I.a, I.t);
          rw(I.b2, I.f);
        } else {
          rw(I.a, NONE);
          rw(I.b2, NONE);
          rw(I.c2, NONE);
          for (uint32_t &av : I.args)
            rw(av, NONE);
        }
      }
    }
    fn.recompute_preds();
    return true; // one duplication per call: the driver re-analyzes
  }
  return false;
}
