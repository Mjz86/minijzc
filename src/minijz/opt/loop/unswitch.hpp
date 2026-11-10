#pragma once
// === unswitch.hpp ========================================
// trivial loop unswitching (README wishlist: "loop unswitching/
// versioning"): a loop whose HEADER branches on a condition that SSA
// already proves loop-invariant (defined outside the loop, dominating
// the preheader) is versioned on that condition —
//     pre: br h          ->   pre: cbr(cond) --> H' (true) / X (false)
//     h:   cbr(cond) body/X    H':  br body'   (the test is known true)
// The false path jumps straight to the header's exit target (the loop
// body never ran: the invariant condition fails at EVERY evaluation,
// so the original loop ran zero iterations). The true path is a full
// clone of the loop with fresh vregs whose header test is gone (the
// condition cannot change — it is an SSA value).
//
// Internal exit edges (`break`) are allowed: the clone keeps them, and
// every exit block's phis are rekeyed from the original in-loop pred
// to its clone (args remapped to the cloned defs). The header edge is
// special: X's h-keyed phi args become pre-edge args carrying each
// header phi's ENTRY value (what the zero-iteration original left in
// the variable), and the preheader edge is born.
//
// Outside uses are classified by false-path reachability F (blocks
// reachable from X without entering the loop): a header phi read
// there must see its preheader value, so a merge phi is inserted at X
// (or the read becomes the pre_arg directly when no cloned pred
// enters X); any other loop-defined value read there cannot exist
// (SSA dominance) and bails the loop. Reads outside F are only
// reachable after the loop ran: they are remapped to the clones.
// Ret/Exit inside the clone keep their (remapped) operands; Bkpt
// observations clone with it, so observation parity is preserved (the
// false path executes zero body instructions, exactly like the
// original with the condition false).
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/phi_norm.hpp"
#include "../analysis/an.hpp"
#include "loop_shape.hpp"

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
    const LoopShape sh = loop_shape_of(fn, A, inL, h);
    if (!sh.ok)
      continue;
    const uint32_t pre = sh.pre, latch = sh.latch, X = sh.X, body = sh.body;
    // ---- the invariant condition -------------------------------------
    const Instr hterm = fn.code[fn.blocks[h].end - 1];
    const uint32_t cond = hterm.a;
    if (cond == NONE || cond >= nv || S.defi[cond] == NONE)
      continue;
    if (inL[S.defb[cond]])
      continue; // defined inside: not invariant
    if (fn.code[S.defi[cond]].k == K::Const)
      continue; // constant test: fold/sccp territory
    if (S.defb[cond] != pre && !A.dominates(S.defb[cond], pre))
      continue; // not available at the preheader's end
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
    struct ExitEdge {
      uint32_t from, to;
    };
    std::vector<ExitEdge> exits{};
    bool bad = false;
    for (uint32_t b = 0; b < nb && !bad; ++b) {
      if (!inL[b])
        continue;
      for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
        if (s == NONE || inL[s])
          continue;
        if (b == h && s == X) // the header exit itself
          continue;
        exits.push_back({b, s});
        if (s == pre || s == h)
          bad = true; // structurally impossible; paranoia
      }
    }
    if (bad)
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
    //     h-key, which becomes the pre edge: header phis take their
    //     pre_arg, other header defs bail);
    // (d) phi arg keyed to an outside pred: remap when the pred is not
    //     in F (the value left through the loop), nothing when it is
    //     (dominance forbids loop values there).
    std::vector<char> loopdef(nv, 0);
    for (uint32_t v = 0; v < nv; ++v)
      if (S.defi[v] != NONE && inL[S.defb[v]])
        loopdef[v] = 1;
    std::vector<char> read_on_false(hphis.size(), 0);
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
              if (pred == h) {              // the header edge: pre is born
                return hphi_idx(v) != NONE; // only header phis have an
              } // entry value to carry; others bail
              return true; // break edge: rekey + remap
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
          }; // phis have a value to read
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
    // -- clone map: every in-loop block gets a fresh id ------------------
    std::vector<uint32_t> bmap(nb, NONE);
    for (uint32_t b = 0; b < nb; ++b) {
      if (!inL[b])
        continue;
      bmap[b] = fn.nblocks();
      fn.blocks.push_back({});
      fn.succ.push_back(NONE);
      fn.succ.push_back(NONE);
    }
    std::vector<uint32_t> fresh(nv, NONE);
    for (uint32_t v = 0; v < nv; ++v)
      if (S.defi[v] != NONE && inL[S.defb[v]])
        fresh[v] = new_anon_vreg(fn, fn.base_of(v));
    auto mapv = [&](uint32_t v) -> uint32_t {
      if (v == NONE || v >= nv)
        return v;
      return fresh[v] != NONE ? fresh[v] : v;
    };
    // -- emit the clone (append-only: existing ranges stay valid) -------
    for (uint32_t b = 0; b < nb; ++b) {
      if (!inL[b])
        continue;
      const uint32_t B = bmap[b];
      fn.blocks[B].begin = (uint32_t)fn.code.size();
      uint32_t hbr_tgt = NONE; // the header clone's Br target
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        Instr c = I;
        if (c.dst != NONE && c.dst < nv)
          c.dst = fresh[c.dst];
        if (c.k == K::Phi) {
          c.a = mapv(c.a); // pre_arg stays (outside def), latch remaps
          c.b2 = mapv(c.b2);
          if (c.t != NONE && inL[c.t])
            c.t = bmap[c.t];
          if (c.f != NONE && inL[c.f])
            c.f = bmap[c.f];
        } else if (i + 1 == fn.blocks[b].end &&
                   (c.k == K::Br || c.k == K::Cbr)) {
          // terminator: in-loop targets -> clones; exits stay
          c.a = mapv(c.a);
          if (c.t != NONE && inL[c.t])
            c.t = bmap[c.t];
          if (c.k == K::Cbr && c.f != NONE && inL[c.f])
            c.f = bmap[c.f];
        } else {
          c.a = mapv(c.a);
          c.b2 = mapv(c.b2);
          if (c.c2 != NONE)
            c.c2 = mapv(c.c2);
          for (uint32_t &av : c.args)
            av = mapv(av);
        }
        // the cloned header's test is GONE: Br to the body arm
        if (b == h && i + 1 == fn.blocks[b].end && c.k == K::Cbr) {
          const uint32_t tgt = (I.t == body) ? I.t : I.f;
          hbr_tgt = bmap[tgt]; // body is in-loop (== h for self-loops)
          c.k = K::Br;
          c.t = hbr_tgt;
          c.f = NONE;
          c.a = NONE;
        }
        fn.code.push_back(c);
      }
      fn.blocks[B].end = (uint32_t)fn.code.size();
      if (b == h) { // succ0 = the Br target; the exit arm is gone
        fn.succ[2 * B] = hbr_tgt;
        fn.succ[2 * B + 1] = NONE;
      } else {
        fn.succ[2 * B] = (fn.succ0(b) != NONE && inL[fn.succ0(b)])
                             ? bmap[fn.succ0(b)]
                             : fn.succ0(b);
        fn.succ[2 * B + 1] = (fn.succ1(b) != NONE && inL[fn.succ1(b)])
                                 ? bmap[fn.succ1(b)]
                                 : fn.succ1(b);
      }
    }
    // -- preheader: br h -> cbr(cond) H' / X ----------------------------
    {
      Instr &pt = fn.code[fn.blocks[pre].end - 1];
      pt.k = K::Cbr;
      pt.a = cond;
      pt.t = bmap[h];
      pt.f = X;
      fn.succ[2 * pre] = bmap[h];
      fn.succ[2 * pre + 1] = X;
    }
    // -- merge values for header phis read on the false path ------------
    // X's post-unswitch preds: pre + X's ORIGINAL outside preds (the
    // break path may reach X through unchanged outside blocks — the
    // if-arm `br` block — so the clone-side key is that block id, not a
    // clone id; at most one by the two-pred invariant). With a side pred
    // a fresh phi at X merges (pre_arg, the cloned value); without one
    // the pre_arg IS the value (X is single-pred then).
    uint32_t x_side_pred = NONE;
    for (uint32_t p : fn.preds[X])
      if (p != h)
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
                     .a = P.pre_arg,     // arrives from pre (false path)
                     .b2 = fresh[P.dst], // arrives from the side pred
                     .t = pre,           // (true path, after the loop ran)
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
            if (key == h && b == X) { // the header edge: pre is born.
              // MUST precede the in-loop branch below: h is in-loop too,
              // but its edge is the exit being replaced by pre, not a
              // break edge to rekey to a clone.
              key = pre;
              for (size_t pi = 0; pi < hphis.size(); ++pi)
                if (hphis[pi].dst == arg) {
                  arg = hphis[pi].pre_arg; // the entry value
                  break;
                }
              return;
            }
            if (key < nb && inL[key]) { // break edge: the clone enters
              arg = mapv(arg);
              key = bmap[key];
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
            arg = mapv(arg);
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
          v = mapv(v);
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
