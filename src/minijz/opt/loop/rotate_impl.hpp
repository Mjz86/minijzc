#pragma once
// === rotate_impl.hpp ========================================
// the LOOP-ROTATION SUPPORT layer, split out of rotate.hpp (the
// code-quality continuation, round eight -- the cdfold/lcf/unroll/
// fold/unswitch_bif/taildup/mem2reg treatment): the per-loop context
// (the shape tables, the def/read classification, the commit's value
// maps the former 405-line monolith held in locals), the candidate
// gates, the read classification, and the commit tail (the fresh
// vregs, the code-vector rebuild's per-block arms, the successor
// rewire). The slim driver -- the phase list -- stays in rotate.hpp.
//
// ALGORITHM (the phase list; every order below is the original's):
//   prepare inL (the loop membership flags) for the header h.
//   shape  the canonical shape (loop_shape_of: unique pre + latch,
//          header Cbr with one arm in / one arm out), latch != h,
//          and pre ends in a DEDICATED `br h' (rot_dedicated_br:
//          Br to h, succ0 == h, succ1 == NONE).
//   exits  the ONLY exit edge is the header's (exit_inventory
//          empty), preds(X) == {h} exactly, and X's first instr is
//          not a Phi (single-pred phis are pathological for the
//          exit re-keying; phis are always a block's FIRST instrs,
//          so one look suffices -- bail, never assume).
//   hphis  h's phis keyed (pre, latch), both keys present.
//   hbody  h's terminator saved (hterm -- the edits shift code
//          indices); a constant test bails (fold/sccp territory);
//          the non-phi body between the phis and the cbr collected
//          in order, <= ROT_BODY_MAX (it duplicates twice: the
//          guard AND the latch -- 2x growth cap). body_on_true
//          recorded (the ORIGINAL polarity the edge tests keep).
//   defs   the HDEF set (h-phis + h-body defs -- the only values
//          that exist on a 0-trip path), split is_hphi/is_hbdef,
//          and loopdef (any def whose site sits in the loop).
//   region F: the after-loop region, reachable from X loop-free
//          (the flood skips inL blocks).
//   reads  loop_read: h-body defs read by in-loop blocks other than
//          h (they get fresh header phis carrying (guard copy,
//          latch copy) -- exactly the (pre, latch) pattern the
//          original phis use); an h-phi whose LATCH arg is a moved
//          h-body def reads it too (the back edge must carry the
//          def's fresh header phi, never the moved def itself).
//          f_read: HDEF values read in F (they get exit phis
//          carrying (entry value, back-edge value)). Any OTHER use
//          of a loop-defined value outside the loop bails:
//          dominance made it impossible before the rotation --
//          belt and braces (the checkpoint-21 audit lesson).
//   commit os.rotations first; fresh vregs IN ORDER (the audit
//          table keys on vreg ids, so the allocation order is
//          observable): gv/lv per h-body def in hbody order, then
//          per v ascending pv (loop-read h-body defs) before xv
//          (F-read HDEFs). The code-vector rebuild: pre emits its
//          own instrs then the GUARD COPY (reads through gmap:
//          h-phi -> its pre_arg, h-body def -> the guard's copy)
//          and the guard cbr; h emits its phis (keys UNCHANGED; a
//          latch-side arg that is a MOVED def re-routes to the
//          def's fresh header phi), the fresh phis for moved defs
//          (dst pv, args (gv, lv), keys (pre, latch)), and `br
//          body' (the test is gone from the header); latch emits
//          its own instrs (reads through rmap: moved defs -> their
//          fresh header phis), the LATCH COPY (reads through lmap:
//          h-phi -> its latch_arg, a MOVED latch arg -> the def's
//          fresh header phi, h-body def -> the latch's copy) and
//          the edge-test cbr; X emits the EXIT PHIS (per F-read
//          HDEF: an h-phi merges (pre_arg, latch_arg) -- its own
//          pair relocated past the loop, a MOVED latch arg leaving
//          its latch copy in place -- else (guard copy, latch
//          copy), keys (pre, latch)) then its own instrs through
//          xmap (HDEF -> exit phi); other in-loop blocks emit
//          through rmap, other F blocks through xmap, everything
//          else verbatim. The successor rewire: pre cbr (h/X), h
//          br body, latch cbr (h/X); recompute_preds.
//
// DATA STRUCTURES (RotCtx, what the former monolith held in locals):
//   fn/A/S/h    the rewrite target, the caller's analysis, the
//               loop scan, the candidate header
//   nb/nv       the PASS-ENTRY block / vreg universe (every map's
//               `v >= nv' guard: a fresh vreg minted at commit is
//               never a pre-rotation value, so the maps' range
//               checks stay exact)
//   inL         the loop membership flags
//   sh/pre/latch/X/body   the canonical shape's blocks
//   hphis       h's phis keyed (pre, latch)
//   nphi/hterm/hbody      the phi count, the SAVED terminator, the
//               non-phi header body (instr indices, order kept)
//   body_on_true  the edge tests' polarity (hterm.t == body)
//   is_hdef/is_hphi/is_hbdef/loopdef   the def classification
//   F           the after-loop region mask
//   loop_read/f_read     the read classification
//   bad         the outside-use scan's bail flag
//   gv/lv/pv/xv per h-body def the guard/latch copies' vregs; per
//               value the fresh header phi / exit phi
//
// THEORY (why the rotated loop computes the same function): the
// whole banner lives in rotate.hpp (the driver kept the theory it
// explains); what this layer adds is the OBSERVABLE-ORDER ledger:
// the fresh-vreg mint order (gv/lv in hbody order, then pv/xv in v
// order), the emission order (blocks ascending; pre's own instrs
// before the guard copy; the exit phis before X's own instrs), and
// the `continue'-vs-fall-through discipline of the gates (a gate
// that fails leaves the fn untouched and tries the next header --
// the driver's chain evaluates them in exactly the original order).

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "../opt_generic.hpp" // the shape kernels (header preds, exits)
#include "loop_shape.hpp"
#include "unswitch_shape.hpp" // exit_inventory

// the growth cap: the h-body duplicates into the guard AND the latch
// before tier-0 folding sees it (LLVM's rotation caps similarly)
inline constexpr uint32_t ROT_BODY_MAX = 8;

// ---- the per-loop context ---------------------------------------------------
struct RotCtx {
  Fn &fn;
  const An &A;
  const LoopScan &S;
  uint32_t h{}, nb{}, nv{};
  std::vector<char> inL{};
  LoopShape sh{};
  uint32_t pre{}, latch{}, X{}, body{};
  std::vector<HPhi> hphis{};
  uint32_t nphi{};
  Instr hterm{}; // saved BEFORE any edit (the edits shift indices)
  bool body_on_true{};
  std::vector<uint32_t> hbody{}; // instr indices, order preserved
  std::vector<char> is_hdef{}, is_hphi{}, is_hbdef{}, loopdef{};
  std::vector<char> F{};
  std::vector<char> loop_read{}, f_read{};
  bool bad{};
  std::vector<uint32_t> gv{}, lv{}, pv{}, xv{}; // h-body def -> copies;
                                                // value -> header/exit phi
  constexpr RotCtx(Fn &fn_, const An &A_, const LoopScan &S_, uint32_t h_)
      : fn(fn_), A(A_), S(S_), h(h_), nb(S_.nb), nv(S_.nv) {}
};

// ---- the gates (each leaves the fn untouched on failure) --------------------
// inL: the loop membership flags for this header
constexpr void rot_prepare(Fn &fn, RotCtx &R) {
  in_loop_flags(fn, R.A, R.h, R.inL);
}
// the canonical shape + latch != h + pre's dedicated `br h'
constexpr bool rot_shape_ok(Fn &fn, RotCtx &R) {
  R.sh = loop_shape_of(fn, R.A, R.inL, R.h);
  if (!R.sh.ok || R.sh.latch == R.h)
    return false; // no self-loops
  R.pre = R.sh.pre;
  R.latch = R.sh.latch;
  R.X = R.sh.X;
  R.body = R.sh.body;
  const Instr &pt = fn.code[fn.blocks[R.pre].end - 1];
  return pt.k == K::Br && pt.t == R.h && fn.succ0(R.pre) == R.h &&
         fn.succ1(R.pre) == NONE;
}
// the ONLY exit edge is the header's; X entered only from h, phi-free
constexpr bool rot_exit_ok(Fn &fn, RotCtx &R) {
  std::vector<ExitEdge> exits{};
  if (!exit_inventory(fn, R.inL, R.nb, R.h, R.X, R.pre, R.h, exits) ||
      !exits.empty())
    return false;
  if (fn.preds[R.X].size() != 1 || fn.preds[R.X][0] != R.h)
    return false;
  // a single-pred X carrying a phi is pathological (the tier-0
  // degenerater turns one-live-key phis into copies; a (h,h)-keyed
  // leftover would break the exit re-keying) -- and phis are always
  // a block's FIRST instrs (the phi_norm invariant), so one look
  // suffices. Bail, never assume.
  return !(fn.blocks[R.X].begin < fn.blocks[R.X].end &&
           fn.code[fn.blocks[R.X].begin].k == K::Phi);
}
// h's phis keyed (pre, latch)
constexpr bool rot_header_phis_ok(Fn &fn, RotCtx &R) {
  return header_phis(fn, R.h, R.pre, R.latch, R.hphis);
}
// the saved terminator, the non-phi body, the size cap
constexpr bool rot_collect_hbody(Fn &fn, RotCtx &R) {
  R.hterm = fn.code[fn.blocks[R.h].end - 1];
  if (R.hterm.k != K::Cbr || R.hterm.a == NONE)
    return false;
  if (R.hterm.a < R.nv && R.S.defi[R.hterm.a] != NONE &&
      fn.code[R.S.defi[R.hterm.a]].k == K::Const)
    return false; // constant test: fold/sccp territory
  R.nphi = 0;
  while (fn.blocks[R.h].begin + R.nphi < fn.blocks[R.h].end &&
         fn.code[fn.blocks[R.h].begin + R.nphi].k == K::Phi)
    ++R.nphi;
  R.hbody.clear();
  for (uint32_t i = fn.blocks[R.h].begin + R.nphi;
       i + 1 < fn.blocks[R.h].end; ++i)
    R.hbody.push_back(i);
  if (R.hbody.size() > ROT_BODY_MAX)
    return false;
  R.body_on_true = (R.hterm.t == R.body);
  return true;
}
// the HDEF set (h-phis + h-body defs) + loopdef
constexpr void rot_mark_def_sets(Fn &fn, RotCtx &R) {
  R.is_hdef.assign(R.nv, 0);
  R.is_hphi.assign(R.nv, 0);
  R.is_hbdef.assign(R.nv, 0);
  R.loopdef.assign(R.nv, 0);
  for (const HPhi &P : R.hphis) {
    R.is_hdef[P.dst] = 1;
    R.is_hphi[P.dst] = 1;
  }
  for (uint32_t i : R.hbody) {
    const uint32_t d = fn.code[i].dst;
    if (d != NONE && d < R.nv) {
      R.is_hdef[d] = 1;
      R.is_hbdef[d] = 1;
    }
  }
  for (uint32_t v = 0; v < R.nv; ++v)
    if (R.S.defi[v] != NONE && R.inL[R.S.defb[v]])
      R.loopdef[v] = 1;
}
// F: the after-loop region (reachable from X loop-free)
constexpr void rot_after_region(Fn &fn, RotCtx &R) {
  R.F.assign(R.nb, 0);
  std::vector<uint32_t> work{R.X};
  R.F[R.X] = 1;
  while (!work.empty()) {
    const uint32_t b = work.back();
    work.pop_back();
    for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
      if (s == NONE || R.inL[s] || R.F[s])
        continue;
      R.F[s] = 1;
      work.push_back(s);
    }
  }
}
// loop_read / f_read + the outside-use safety scan (sets R.bad)
constexpr bool rot_classify_reads(Fn &fn, RotCtx &R) {
  R.loop_read.assign(R.nv, 0);
  R.f_read.assign(R.nv, 0);
  // an h-phi whose LATCH arg is a moved h-body def reads that moved
  // def: the back edge must carry the def's fresh header phi (the
  // value it computes per header position), never the moved def itself
  for (const HPhi &P : R.hphis)
    if (P.latch_arg != NONE && P.latch_arg < R.nv && R.is_hbdef[P.latch_arg])
      R.loop_read[P.latch_arg] = 1;
  R.bad = false;
  for (uint32_t b = 0; b < R.nb && !R.bad; ++b) {
    if (!R.inL[b] || b == R.h)
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end && !R.bad;
         ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Phi) { // args are uses keyed to the pred edges
        if (R.is_hbdef[I.a])
          R.loop_read[I.a] = 1;
        if (R.is_hbdef[I.b2])
          R.loop_read[I.b2] = 1;
        continue;
      }
      auto mark = [&](uint32_t v) {
        if (v != NONE && v < R.nv && R.is_hbdef[v])
          R.loop_read[v] = 1;
      };
      mark(I.a);
      mark(I.b2);
      mark(I.c2);
      for (uint32_t v : I.args)
        mark(v);
    }
  }
  for (uint32_t b = 0; b < R.nb && !R.bad; ++b) {
    if (R.inL[b] || b == R.pre)
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end && !R.bad;
         ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Phi) {
        // arg a is keyed to pred t; arg b2 to pred f. A loop value can
        // only arrive through a pred in F (the post-loop region): the
        // arg then reads the exit phi. Any other delivery bails.
        auto chk = [&](uint32_t arg, uint32_t key) {
          if (arg == NONE || arg >= R.nv || !R.loopdef[arg])
            return;
          if (!R.is_hdef[arg]) {
            R.bad = true;
            return;
          }
          if (R.F[b] && key < R.nb && R.F[key]) {
            R.f_read[arg] = 1;
            return;
          }
          R.bad = true; // through-only pred: dominance forbids it
        };
        chk(I.a, I.t);
        chk(I.b2, I.f);
      } else {
        auto chk = [&](uint32_t v) {
          if (v == NONE || v >= R.nv || !R.loopdef[v])
            return;
          if (!R.is_hdef[v] || !R.F[b]) {
            R.bad = true;
            return;
          }
          R.f_read[v] = 1;
        };
        chk(I.a);
        chk(I.b2);
        chk(I.c2);
        for (uint32_t v : I.args)
          chk(v);
      }
    }
  }
  return !R.bad;
}

// ---- the value maps (the commit's read rewrites) ---------------------------
// guard: h-phi -> its pre_arg (the entry value), h-body def -> the
// guard's own copy; everything else (defs dominating the preheader)
// stays
constexpr uint32_t rot_gmap(RotCtx &R, uint32_t v) {
  if (v == NONE || v >= R.nv)
    return v;
  if (R.is_hphi[v]) {
    for (const HPhi &P : R.hphis)
      if (P.dst == v)
        return P.pre_arg;
  }
  if (R.is_hbdef[v] && R.gv[v] != NONE)
    return R.gv[v];
  return v;
}
// the latch copy: h-phi -> its latch_arg (the back-edge value), and
// when that arg is itself a MOVED h-body def, -> the def's fresh header
// phi (the value the moved def carries per header position -- the back-
// edge value of the phi, which is exactly what the moved instr must
// read: the original def no longer exists in place)
constexpr uint32_t rot_lmap(RotCtx &R, uint32_t v) {
  if (v == NONE || v >= R.nv)
    return v;
  if (R.is_hphi[v]) {
    for (const HPhi &P : R.hphis)
      if (P.dst == v) {
        if (P.latch_arg < R.nv && R.is_hbdef[P.latch_arg] &&
            R.pv[P.latch_arg] != NONE)
          return R.pv[P.latch_arg];
        return P.latch_arg;
      }
  }
  if (R.is_hbdef[v] && R.lv[v] != NONE)
    return R.lv[v];
  return v;
}
// in-loop rewrite: a moved h-body def reads as its fresh header phi
constexpr uint32_t rot_rmap(RotCtx &R, uint32_t v) {
  if (v == NONE || v >= R.nv)
    return v;
  if (R.is_hbdef[v] && R.pv[v] != NONE)
    return R.pv[v];
  return v;
}
// F rewrite: an HDEF value reads as its exit phi
constexpr uint32_t rot_xmap(RotCtx &R, uint32_t v) {
  if (v == NONE || v >= R.nv)
    return v;
  if (R.is_hdef[v] && R.xv[v] != NONE)
    return R.xv[v];
  return v;
}
// one instr's reads through a value map (phi args, or the operand
// slots; Br carries no reads; Const's c is a value, not a vreg)
template <class Map> constexpr void rot_rw(Instr &c, Map &&m) {
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
}
// the edge test: h's original cbr with the cond mapped, one arm back
// into the loop (h), one arm out (X) -- the ORIGINAL polarity
constexpr Instr rot_test_cbr(RotCtx &R, uint32_t cond) {
  Instr c{};
  c.k = K::Cbr;
  c.a = cond;
  c.t = R.body_on_true ? R.h : R.X;
  c.f = R.body_on_true ? R.X : R.h;
  c.src = R.hterm.src;
  return c;
}

// ---- the commit tail --------------------------------------------------------
// fresh vregs IN ORDER (the audit table keys on vreg ids): gv/lv per
// h-body def in hbody order, then pv/xv per v ascending (pv first)
constexpr void rot_mint_vregs(Fn &fn, RotCtx &R) {
  R.gv.assign(R.nv, NONE);
  R.lv.assign(R.nv, NONE);
  R.pv.assign(R.nv, NONE);
  R.xv.assign(R.nv, NONE);
  for (uint32_t i : R.hbody) {
    const uint32_t d = fn.code[i].dst;
    if (d != NONE && d < R.nv) {
      R.gv[d] = new_anon_vreg(fn, fn.base_of(d));
      R.lv[d] = new_anon_vreg(fn, fn.base_of(d));
    }
  }
  for (uint32_t v = 0; v < R.nv; ++v) {
    if (R.is_hbdef[v] && R.loop_read[v])
      R.pv[v] = new_anon_vreg(fn, fn.base_of(v));
    if (R.is_hdef[v] && R.f_read[v])
      R.xv[v] = new_anon_vreg(fn, fn.base_of(v));
  }
}
// the rebuild's per-block arms (block ranges reallocated cleanly; the
// four rewritten blocks emit their new content, the other in-loop / F
// blocks rewrite reads, everything else copies verbatim)
constexpr void rot_emit_pre(Fn &fn, RotCtx &R, std::vector<Instr> &nc,
                            uint32_t ob, uint32_t oe) {
  for (uint32_t i = ob; i + 1 < oe; ++i)
    nc.push_back(fn.code[i]); // pre's own instrs (phis untouched)
  for (uint32_t i : R.hbody) { // the guard copy: the 1st header run
    Instr c = fn.code[i];
    rot_rw(c, [&](uint32_t v) { return rot_gmap(R, v); });
    if (c.dst != NONE && c.dst < R.nv)
      c.dst = R.gv[c.dst]; // the COPY's fresh vreg (reads mapped above)
    nc.push_back(c);
  }
  nc.push_back(rot_test_cbr(R, rot_gmap(R, R.hterm.a))); // the guard
}
constexpr void rot_emit_header(Fn &fn, RotCtx &R, std::vector<Instr> &nc,
                               uint32_t ob) {
  for (uint32_t i = ob; i < ob + R.nphi; ++i) {
    Instr c = fn.code[i]; // the phis: keys unchanged; a LATCH-side
    if (c.k == K::Phi) {  // arg that is a MOVED def re-routes to the
      // def's fresh header phi -- the value it carries per position,
      // which is what the back edge must deliver
      if (c.t == R.latch && c.a < R.nv && R.is_hbdef[c.a] &&
          R.pv[c.a] != NONE)
        c.a = R.pv[c.a];
      if (c.f == R.latch && c.b2 < R.nv && R.is_hbdef[c.b2] &&
          R.pv[c.b2] != NONE)
        c.b2 = R.pv[c.b2];
    }
    nc.push_back(c);
  }
  for (uint32_t v = 0; v < R.nv; ++v) { // fresh phis for moved defs
    if (!R.is_hbdef[v] || !R.loop_read[v])
      continue;
    Instr c{};
    c.k = K::Phi;
    c.dst = R.pv[v];
    c.a = R.gv[v];  // first entry: the guard's copy
    c.b2 = R.lv[v]; // every back edge: the latch's copy
    c.t = R.pre;
    c.f = R.latch;
    c.c = (int64_t)fn.base_of(v);
    nc.push_back(c);
  }
  Instr br{}; // the test is gone from the header: fall into the body
  br.k = K::Br;
  br.t = R.body;
  br.src = R.hterm.src;
  nc.push_back(br);
}
constexpr void rot_emit_latch(Fn &fn, RotCtx &R, std::vector<Instr> &nc,
                              uint32_t ob, uint32_t oe) {
  for (uint32_t i = ob; i + 1 < oe; ++i) { // the latch's own instrs
    Instr c = fn.code[i];
    rot_rw(c, [&](uint32_t v) { return rot_rmap(R, v); });
    nc.push_back(c);
  }
  for (uint32_t i : R.hbody) { // the edge test: the (i+1)-th header run
    Instr c = fn.code[i];
    rot_rw(c, [&](uint32_t v) { return rot_lmap(R, v); });
    if (c.dst != NONE && c.dst < R.nv)
      c.dst = R.lv[c.dst]; // the COPY's fresh vreg (reads mapped above)
    nc.push_back(c);
  }
  nc.push_back(rot_test_cbr(R, rot_lmap(R, R.hterm.a)));
}
constexpr void rot_emit_exit(Fn &fn, RotCtx &R, std::vector<Instr> &nc,
                             uint32_t ob, uint32_t oe) {
  for (uint32_t v = 0; v < R.nv; ++v) { // the exit phis
    if (!R.is_hdef[v] || !R.f_read[v])
      continue;
    Instr c{};
    c.k = K::Phi;
    c.dst = R.xv[v];
    if (R.is_hphi[v]) { // (pre_arg, latch_arg): the header phi's own
      for (const HPhi &P : R.hphis)
        if (P.dst == v) {
          c.a = P.pre_arg;
          // a MOVED latch arg leaves its latch copy in place: the
          // (k+1)-th computation, computed right before the exit
          c.b2 = (P.latch_arg < R.nv && R.is_hbdef[P.latch_arg] &&
                  R.lv[P.latch_arg] != NONE)
                     ? R.lv[P.latch_arg]
                     : P.latch_arg;
          break;
        }
    } else { // (guard copy, latch copy): the moved def's pair
      c.a = R.gv[v];
      c.b2 = R.lv[v];
    }
    c.t = R.pre;   // the zero-iteration edge: the entry value
    c.f = R.latch; // the back-edge exit: the after-iteration value
    c.c = (int64_t)fn.base_of(v);
    nc.push_back(c);
  }
  for (uint32_t i = ob; i < oe; ++i) { // X's own instrs
    Instr c = fn.code[i];
    rot_rw(c, [&](uint32_t v) { return rot_xmap(R, v); });
    nc.push_back(c);
  }
}
// the whole code-vector rebuild (deterministic block/instr order)
constexpr void rot_rebuild(Fn &fn, RotCtx &R) {
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size() + 2 * R.hbody.size() + 8);
  for (uint32_t b = 0; b < R.nb; ++b) {
    const uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
    fn.blocks[b].begin = (uint32_t)nc.size();
    if (b == R.pre)
      rot_emit_pre(fn, R, nc, ob, oe);
    else if (b == R.h)
      rot_emit_header(fn, R, nc, ob);
    else if (b == R.latch)
      rot_emit_latch(fn, R, nc, ob, oe);
    else if (b == R.X)
      rot_emit_exit(fn, R, nc, ob, oe);
    else if (R.inL[b]) { // moved-def reads -> the fresh header phis
      for (uint32_t i = ob; i < oe; ++i) {
        Instr c = fn.code[i];
        rot_rw(c, [&](uint32_t v) { return rot_rmap(R, v); });
        nc.push_back(c);
      }
    } else if (R.F[b]) { // HDEF reads -> the exit phis
      for (uint32_t i = ob; i < oe; ++i) {
        Instr c = fn.code[i];
        rot_rw(c, [&](uint32_t v) { return rot_xmap(R, v); });
        nc.push_back(c);
      }
    } else {
      for (uint32_t i = ob; i < oe; ++i)
        nc.push_back(fn.code[i]);
    }
    fn.blocks[b].end = (uint32_t)nc.size();
  }
  fn.code = std::move(nc);
}
// successors: pre cbr (h/X), h br body, latch cbr (h/X)
constexpr void rot_succ_rewire(Fn &fn, RotCtx &R) {
  fn.succ[2 * R.pre] = R.body_on_true ? R.h : R.X;
  fn.succ[2 * R.pre + 1] = R.body_on_true ? R.X : R.h;
  fn.succ[2 * R.h] = R.body;
  fn.succ[2 * R.h + 1] = NONE;
  fn.succ[2 * R.latch] = R.body_on_true ? R.h : R.X;
  fn.succ[2 * R.latch + 1] = R.body_on_true ? R.X : R.h;
  fn.recompute_preds();
}
// the commit: vregs, rebuild, rewire (one rotation -- the driver
// re-analyzes and may come back for another header)
constexpr void rot_commit(Fn &fn, RotCtx &R, OptStats &os) {
  os.rotations++;
  rot_mint_vregs(fn, R);
  rot_rebuild(fn, R);
  rot_succ_rewire(fn, R);
}
