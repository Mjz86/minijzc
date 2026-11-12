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
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "loop_shape.hpp"
#include "unswitch_shape.hpp" // exit_inventory

// the growth cap: the h-body duplicates into the guard AND the latch
// before tier-0 folding sees it (LLVM's rotation caps similarly)
inline constexpr uint32_t ROT_BODY_MAX = 8;

constexpr bool opt_rotate(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const LoopScan S = loop_scan(fn, A);
  const uint32_t nb = S.nb, nv = S.nv;
  if (!nv)
    return false;
  for (uint32_t h : S.headers) {
    std::vector<char> inL;
    in_loop_flags(fn, A, h, inL);
    // ---- the canonical shape + the dedicated preheader ----------------
    const LoopShape sh = loop_shape_of(fn, A, inL, h);
    if (!sh.ok || sh.latch == h)
      continue; // no self-loops
    const uint32_t pre = sh.pre, latch = sh.latch, X = sh.X, body = sh.body;
    {
      const Instr &pt = fn.code[fn.blocks[pre].end - 1];
      if (pt.k != K::Br || pt.t != h || fn.succ0(pre) != h ||
          fn.succ1(pre) != NONE)
        continue;
    }
    // ---- the ONLY exit edge is the header's; X entered only from h ----
    {
      std::vector<ExitEdge> exits{};
      if (!exit_inventory(fn, inL, nb, h, X, pre, h, exits) || !exits.empty())
        continue;
      if (fn.preds[X].size() != 1 || fn.preds[X][0] != h)
        continue;
      // a single-pred X carrying a phi is pathological (the tier-0
      // degenerater turns one-live-key phis into copies; a (h,h)-keyed
      // leftover would break the exit re-keying below) — and phis are
      // always a block's FIRST instrs (the phi_norm invariant), so one
      // look suffices. Bail, never assume.
      if (fn.blocks[X].begin < fn.blocks[X].end &&
          fn.code[fn.blocks[X].begin].k == K::Phi)
        continue;
    }
    // ---- header phis keyed (pre, latch) -------------------------------
    std::vector<HPhi> hphis;
    if (!header_phis(fn, h, pre, latch, hphis))
      continue;
    // ---- h's non-phi body: everything between the phis and the cbr ----
    const Instr hterm = fn.code[fn.blocks[h].end - 1];
    if (hterm.k != K::Cbr || hterm.a == NONE)
      continue;
    if (hterm.a < nv && S.defi[hterm.a] != NONE &&
        fn.code[S.defi[hterm.a]].k == K::Const)
      continue; // constant test: fold/sccp territory
    uint32_t nphi = 0;
    while (fn.blocks[h].begin + nphi < fn.blocks[h].end &&
           fn.code[fn.blocks[h].begin + nphi].k == K::Phi)
      ++nphi;
    std::vector<uint32_t> hbody{}; // instr indices, order preserved
    for (uint32_t i = fn.blocks[h].begin + nphi; i + 1 < fn.blocks[h].end; ++i)
      hbody.push_back(i);
    if (hbody.size() > ROT_BODY_MAX)
      continue;
    // ---- the HDEF set (h-phis + h-body defs) + loopdef ----------------
    std::vector<char> is_hdef(nv, 0), is_hphi(nv, 0), is_hbdef(nv, 0);
    for (const HPhi &P : hphis) {
      is_hdef[P.dst] = 1;
      is_hphi[P.dst] = 1;
    }
    for (uint32_t i : hbody) {
      const uint32_t d = fn.code[i].dst;
      if (d != NONE && d < nv) {
        is_hdef[d] = 1;
        is_hbdef[d] = 1;
      }
    }
    // an h-phi whose LATCH arg is a moved h-body def is a use of a moved
    // def: the back edge must carry the def's fresh header phi (the value
    // it computes per header position), never the moved def itself. The
    // marking happens below, once loop_read exists.
    std::vector<char> loopdef(nv, 0);
    for (uint32_t v = 0; v < nv; ++v)
      if (S.defi[v] != NONE && inL[S.defb[v]])
        loopdef[v] = 1;
    // ---- F: the after-loop region (reachable from X loop-free) --------
    std::vector<char> F(nb, 0);
    {
      std::vector<uint32_t> work{X};
      F[X] = 1;
      while (!work.empty()) {
        const uint32_t b = work.back();
        work.pop_back();
        for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
          if (s == NONE || inL[s] || F[s])
            continue;
          F[s] = 1;
          work.push_back(s);
        }
      }
    }
    // ---- the read classification + the safety scan --------------------
    // loop_read: h-body defs read by in-loop blocks (they get the fresh
    // header phis). f_read: HDEF values read in F (they get exit phis).
    // Any OTHER use of a loop-defined value outside the loop bails:
    // dominance made it impossible before the rotation — belt and braces
    // (the checkpoint-21 audit lesson: verify, never assume).
    std::vector<char> loop_read(nv, 0), f_read(nv, 0);
    // an h-phi whose LATCH arg is a moved h-body def reads that moved def:
    // the back edge must carry the def's fresh header phi (the value it
    // computes per header position), never the moved def itself
    for (const HPhi &P : hphis)
      if (P.latch_arg != NONE && P.latch_arg < nv && is_hbdef[P.latch_arg])
        loop_read[P.latch_arg] = 1;
    bool bad = false;
    for (uint32_t b = 0; b < nb && !bad; ++b) {
      if (!inL[b] || b == h)
        continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end && !bad; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Phi) { // args are uses keyed to the pred edges
          if (is_hbdef[I.a])
            loop_read[I.a] = 1;
          if (is_hbdef[I.b2])
            loop_read[I.b2] = 1;
          continue;
        }
        auto mark = [&](uint32_t v) {
          if (v != NONE && v < nv && is_hbdef[v])
            loop_read[v] = 1;
        };
        mark(I.a);
        mark(I.b2);
        mark(I.c2);
        for (uint32_t v : I.args)
          mark(v);
      }
    }
    for (uint32_t b = 0; b < nb && !bad; ++b) {
      if (inL[b] || b == pre)
        continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end && !bad; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Phi) {
          // arg a is keyed to pred t; arg b2 to pred f. A loop value can
          // only arrive through a pred in F (the post-loop region): the
          // arg then reads the exit phi. Any other delivery bails.
          auto chk = [&](uint32_t arg, uint32_t key) {
            if (arg == NONE || arg >= nv || !loopdef[arg])
              return;
            if (!is_hdef[arg]) {
              bad = true;
              return;
            }
            if (F[b] && key < nb && F[key]) {
              f_read[arg] = 1;
              return;
            }
            bad = true; // through-only pred: dominance forbids it
          };
          chk(I.a, I.t);
          chk(I.b2, I.f);
        } else {
          auto chk = [&](uint32_t v) {
            if (v == NONE || v >= nv || !loopdef[v])
              return;
            if (!is_hdef[v] || !F[b]) {
              bad = true;
              return;
            }
            f_read[v] = 1;
          };
          chk(I.a);
          chk(I.b2);
          chk(I.c2);
          for (uint32_t v : I.args)
            chk(v);
        }
      }
    }
    if (bad)
      continue;
    // ================= commit =================
    os.rotations++;
    // -- fresh vregs: guard/latch copies per h-body def, a header phi per
    //    loop-read h-body def, an exit phi per F-read HDEF value ---------
    std::vector<uint32_t> gv(nv, NONE), lv(nv, NONE); // h-body def -> copies
    std::vector<uint32_t> pv(nv, NONE);               // -> header phi
    std::vector<uint32_t> xv(nv, NONE);               // -> exit phi
    for (uint32_t i : hbody) {
      const uint32_t d = fn.code[i].dst;
      if (d != NONE && d < nv) {
        gv[d] = new_anon_vreg(fn, fn.base_of(d));
        lv[d] = new_anon_vreg(fn, fn.base_of(d));
      }
    }
    for (uint32_t v = 0; v < nv; ++v) {
      if (is_hbdef[v] && loop_read[v])
        pv[v] = new_anon_vreg(fn, fn.base_of(v));
      if (is_hdef[v] && f_read[v])
        xv[v] = new_anon_vreg(fn, fn.base_of(v));
    }
    // -- the value maps --------------------------------------------------
    // guard: h-phi -> its pre_arg (the entry value), h-body def -> the
    // guard's own copy; everything else (defs dominating the preheader)
    // stays
    auto gmap = [&](uint32_t v) -> uint32_t {
      if (v == NONE || v >= nv)
        return v;
      if (is_hphi[v]) {
        for (const HPhi &P : hphis)
          if (P.dst == v)
            return P.pre_arg;
      }
      if (is_hbdef[v] && gv[v] != NONE)
        return gv[v];
      return v;
    };
    // the latch copy: h-phi -> its latch_arg (the back-edge value), and
    // when that arg is itself a MOVED h-body def, -> the def's fresh header
    // phi (the value the moved def carries per header position — the back-
    // edge value of the phi, which is exactly what the moved instr must
    // read: the original def no longer exists in place)
    auto lmap = [&](uint32_t v) -> uint32_t {
      if (v == NONE || v >= nv)
        return v;
      if (is_hphi[v]) {
        for (const HPhi &P : hphis)
          if (P.dst == v) {
            if (P.latch_arg < nv && is_hbdef[P.latch_arg] &&
                pv[P.latch_arg] != NONE)
              return pv[P.latch_arg];
            return P.latch_arg;
          }
      }
      if (is_hbdef[v] && lv[v] != NONE)
        return lv[v];
      return v;
    };
    // in-loop rewrite: a moved h-body def reads as its fresh header phi
    auto rmap = [&](uint32_t v) -> uint32_t {
      if (v == NONE || v >= nv)
        return v;
      if (is_hbdef[v] && pv[v] != NONE)
        return pv[v];
      return v;
    };
    // F rewrite: an HDEF value reads as its exit phi
    auto xmap = [&](uint32_t v) -> uint32_t {
      if (v == NONE || v >= nv)
        return v;
      if (is_hdef[v] && xv[v] != NONE)
        return xv[v];
      return v;
    };
    // one instr's reads through a value map (phi args, or the operand
    // slots; Br carries no reads; Const's c is a value, not a vreg)
    auto rw = [](Instr &c, auto &m) {
      if (c.k == K::Phi) {
        c.a = m(c.a);
        c.b2 = m(c.b2);
        return;
      }
      if (c.k == K::Br)
        return;
      c.a = m(c.a);
      c.b2 = m(c.b2);
      if (c.c2 != NONE)
        c.c2 = m(c.c2);
      for (uint32_t &av : c.args)
        av = m(av);
    };
    // the edge test: h's original cbr with the cond mapped, one arm back
    // into the loop (h), one arm out (X) — the ORIGINAL polarity
    const bool body_on_true = (hterm.t == body);
    auto test_cbr = [&](uint32_t cond) {
      Instr c{};
      c.k = K::Cbr;
      c.a = cond;
      c.t = body_on_true ? h : X;
      c.f = body_on_true ? X : h;
      c.src = hterm.src;
      return c;
    };
    // -- rebuild the code vector (block ranges reallocated cleanly; the
    //    three rewritten blocks emit their new content, everything else
    //    copies verbatim — deterministic block/instr order) -------------
    std::vector<Instr> nc{};
    nc.reserve(fn.code.size() + 2 * hbody.size() + 8);
    for (uint32_t b = 0; b < nb; ++b) {
      const uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
      fn.blocks[b].begin = (uint32_t)nc.size();
      if (b == pre) {
        for (uint32_t i = ob; i + 1 < oe; ++i)
          nc.push_back(fn.code[i]); // pre's own instrs (phis untouched)
        for (uint32_t i : hbody) {  // the guard copy: the 1st header run
          Instr c = fn.code[i];
          rw(c, gmap);
          if (c.dst != NONE && c.dst < nv)
            c.dst = gv[c.dst]; // the COPY's fresh vreg (reads mapped above)
          nc.push_back(c);
        }
        nc.push_back(test_cbr(gmap(hterm.a))); // the guard
      } else if (b == h) {
        for (uint32_t i = ob; i < ob + nphi; ++i) {
          Instr c = fn.code[i]; // the phis: keys unchanged; a LATCH-side
          if (c.k == K::Phi) {  // arg that is a MOVED def re-routes to the
            // def's fresh header phi — the value it carries per position,
            // which is what the back edge must deliver
            if (c.t == latch && c.a < nv && is_hbdef[c.a] && pv[c.a] != NONE)
              c.a = pv[c.a];
            if (c.f == latch && c.b2 < nv && is_hbdef[c.b2] && pv[c.b2] != NONE)
              c.b2 = pv[c.b2];
          }
          nc.push_back(c);
        }
        for (uint32_t v = 0; v < nv; ++v) { // fresh phis for moved defs
          if (!is_hbdef[v] || !loop_read[v])
            continue;
          Instr c{};
          c.k = K::Phi;
          c.dst = pv[v];
          c.a = gv[v];  // first entry: the guard's copy
          c.b2 = lv[v]; // every back edge: the latch's copy
          c.t = pre;
          c.f = latch;
          c.c = (int64_t)fn.base_of(v);
          nc.push_back(c);
        }
        Instr br{}; // the test is gone from the header: fall into the body
        br.k = K::Br;
        br.t = body;
        br.src = hterm.src;
        nc.push_back(br);
      } else if (b == latch) {
        for (uint32_t i = ob; i + 1 < oe; ++i) { // the latch's own instrs
          Instr c = fn.code[i];
          rw(c, rmap); // moved-def reads -> the fresh header phis
          nc.push_back(c);
        }
        for (uint32_t i : hbody) { // the edge test: the (i+1)-th header run
          Instr c = fn.code[i];
          rw(c, lmap);
          if (c.dst != NONE && c.dst < nv)
            c.dst = lv[c.dst]; // the COPY's fresh vreg (reads mapped above)
          nc.push_back(c);
        }
        nc.push_back(test_cbr(lmap(hterm.a)));
      } else if (b == X) {
        for (uint32_t v = 0; v < nv; ++v) { // the exit phis
          if (!is_hdef[v] || !f_read[v])
            continue;
          Instr c{};
          c.k = K::Phi;
          c.dst = xv[v];
          if (is_hphi[v]) { // (pre_arg, latch_arg): the header phi's own
            for (const HPhi &P : hphis)
              if (P.dst == v) {
                c.a = P.pre_arg;
                // a MOVED latch arg leaves its latch copy in place: the
                // (k+1)-th computation, computed right before the exit
                c.b2 = (P.latch_arg < nv && is_hbdef[P.latch_arg] &&
                        lv[P.latch_arg] != NONE)
                           ? lv[P.latch_arg]
                           : P.latch_arg;
                break;
              }
          } else { // (guard copy, latch copy): the moved def's pair
            c.a = gv[v];
            c.b2 = lv[v];
          }
          c.t = pre;   // the zero-iteration edge: the entry value
          c.f = latch; // the back-edge exit: the after-iteration value
          c.c = (int64_t)fn.base_of(v);
          nc.push_back(c);
        }
        for (uint32_t i = ob; i < oe; ++i) { // X's own instrs
          Instr c = fn.code[i];
          rw(c, xmap);
          nc.push_back(c);
        }
      } else if (inL[b]) {
        for (uint32_t i = ob; i < oe; ++i) {
          Instr c = fn.code[i];
          rw(c, rmap);
          nc.push_back(c);
        }
      } else if (F[b]) {
        for (uint32_t i = ob; i < oe; ++i) {
          Instr c = fn.code[i];
          rw(c, xmap);
          nc.push_back(c);
        }
      } else {
        for (uint32_t i = ob; i < oe; ++i)
          nc.push_back(fn.code[i]);
      }
      fn.blocks[b].end = (uint32_t)nc.size();
    }
    fn.code = std::move(nc);
    // -- successors: pre cbr (h/X), h br body, latch cbr (h/X) ----------
    fn.succ[2 * pre] = body_on_true ? h : X;
    fn.succ[2 * pre + 1] = body_on_true ? X : h;
    fn.succ[2 * h] = body;
    fn.succ[2 * h + 1] = NONE;
    fn.succ[2 * latch] = body_on_true ? h : X;
    fn.succ[2 * latch + 1] = body_on_true ? X : h;
    fn.recompute_preds();
    return true; // one rotation per call: the driver re-analyzes
  }
  return false;
}
