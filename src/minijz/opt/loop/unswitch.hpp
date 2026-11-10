#pragma once
// === unswitch.hpp ========================================
// loop unswitching (README wishlist: "loop unswitching/versioning"):
// a loop whose condition SSA already proves loop-invariant (defined
// outside the loop, dominating the preheader) is versioned on it —
//     pre: br h          ->   pre: cbr(cond) --> H' (true) / X (false)
//     h:   cbr(cond) body/X      H':  br body'   (the test is known true)
// The false path jumps straight to the header's exit target (the loop
// body never ran: the invariant condition fails at EVERY evaluation,
// so the original loop ran zero iterations). The true path is a full
// clone of the loop with fresh vregs whose header test is gone (the
// condition cannot change — it is an SSA value).
//
// CHECKPOINT 18 extension: compound (short-circuit) conditions. The
// while-lowering spreads `while (a && b)` / `while (a || b)` over an
// elbow chain: the header tests a, the elbow E tests b, the 1/0 blocks
// TT/FF feed the merge mm whose phi+cbr IS the loop's exit edge. When
// BOTH leaf tests are invariant, the guard chain is versioned as one:
//     && : pre: cbr(a) G2/FX ; G2: cbr(b) H'/FX ; FX: br X
//     || : pre: cbr(a) T1/G2 ; G2: cbr(b) T1/FX ; T1: br H' ; FX: br X
// (per-arm polarity: && exits on either false, || enters on either
// true). The true clone bypasses the whole diamond (h' br body'), and
// the mm->X edge becomes the zero-iteration edge (X's mm-keyed phi
// args take the header phis' ENTRY values, keyed to FX). The diamond
// blocks TT/FF/E/mm are required to be pure (Const/Copy/Bin with nt
// on Div/Mod): the false path skips them, and skipped side effects
// would be a miscompile — the invariance check already rejects anything
// that computes the conditions in-loop, this is belt and braces.
//
// Internal exit edges (`break`) are allowed: the clone keeps them, and
// every exit block's phis are rekeyed from the original in-loop pred
// to its clone (args remapped to the cloned defs). The primary (zero-
// iteration) edge — the header exit in the trivial shape, mm's exit in
// the compound shape — is special: X's phi args keyed to it become
// FX-/pre-edge args carrying each header phi's ENTRY value (what the
// zero-iteration original left in the variable), and that edge is born.
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

// ---- the compound (short-circuit) condition shape -------------------------
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
    // ---- path selection: trivial shape, or the compound diamond -----
    uint32_t pre, latch, X, primary, bypass, entry_pred, false_pred;
    uint32_t guard1 = NONE, guard2 = NONE;
    bool compound = false, is_and = false;
    {
      const LoopShape sh = loop_shape_of(fn, A, inL, h);
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
    // ---- the invariant condition(s) -------------------------------------
    auto invariant = [&](uint32_t cond) -> bool {
      if (cond == NONE || cond >= nv || S.defi[cond] == NONE)
        return false;
      if (inL[S.defb[cond]])
        return false; // defined inside: not invariant
      if (fn.code[S.defi[cond]].k == K::Const)
        return false; // constant test: fold/sccp territory
      if (S.defb[cond] != pre && !A.dominates(S.defb[cond], pre))
        return false; // not available at the preheader's end
      return true;
    };
    if (!invariant(guard1))
      continue;
    if (compound && !invariant(guard2))
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
        if (b == primary && s == X) // the zero-iteration edge itself
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
          // the clone is entered through the LAST guard block (the
          // trivial shape: pre itself, a no-op rekey)
          if (b == h) {
            if (c.t == pre)
              c.t = entry_pred;
            if (c.f == pre)
              c.f = entry_pred;
          }
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
        // the cloned header's test is GONE: Br to the bypass target
        // (the trivial shape's body arm; the compound shape's real
        // body, past the whole condition diamond)
        if (b == h && i + 1 == fn.blocks[b].end && c.k == K::Cbr) {
          hbr_tgt = bmap[bypass]; // bypass is in-loop by construction
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
    // -- the guards ------------------------------------------------------
    if (!compound) {
      Instr &pt = fn.code[fn.blocks[pre].end - 1];
      pt.k = K::Cbr;
      pt.a = guard1;
      pt.t = bmap[h];
      pt.f = X;
      fn.succ[2 * pre] = bmap[h];
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
      fn.code.push_back({.k = K::Cbr, .a = guard2, .t = bmap[h], .f = fx});
      fn.blocks[g2].end = (uint32_t)fn.code.size();
      fn.succ[2 * g2] = bmap[h];
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
        fn.code.push_back({.k = K::Br, .t = bmap[h]});
        fn.blocks[t1].end = (uint32_t)fn.code.size();
        fn.succ[2 * t1] = bmap[h];
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
                     .a = P.pre_arg,     // arrives on the zero-iter path
                     .b2 = fresh[P.dst], // arrives from the side pred
                     .t = false_pred,    // (true path, after the loop ran)
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
