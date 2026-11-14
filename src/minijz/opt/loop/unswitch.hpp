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
//   version-merge block. The scan + commit live in unswitch_bif.hpp;
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
// loop_shape_of (header test NOT invariant -> unswitch_bif.hpp's
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
#include "unswitch_shape.hpp"

constexpr bool opt_unswitch(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const LoopScan S = loop_scan(fn, A);
  const uint32_t nb = S.nb, nv = S.nv;
  if (!nv)
    return false;
  for (uint32_t h : S.headers) {
    std::vector<char> inL;
    in_loop_flags(fn, A, h, inL);
    // ---- path selection ---------------------------------------------
    const LoopShape sh = loop_shape_of(fn, A, inL, h);
    uint32_t pre, latch, X, primary, bypass, entry_pred, false_pred;
    uint32_t guard1 = NONE, guard2 = NONE;
    bool compound = false, is_and = false;
    if (sh.ok && !cond_invariant(fn, A, inL, S, sh.pre,
                                 fn.code[fn.blocks[h].end - 1].a)) {
      // the header test varies per iteration — but an invariant INNER
      // branch still versions the loop (both versions keep running)
      const BifShape bs = body_if_shape_of(fn, A, inL, S, h, sh.pre, sh.latch);
      if (bs.ok && unswitch_body_if(fn, A, os, S, inL, h, sh, bs))
        return true; // one loop per call: the driver re-analyzes
      continue;      // not trivial (cond varies), not body-if: skip
    }
    {
      if (sh.ok) {
        pre = sh.pre;
        latch = sh.latch;
        X = sh.X;
        primary = h; // the zero-iteration edge is h's exit arm
        bypass = sh.body;
        entry_pred = pre;
        false_pred = pre;
        guard1 = fn.code[fn.blocks[h].end - 1].a;
      } else {
        const CompShape cs = compound_shape_of(fn, A, inL, S, h);
        if (!cs.ok)
          continue;
        pre = cs.pre;
        latch = cs.latch;
        X = cs.X;
        primary = cs.mm; // the zero-iteration edge is mm's exit arm
        bypass = cs.body;
        compound = true;
        is_and = cs.is_and;
        guard1 = cs.c1;
        guard2 = cs.c2;
        entry_pred = NONE; // filled once the guard blocks exist
        false_pred = NONE;
      }
    }
    // ---- the invariant condition(s) -----------------------------------
    if (!cond_invariant(fn, A, inL, S, pre, guard1))
      continue;
    if (compound && !cond_invariant(fn, A, inL, S, pre, guard2))
      continue;
    // ---- the preheader must be a dedicated `br h` block ---------------
    {
      const Instr &pt = fn.code[fn.blocks[pre].end - 1];
      if (pt.k != K::Br || pt.t != h)
        continue;
      if (fn.succ0(pre) != h || fn.succ1(pre) != NONE)
        continue;
    }
    // ---- growth cap ---------------------------------------------------
    uint32_t loop_instrs = 0;
    for (uint32_t b = 0; b < nb; ++b)
      if (inL[b])
        loop_instrs += fn.blocks[b].end - fn.blocks[b].begin;
    if (loop_instrs == 0 || loop_instrs > 120)
      continue;
    // ---- exit-edge inventory (breaks allowed; Ret/Exit have no succ) --
    std::vector<ExitEdge> exits{};
    if (!exit_inventory(fn, inL, nb, primary, X, pre, h, exits))
      continue;
    // ---- header phis keyed (pre, latch) -------------------------------
    std::vector<HPhi> hphis;
    if (!header_phis(fn, h, pre, latch, hphis))
      continue;
    // ---- false-path region F: reachable from X loop-free --------------
    std::vector<char> F(nb, 0);
    {
      std::vector<uint32_t> work{X};
      F[X] = 1;
      while (!work.empty()) {
        uint32_t b = work.back();
        work.pop_back();
        for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
          if (s == NONE || inL[s] || F[s])
            continue;
          F[s] = 1;
          work.push_back(s);
        }
      }
    }
    // ---- safety scan over outside uses of loop-defined values ---------
    // kinds: (a) plain use in F of a header phi -> merge value;
    // (b) plain use outside F -> remap to the clone;
    // (c) phi arg keyed to an in-loop pred -> rekey + remap (except the
    //     primary edge: header phis take their pre_arg, other header
    //     defs bail);
    // (d) phi arg keyed to an outside pred: remap when the pred is not
    //     in F (the value left through the loop), nothing when it is
    //     (dominance forbids loop values there).
    std::vector<char> loopdef(nv, 0);
    for (uint32_t v = 0; v < nv; ++v)
      if (S.defi[v] != NONE && inL[S.defb[v]])
        loopdef[v] = 1;
    std::vector<char> read_on_false(hphis.size(), 0);
    bool bad = false;
    for (uint32_t b = 0; b < nb && !bad; ++b) {
      if (inL[b] || b == pre)
        continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end && !bad; ++i) {
        const Instr &I = fn.code[i];
        // is v a header phi? returns its index or NONE
        auto hphi_idx = [&](uint32_t v) -> uint32_t {
          if (v == NONE || v >= nv || !loopdef[v])
            return NONE;
          for (size_t pi = 0; pi < hphis.size(); ++pi)
            if (hphis[pi].dst == v)
              return (uint32_t)pi;
          return NONE;
        };
        if (I.k == K::Phi) {
          // arg a is keyed to pred t; arg b2 to pred f
          auto arg_ok = [&](uint32_t v, uint32_t pred) -> bool {
            if (v == NONE || v >= nv || !loopdef[v])
              return true;
            if (pred < nb && inL[pred]) {
              if (pred == primary) {        // the zero-iteration edge:
                return hphi_idx(v) != NONE; // only header phis have an
              }                             // entry value to carry; others bail
              return true;                  // break edge: rekey + remap
            }
            if (F[b] && pred < nb && F[pred])
              return hphi_idx(v) != NONE; // dominance forbids this for
            // non-phi defs; treat like a false-path read (merge value)
            return true; // through-only pred: remap during commit
          };
          if (!arg_ok(I.a, I.t) || !arg_ok(I.b2, I.f))
            bad = true;
          // a phi arg delivered from an F pred on the false path reads
          // the merge value: mark the reader (merge phis at X handle
          // the plain uses; phi args get rewritten in the same sweep)
          if (F[b] && I.t < nb && F[I.t] && I.a != NONE && I.a < nv &&
              loopdef[I.a]) {
            uint32_t pi = hphi_idx(I.a);
            if (pi != NONE)
              read_on_false[pi] = 1;
          }
          if (F[b] && I.f < nb && F[I.f] && I.b2 != NONE && I.b2 < nv &&
              loopdef[I.b2]) {
            uint32_t pi = hphi_idx(I.b2);
            if (pi != NONE)
              read_on_false[pi] = 1;
          }
        } else {
          auto plain_ok = [&](uint32_t v) -> bool {
            if (v == NONE || v >= nv || !loopdef[v])
              return true;
            if (!F[b])
              return true;              // through-only: remap
            return hphi_idx(v) != NONE; // on the false path only header
          };                            // phis have a value to read
          if (!plain_ok(I.a) || !plain_ok(I.b2) || !plain_ok(I.c2))
            bad = true;
          else
            for (uint32_t v : I.args)
              if (!plain_ok(v)) {
                bad = true;
                break;
              }
          if (!bad) { // remember false-path readers (merge candidates)
            auto mark = [&](uint32_t v) {
              if (v == NONE || v >= nv || !loopdef[v] || !F[b])
                return;
              uint32_t pi = hphi_idx(v);
              if (pi != NONE)
                read_on_false[pi] = 1;
            };
            mark(I.a);
            mark(I.b2);
            mark(I.c2);
            for (uint32_t v : I.args)
              mark(v);
          }
        }
      }
    }
    if (bad)
      continue;
    // ================= commit =================
    os.unswitches++;
    // -- compound guard blocks (allocated BEFORE the clone map so the
    //    clone ids and the header-phi entry rekey are stable) ---------
    // && : pre: cbr(c1) g2/fx ; g2: cbr(c2) H'/fx ; fx: br X
    // || : pre: cbr(c1) t1/g2 ; g2: cbr(c2) t1/fx ; t1: br H' ; fx: br X
    uint32_t g2 = NONE, fx = NONE, t1 = NONE;
    if (compound) {
      const uint32_t nb0 = fn.nblocks();
      g2 = nb0;
      fx = nb0 + 1;
      fn.blocks.push_back({});
      fn.succ.push_back(NONE);
      fn.succ.push_back(NONE);
      fn.blocks.push_back({});
      fn.succ.push_back(NONE);
      fn.succ.push_back(NONE);
      if (!is_and) {
        t1 = nb0 + 2;
        fn.blocks.push_back({});
        fn.succ.push_back(NONE);
        fn.succ.push_back(NONE);
      }
      entry_pred = is_and ? g2 : t1;
      false_pred = fx;
    }
    // -- the clone: every in-loop block gets a fresh id ------------------
    const CloneMap C = alloc_clone(fn, S, inL);
    emit_loop_clone(fn, S, inL, C, h, pre, entry_pred, bypass);
    // -- the guards ------------------------------------------------------
    if (!compound) {
      Instr &pt = fn.code[fn.blocks[pre].end - 1];
      pt.k = K::Cbr;
      pt.a = guard1;
      pt.t = C.bmap[h];
      pt.f = X;
      fn.succ[2 * pre] = C.bmap[h];
      fn.succ[2 * pre + 1] = X;
    } else if (is_and) {
      Instr &pt = fn.code[fn.blocks[pre].end - 1];
      pt.k = K::Cbr;
      pt.a = guard1;
      pt.t = g2;
      pt.f = fx;
      fn.succ[2 * pre] = g2;
      fn.succ[2 * pre + 1] = fx;
      fn.blocks[g2].begin = (uint32_t)fn.code.size();
      fn.code.push_back({.k = K::Cbr, .a = guard2, .t = C.bmap[h], .f = fx});
      fn.blocks[g2].end = (uint32_t)fn.code.size();
      fn.succ[2 * g2] = C.bmap[h];
      fn.succ[2 * g2 + 1] = fx;
    } else { // ||: the true arms short-circuit into the loop
      Instr &pt = fn.code[fn.blocks[pre].end - 1];
      pt.k = K::Cbr;
      pt.a = guard1;
      pt.t = t1;
      pt.f = g2;
      fn.succ[2 * pre] = t1;
      fn.succ[2 * pre + 1] = g2;
      fn.blocks[g2].begin = (uint32_t)fn.code.size();
      fn.code.push_back({.k = K::Cbr, .a = guard2, .t = t1, .f = fx});
      fn.blocks[g2].end = (uint32_t)fn.code.size();
      fn.succ[2 * g2] = t1;
      fn.succ[2 * g2 + 1] = fx;
    }
    // fx: br X (the zero-iteration edge's new outside pred; compound
    // shapes only — the trivial shape jumps pre -> X directly)
    if (compound) {
      fn.blocks[fx].begin = (uint32_t)fn.code.size();
      fn.code.push_back({.k = K::Br, .t = X});
      fn.blocks[fx].end = (uint32_t)fn.code.size();
      fn.succ[2 * fx] = X;
      fn.succ[2 * fx + 1] = NONE;
      if (t1 != NONE) { // ||: t1: br H'
        fn.blocks[t1].begin = (uint32_t)fn.code.size();
        fn.code.push_back({.k = K::Br, .t = C.bmap[h]});
        fn.blocks[t1].end = (uint32_t)fn.code.size();
        fn.succ[2 * t1] = C.bmap[h];
        fn.succ[2 * t1 + 1] = NONE;
      }
    }
    // -- merge values for header phis read on the false path ------------
    // X's post-unswitch preds: the new false-side pred (pre / fx) +
    // X's ORIGINAL outside preds (the break path may enter X through
    // unchanged outside blocks — the if-arm `br` block — so the
    // clone-side key is that block id, not a clone id; at most one by
    // the two-pred invariant; an in-loop side pred is rekeyed to its
    // clone by the sweep below). With a side pred a fresh phi at X
    // merges (false-side arg, the cloned value); without one the
    // false-side arg IS the value (X is single-pred then).
    uint32_t x_side_pred = NONE;
    for (uint32_t p : fn.preds[X])
      if (p != primary)
        x_side_pred = p;
    std::vector<uint32_t> phi_final(hphis.size(), NONE);
    for (size_t pi = 0; pi < hphis.size(); ++pi) {
      if (!read_on_false[pi])
        continue;
      const HPhi &P = hphis[pi];
      if (x_side_pred == NONE) {
        phi_final[pi] = P.pre_arg; // single-pred X: the value is direct
        continue;
      }
      const uint32_t d = new_anon_vreg(fn, fn.base_of(P.dst));
      const Instr np{.k = K::Phi,
                     .dst = d,
                     .a = P.pre_arg,       // arrives on the zero-iter path
                     .b2 = C.fresh[P.dst], // arrives from the side pred
                     .t = false_pred,      // (true path, after the loop ran)
                     .f = x_side_pred,
                     .c = (int64_t)fn.base_of(P.dst)};
      const uint32_t at = fn.blocks[X].begin;
      fn.code.insert(fn.code.begin() + (long)at, np);
      for (uint32_t x = 0; x < fn.nblocks(); ++x) {
        if (x == X)
          continue;
        if (fn.blocks[x].begin >= at)
          fn.blocks[x].begin++;
        if (fn.blocks[x].end > at)
          fn.blocks[x].end++;
      }
      fn.blocks[X].end++;
      phi_final[pi] = d;
    }
    // -- rewire outside uses ---------------------------------------------
    for (uint32_t b = 0; b < nb; ++b) {
      if (inL[b] || b == pre)
        continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.k == K::Phi) {
          auto rk = [&](uint32_t &key, uint32_t &arg) {
            if (key == NONE)
              return;
            if (key == primary && b == X) { // the zero-iteration edge:
              // the false-side pred is born (pre / fx). MUST precede
              // the in-loop branch below: the primary block is
              // in-loop too, but its edge is the exit being replaced,
              // not a break edge to rekey to a clone.
              key = false_pred;
              for (size_t pi = 0; pi < hphis.size(); ++pi)
                if (hphis[pi].dst == arg) {
                  arg = hphis[pi].pre_arg; // the entry value
                  break;
                }
              return;
            }
            if (key < nb && inL[key]) { // break edge: the clone enters
              arg = mapv(C, arg);
              key = C.bmap[key];
              return;
            }
            // outside pred: the value left through it only when it is
            // not false-reachable; remap. An F-pred arg of a header phi
            // reads the merge value (the scan marked exactly those).
            if (F[b] && key < nb && F[key]) {
              for (size_t pi = 0; pi < hphis.size(); ++pi)
                if (hphis[pi].dst == arg) {
                  // NONE would mean "undefined on this edge" (the scan
                  // marks every real reader; this is belt-and-braces)
                  arg = phi_final[pi] != NONE ? phi_final[pi] : NONE;
                  break;
                }
              return;
            }
            arg = mapv(C, arg);
          };
          rk(I.t, I.a);
          rk(I.f, I.b2);
          continue;
        }
        // plain uses: false-path readers of header phis take the merge
        // value; everything loop-defined else maps to its clone
        auto rw = [&](uint32_t &v) {
          if (v == NONE || v >= nv || !loopdef[v])
            return;
          if (F[b]) {
            for (size_t pi = 0; pi < hphis.size(); ++pi)
              if (hphis[pi].dst == v) {
                v = phi_final[pi]; // != NONE: the scan marked it
                return;
              }
          }
          v = mapv(C, v);
        };
        rw(I.a);
        rw(I.b2);
        rw(I.c2);
        for (uint32_t &av : I.args)
          rw(av);
      }
    }
    fn.recompute_preds();
    return true; // one loop per call: the driver re-analyzes
  }
  return false;
}
