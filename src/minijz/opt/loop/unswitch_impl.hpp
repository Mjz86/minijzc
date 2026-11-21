#pragma once
// === unswitch_impl.hpp ========================================
// the LOOP-UNSWITCHING SUPPORT layer, split out of unswitch.hpp (the
// code-quality continuation, round ten -- the cdfold/lcf/unroll/fold/
// unswitch_bif/taildup/mem2reg/rotate treatment): the per-loop
// context (the shape fields, the analysis tables, the scan marks and
// the commit's merge values the former 381-line monolith held in
// locals), the candidate gates, the outside-use safety scan, and the
// commit tail (the compound guard blocks, the guard emission, the
// merge phis at X, the rewire sweep). The slim driver -- the phase
// list -- stays in unswitch.hpp; the shape matchers, the clone
// machinery and the bif path live in unswitch_shape.hpp /
// unswitch_bif*.hpp, shared with all three unswitch shapes.
//
// ALGORITHM (the phase list; every order below is the original's):
//   shape  the trivial shape (loop_shape_of: primary = h, bypass =
//          sh.body, both preds of the clone entry = pre) or the
//          compound shape (compound_shape_of: primary = mm, the
//          zero-iteration edge is mm's exit arm), then the shared
//          gates: both guards cond_invariant (SSA-defined outside,
//          dominating the preheader), the preheader a DEDICATED
//          `br h' (Br to h, succ0 == h, succ1 == NONE), the growth
//          cap (1..120 loop instrs).
//   exits  exit_inventory (break edges allowed; Ret/Exit have no
//          succ) + header_phis keyed (pre, latch).
//   region the false-path region F (worklist from X, loop-free) and
//          loopdef (defs whose site sits in the loop).
//   scan   the outside-use safety scan (us_outside_uses_ok): plain
//          uses in F may read only header phis (they get merge
//          values); phi args keyed to an in-loop pred rekey to the
//          clone (the primary edge's args take the header phis'
//          ENTRY values); reads outside F remap to the clone.
//   commit guard blocks first (compound only, BEFORE the clone map
//          so the clone ids and the header-phi entry rekey are
//          stable), then alloc_clone + emit_loop_clone, the guard
//          emission (trivial: pre's terminator becomes the version
//          cbr; &&: pre/g2 cbr pair; ||: pre/g2/t1 chain), the
//          merge phis at X (fresh vreg per header phi read on the
//          false path, when X has a side pred), the rewire sweep,
//          recompute_preds.
//
// THE LANDMINES (preserved exactly, per the house rule):
//   * the primary-edge phi-arg branch in the rewire MUST precede the
//     in-loop branch (the primary block is in-loop too, but its edge
//     is the exit being replaced, not a break edge to rekey).
//   * guard blocks are allocated BEFORE alloc_clone (the bmap ids
//     feed the guard terminators and the entry rekey).
//   * the merge-phi insertion shifts every later block's code range
//     (the begin/end fixup loop is part of the insert).
//   * os.unswitches++ happens in the driver before the first commit
//     call, exactly where the monolith had it.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "loop_shape.hpp"
#include "unswitch_shape.hpp"

// ---- the per-loop context ---------------------------------------------------
// The block/vreg id fields default to NONE exactly like the monolith's
// per-header locals did (`uint32_t g2 = NONE, fx = NONE, t1 = NONE`):
// the && compound shape never allocates t1, and us_emit_guards'
// `if (U.t1 != NONE)` guard reads that default. Zero-init would emit
// the ||-only `t1: br H'` into block 0 (fuzz seed 20029 caught the
// first draft doing exactly that).
struct UsCtx {
  // the shape (us_shape_ok)
  std::vector<char> inL{}; // loop membership flags for h
  uint32_t pre{NONE}, latch{NONE}, X{NONE};
  uint32_t primary{NONE}, bypass{NONE};
  uint32_t entry_pred{NONE}, false_pred{NONE}; // clone entry / zero-iter pred
  uint32_t guard1{NONE}, guard2{NONE};         // the invariant conditions
  bool compound{}, is_and{};
  // the analysis (us_exits_ok / us_false_region)
  std::vector<ExitEdge> exits{};
  std::vector<HPhi> hphis{};
  std::vector<char> F{}, loopdef{};
  // the scan (us_outside_uses_ok)
  std::vector<char> read_on_false{};
  // the commit (us_guard_blocks / us_merge_phis)
  uint32_t g2{NONE}, fx{NONE}, t1{NONE};
  uint32_t x_side_pred{NONE};
  std::vector<uint32_t> phi_final{};
};

// ---- the shape pick + the shared gates --------------------------------------
// Trivial (primary = h, the zero-iteration edge is h's exit arm) or
// compound (primary = mm). False = this header declines.
constexpr bool us_shape_ok(const Fn &fn, const An &A, const LoopScan &S,
                           uint32_t h, const LoopShape &sh, UsCtx &U) {
  if (sh.ok) {
    U.pre = sh.pre;
    U.latch = sh.latch;
    U.X = sh.X;
    U.primary = h; // the zero-iteration edge is h's exit arm
    U.bypass = sh.body;
    U.entry_pred = U.pre;
    U.false_pred = U.pre;
    U.guard1 = fn.code[fn.blocks[h].end - 1].a;
  } else {
    const CompShape cs = compound_shape_of(fn, A, U.inL, S, h);
    if (!cs.ok)
      return false;
    U.pre = cs.pre;
    U.latch = cs.latch;
    U.X = cs.X;
    U.primary = cs.mm; // the zero-iteration edge is mm's exit arm
    U.bypass = cs.body;
    U.compound = true;
    U.is_and = cs.is_and;
    U.guard1 = cs.c1;
    U.guard2 = cs.c2;
    U.entry_pred = NONE; // filled once the guard blocks exist
    U.false_pred = NONE;
  }
  // ---- the invariant condition(s) -----------------------------------
  if (!cond_invariant(fn, A, U.inL, S, U.pre, U.guard1))
    return false;
  if (U.compound && !cond_invariant(fn, A, U.inL, S, U.pre, U.guard2))
    return false;
  // ---- the preheader must be a dedicated `br h' block ---------------
  {
    const Instr &pt = fn.code[fn.blocks[U.pre].end - 1];
    if (pt.k != K::Br || pt.t != h)
      return false;
    if (fn.succ0(U.pre) != h || fn.succ1(U.pre) != NONE)
      return false;
  }
  // ---- growth cap ---------------------------------------------------
  uint32_t loop_instrs = 0;
  for (uint32_t b = 0; b < S.nb; ++b)
    if (U.inL[b])
      loop_instrs += fn.blocks[b].end - fn.blocks[b].begin;
  return loop_instrs != 0 && loop_instrs <= 120;
}

// ---- exit-edge inventory + header phis --------------------------------------
// Breaks allowed (Ret/Exit have no succ); every header phi keyed
// (pre, latch). False = decline.
constexpr bool us_exits_ok(const Fn &fn, const LoopScan &S, uint32_t h,
                           UsCtx &U) {
  if (!exit_inventory(fn, U.inL, S.nb, U.primary, U.X, U.pre, h, U.exits))
    return false;
  return header_phis(fn, h, U.pre, U.latch, U.hphis);
}

// ---- the false-path region F + loopdef --------------------------------------
// F: reachable from X without entering the loop (the merge-phi region);
// loopdef: defs whose site sits in the loop (the outside-use domain).
constexpr void us_false_region(const Fn &fn, const LoopScan &S, UsCtx &U) {
  U.F.assign(S.nb, 0);
  {
    std::vector<uint32_t> work{U.X};
    U.F[U.X] = 1;
    while (!work.empty()) {
      uint32_t b = work.back();
      work.pop_back();
      for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
        if (s == NONE || U.inL[s] || U.F[s])
          continue;
        U.F[s] = 1;
        work.push_back(s);
      }
    }
  }
  U.loopdef.assign(S.nv, 0);
  for (uint32_t v = 0; v < S.nv; ++v)
    if (S.defi[v] != NONE && U.inL[S.defb[v]])
      U.loopdef[v] = 1;
}

// ---- the outside-use safety scan --------------------------------------------
// kinds: (a) plain use in F of a header phi -> merge value;
// (b) plain use outside F -> remap to the clone;
// (c) phi arg keyed to an in-loop pred -> rekey + remap (except the
//     primary edge: header phis take their pre_arg, other header
//     defs bail);
// (d) phi arg keyed to an outside pred: remap when the pred is not
//     in F (the value left through the loop), nothing when it is
//     (dominance forbids loop values there).
// False = decline (bad); U.read_on_false marks the merge candidates.
constexpr bool us_outside_uses_ok(const Fn &fn, const LoopScan &S, UsCtx &U) {
  const uint32_t nb = S.nb, nv = S.nv;
  const std::vector<char> &inL = U.inL, &F = U.F, &loopdef = U.loopdef;
  const std::vector<HPhi> &hphis = U.hphis;
  U.read_on_false.assign(hphis.size(), 0);
  std::vector<char> &read_on_false = U.read_on_false;
  bool bad = false;
  for (uint32_t b = 0; b < nb && !bad; ++b) {
    if (inL[b] || b == U.pre)
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
            if (pred == U.primary) {      // the zero-iteration edge:
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
          for_each_operand(I, mark); // the shared enumeration
        }
      }
    }
  }
  return !bad;
}

// ---- commit phase 1: the compound guard blocks ------------------------------
// && : pre: cbr(c1) g2/fx ; g2: cbr(c2) H'/fx ; fx: br X
// || : pre: cbr(c1) t1/g2 ; g2: cbr(c2) t1/fx ; t1: br H' ; fx: br X
// Allocated BEFORE the clone map so the clone ids and the header-phi
// entry rekey are stable.
constexpr void us_guard_blocks(Fn &fn, UsCtx &U) {
  if (!U.compound)
    return;
  const uint32_t nb0 = fn.nblocks();
  U.g2 = nb0;
  U.fx = nb0 + 1;
  fn.blocks.push_back({});
  fn.succ.push_back(NONE);
  fn.succ.push_back(NONE);
  fn.blocks.push_back({});
  fn.succ.push_back(NONE);
  fn.succ.push_back(NONE);
  if (!U.is_and) {
    U.t1 = nb0 + 2;
    fn.blocks.push_back({});
    fn.succ.push_back(NONE);
    fn.succ.push_back(NONE);
  }
  U.entry_pred = U.is_and ? U.g2 : U.t1;
  U.false_pred = U.fx;
}

// ---- commit phase 2: the guard emission -------------------------------------
// (the clone itself -- alloc_clone + emit_loop_clone -- is the
// driver's, between the two). Trivial: pre's terminator becomes the
// version cbr. fx: br X (the zero-iteration edge's new outside pred;
// compound shapes only -- the trivial shape jumps pre -> X directly).
constexpr void us_emit_guards(Fn &fn, const UsCtx &U, const CloneMap &C,
                              uint32_t h) {
  if (!U.compound) {
    Instr &pt = fn.code[fn.blocks[U.pre].end - 1];
    pt.k = K::Cbr;
    pt.a = U.guard1;
    pt.t = C.bmap[h];
    pt.f = U.X;
    fn.succ[2 * U.pre] = C.bmap[h];
    fn.succ[2 * U.pre + 1] = U.X;
  } else if (U.is_and) {
    Instr &pt = fn.code[fn.blocks[U.pre].end - 1];
    pt.k = K::Cbr;
    pt.a = U.guard1;
    pt.t = U.g2;
    pt.f = U.fx;
    fn.succ[2 * U.pre] = U.g2;
    fn.succ[2 * U.pre + 1] = U.fx;
    fn.blocks[U.g2].begin = (uint32_t)fn.code.size();
    fn.code.push_back({.k = K::Cbr, .a = U.guard2, .t = C.bmap[h], .f = U.fx});
    fn.blocks[U.g2].end = (uint32_t)fn.code.size();
    fn.succ[2 * U.g2] = C.bmap[h];
    fn.succ[2 * U.g2 + 1] = U.fx;
  } else { // ||: the true arms short-circuit into the loop
    Instr &pt = fn.code[fn.blocks[U.pre].end - 1];
    pt.k = K::Cbr;
    pt.a = U.guard1;
    pt.t = U.t1;
    pt.f = U.g2;
    fn.succ[2 * U.pre] = U.t1;
    fn.succ[2 * U.pre + 1] = U.g2;
    fn.blocks[U.g2].begin = (uint32_t)fn.code.size();
    fn.code.push_back({.k = K::Cbr, .a = U.guard2, .t = U.t1, .f = U.fx});
    fn.blocks[U.g2].end = (uint32_t)fn.code.size();
    fn.succ[2 * U.g2] = U.t1;
    fn.succ[2 * U.g2 + 1] = U.fx;
  }
  // fx: br X (the zero-iteration edge's new outside pred; compound
  // shapes only -- the trivial shape jumps pre -> X directly)
  if (U.compound) {
    fn.blocks[U.fx].begin = (uint32_t)fn.code.size();
    fn.code.push_back({.k = K::Br, .t = U.X});
    fn.blocks[U.fx].end = (uint32_t)fn.code.size();
    fn.succ[2 * U.fx] = U.X;
    fn.succ[2 * U.fx + 1] = NONE;
    if (U.t1 != NONE) { // ||: t1: br H'
      fn.blocks[U.t1].begin = (uint32_t)fn.code.size();
      fn.code.push_back({.k = K::Br, .t = C.bmap[h]});
      fn.blocks[U.t1].end = (uint32_t)fn.code.size();
      fn.succ[2 * U.t1] = C.bmap[h];
      fn.succ[2 * U.t1 + 1] = NONE;
    }
  }
}

// ---- commit phase 3: merge values for header phis read on the false path ----
// X's post-unswitch preds: the new false-side pred (pre / fx) + X's
// ORIGINAL outside preds (the break path may enter X through unchanged
// outside blocks -- the if-arm `br` block -- so the clone-side key is
// that block id, not a clone id; at most one by the two-pred invariant;
// an in-loop side pred is rekeyed to its clone by the sweep below).
// With a side pred a fresh phi at X merges (false-side arg, the cloned
// value); without one the false-side arg IS the value (X is
// single-pred then).
constexpr void us_merge_phis(Fn &fn, UsCtx &U, const CloneMap &C) {
  U.x_side_pred = NONE;
  for (uint32_t p : fn.preds[U.X])
    if (p != U.primary)
      U.x_side_pred = p;
  U.phi_final.assign(U.hphis.size(), NONE);
  for (size_t pi = 0; pi < U.hphis.size(); ++pi) {
    if (!U.read_on_false[pi])
      continue;
    const HPhi &P = U.hphis[pi];
    if (U.x_side_pred == NONE) {
      U.phi_final[pi] = P.pre_arg; // single-pred X: the value is direct
      continue;
    }
    const uint32_t d = new_anon_vreg(fn, fn.base_of(P.dst));
    const Instr np{.k = K::Phi,
                   .dst = d,
                   .a = P.pre_arg,       // arrives on the zero-iter path
                   .b2 = C.fresh[P.dst], // arrives from the side pred
                   .t = U.false_pred,    // (true path, after the loop ran)
                   .f = U.x_side_pred,
                   .c = (int64_t)fn.base_of(P.dst)};
    const uint32_t at = fn.blocks[U.X].begin;
    fn.code.insert(fn.code.begin() + (long)at, np);
    for (uint32_t x = 0; x < fn.nblocks(); ++x) {
      if (x == U.X)
        continue;
      if (fn.blocks[x].begin >= at)
        fn.blocks[x].begin++;
      if (fn.blocks[x].end > at)
        fn.blocks[x].end++;
    }
    fn.blocks[U.X].end++;
    U.phi_final[pi] = d;
  }
}

// ---- commit phase 4: the rewire sweep ---------------------------------------
// Outside uses: false-path readers of header phis take the merge value;
// everything loop-defined else maps to its clone. Phi args rekey by
// edge (the primary edge's args take the header phis' ENTRY values;
// break edges rekey to the clone; F-pred args read the merge value).
constexpr void us_rewire(Fn &fn, const LoopScan &S, const UsCtx &U,
                         const CloneMap &C) {
  const uint32_t nb = S.nb, nv = S.nv;
  for (uint32_t b = 0; b < nb; ++b) {
    if (U.inL[b] || b == U.pre)
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Phi) {
        auto rk = [&](uint32_t &key, uint32_t &arg) {
          if (key == NONE)
            return;
          if (key == U.primary && b == U.X) { // the zero-iteration edge:
            // the false-side pred is born (pre / fx). MUST precede
            // the in-loop branch below: the primary block is
            // in-loop too, but its edge is the exit being replaced,
            // not a break edge to rekey to a clone.
            key = U.false_pred;
            for (size_t pi = 0; pi < U.hphis.size(); ++pi)
              if (U.hphis[pi].dst == arg) {
                arg = U.hphis[pi].pre_arg; // the entry value
                break;
              }
            return;
          }
          if (key < nb && U.inL[key]) { // break edge: the clone enters
            arg = mapv(C, arg);
            key = C.bmap[key];
            return;
          }
          // outside pred: the value left through it only when it is
          // not false-reachable; remap. An F-pred arg of a header phi
          // reads the merge value (the scan marked exactly those).
          if (U.F[b] && key < nb && U.F[key]) {
            for (size_t pi = 0; pi < U.hphis.size(); ++pi)
              if (U.hphis[pi].dst == arg) {
                // NONE would mean "undefined on this edge" (the scan
                // marks every real reader; this is belt-and-braces)
                arg = U.phi_final[pi] != NONE ? U.phi_final[pi] : NONE;
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
        if (v == NONE || v >= nv || !U.loopdef[v])
          return;
        if (U.F[b]) {
          for (size_t pi = 0; pi < U.hphis.size(); ++pi)
            if (U.hphis[pi].dst == v) {
              v = U.phi_final[pi]; // != NONE: the scan marked it
              return;
            }
        }
        v = mapv(C, v);
      };
      rewrite_operands(I, rw); // the shared enumeration (instr.hpp)
    }
  }
}
