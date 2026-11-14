#pragma once
// === unroll_impl.hpp ========================================
// the UNROLL SUPPORT layer, split out of unroll.hpp (the code-quality
// continuation, round three -- the cdfold/lcf treatment): the
// pass-wide scan tables, the per-loop shape matchers, the trip-count
// simulator, the safety scan and the emit/rewire tail. The slim
// driver -- the per-header loop -- stays in unroll.hpp.
//
// ALGORITHM (what the pass does, phase by phase):
//   scan    one pass over the fn: last-def tables (defi/defb), const
//           tables (kc/kv), the header list nest-descending.
//   shape   per header: the natural loop's membership flags, the
//           preheader/latch classification, the only-exit-edge scan,
//           the arm polarity, the (pre,latch)-keyed header phis, the
//           induction phi + its Lt/Le test.
//   plan    simulate the induction with the program's own wrapping
//           add/sub until the test exits: K = the trip count, ivk[k]
//           = the induction value at test k. Then the growth cap.
//   safety  values defined INSIDE the loop may not be used outside
//           (only h-defined values may escape, and only via the
//           header-exit phi slots or plain uses rewritten to the
//           tail's values).
//   commit  K+1 copies: fresh blocks + fresh vregs, the induction as
//           per-copy Consts, uses redirected by mapv, the preheader
//           edge to copy 0, the exit phis rekeyed to the tail, the
//           outside uses rewritten to the tail's values.
//
// THEORY (why the rewrite preserves the program's semantics):
//   * TRIP-COUNT EXACTNESS. K is not derived from a closed formula
//     but by RUNNING the loop's own arithmetic: the induction starts
//     at i0 and advances by w_add/w_sub -- the same wrapping (mod
//     2^64) add/sub the emitted code performs -- and the test is
//     evaluated with the same < / <= the header Cbr will evaluate.
//     The simulation therefore IS an execution; wrap-around trip
//     counts and negative strides come out exact for free. ivk[k]
//     is pushed BEFORE the exit check, so ivk[K] is the value the
//     program leaves in i after the loop -- that is what outside
//     uses and exit phis must see.
//   * SINGLE EXIT. The only-exit-edge scan proves no `break` exists:
//     the header's test is the sole edge leaving the loop. Hence
//     after K copies no control edge can re-enter the body, and the
//     (K)th copy's test is provably false -- the tail copy is
//     header-only and its terminator becomes Br X.
//   * SSA BY NAMING. Every copied instruction defines a FRESH vreg
//     (new_anon_vreg) and every use is redirected by mapv:
//       phi use  -> entry[k][phi]  (the induction: the Const i_k
//                                   materialized in that copy's
//                                   header; an accumulator: copy
//                                   k-1's def, chained across
//                                   copies)
//       loop def -> fresh[k][v]    (this copy's private def)
//       other    -> v itself       (defined outside; shared)
//     Each value keeps a single definition site, so the result is
//     still strict SSA -- no reconstruction pass is needed. This is
//     the same trick SSA construction itself uses (one name per
//     def-point, phis at the merge points); here the phis disappear
//     because control flow became linear.
//   * INNERMOST FIRST. Headers are walked nest-descending and ONE
//     loop is unrolled per call: the fixpoint driver re-analyzes,
//     so an inner loop unrolls before its parent tries -- the
//     parent then sees the inner copy's constants and its own trip
//     count often becomes computable. Nested loops therefore
//     unroll inside-out across driver iterations.
//
// Byte-identical contract (what a refactor must NOT perturb):
//   * every block/vreg scan is bounded by S.nb/S.nv, the PASS-ENTRY
//     counts -- exactly the original's nb/nv locals. No
//     fn.nblocks() substitutions (commit appends blocks, so a
//     mid-pass nblocks() would differ).
//   * the def/const tables take the LAST def and the LAST Const
//     (later writes overwrite earlier within the scan loop). There
//     is no ndefs==1 requirement here -- that strictness is lcf's;
//     loop_shape.hpp's loop_scan coincides with this scan today but
//     is kept local anyway (the lcf precedent: a future
//     unification is a behavior review, not a silent swap).
//   * several in-loop preds are tolerated: the LAST is the latch;
//     outside must be exactly one (the preheader).
//   * the induction search takes the FIRST matching phi, in hphis
//     order; the header test must be Lt/Le against a Const, with
//     either operand order; the Sub step form allows only
//     D.a == phi (Add allows both orders) -- the original's
//     asymmetry, kept.
//   * the copy-succ bookkeeping's succ0 line lacks the `!= NONE`
//     guard its succ1 line carries -- the original's asymmetry
//     again, preserved to the character (a body block ending in
//     Ret has succ0 == NONE there; the expression reads
//     bmap[k][NONE] exactly as the original did).
//   * the outside-uses scan: the strict form flags only non-h
//     defined loop values; the X-block phi relaxation then
//     re-checks with PLAIN loopdef on args keyed to preds other
//     than h (stricter there: h-defined values are also banned
//     from non-h-keyed slots). Both levels are the original's.
//   * os.unrolls++ happens BEFORE the K==0 shortcut, and one unroll
//     per call: the driver returns true immediately after commit.
//   * allocation order: blocks first (k-major, block id ascending),
//     then vregs (k-major, vreg id ascending); emission per copy is
//     header-first, then body blocks in original block order, and
//     the latch's back edge targets the NEXT copy's header. The
//     register audit table regenerates byte-identically only if
//     this order is preserved.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "loop_shape.hpp" // HPhi (the shared header-phi record)

// ---- the pass-wide scan (built once per opt_unroll call) ------------------
struct UnrScan {
  uint32_t nb{}, nv{};             // pass-entry block count / vreg count
  std::vector<char> kc;            // vreg's def is a Const (LAST def wins)
  std::vector<val_t> kv;           // ... and its value
  std::vector<uint32_t> defi{};    // last defining instr (pass-entry scan)
  std::vector<uint32_t> defb{};    // ... and its block
  std::vector<uint32_t> headers{}; // loop headers, nest-descending
};

// def/const tables: one pass, block order, instr order. A Const def
// sets BOTH tables (defi/defb and kc/kv); any later def overwrites
// defi/defb, any later Const overwrites kc/kv -- LAST wins, exactly
// as the original's single fused loop did.
constexpr void unr_defs(Fn &fn, UnrScan &S) {
  for (uint32_t b = 0; b < S.nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < S.nv) {
        S.defi[I.dst] = i;
        S.defb[I.dst] = b;
      }
      if (I.k == K::Const && I.dst != NONE && I.dst < S.nv) {
        S.kc[I.dst] = 1;
        S.kv[I.dst] = I.c;
      }
    }
}

// headers = blocks that are their own innermost loop; sorted
// nest-descending by the pair-swap sort (identical to loop_scan's,
// kept local: see the byte-identical contract above).
constexpr void unr_header_list(const An &A, UnrScan &S) {
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h)
      S.headers.push_back(h);
  for (size_t i = 0; i + 1 < S.headers.size(); ++i)
    for (size_t j = i + 1; j < S.headers.size(); ++j)
      if (A.nest[S.headers[j]] > A.nest[S.headers[i]])
        std::swap(S.headers[i], S.headers[j]);
}

constexpr bool unr_scan(Fn &fn, const An &A, uint32_t limit, UnrScan &S) {
  if (!limit)
    return false; // unrolling disabled for this config
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false; // analysis must be fresh w.r.t. the block count
  S.nb = fn.nblocks();
  S.nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!S.nv)
    return false;
  S.kc.assign(S.nv, 0);
  S.kv.assign(S.nv, 0);
  S.defi.assign(S.nv, NONE);
  S.defb.assign(S.nv, NONE);
  unr_defs(fn, S);
  unr_header_list(A, S);
  return true;
}

// ---- the per-loop shape ----------------------------------------------------
struct UnrLoop {
  uint32_t h{}, pre{}, latch{};      // header / outside pred / back-edge pred
  uint32_t X{NONE};                  // the single exit block
  uint32_t body{NONE};               // the header's in-loop arm
  bool exit_on_true{};               // TRUE arm leaves (break-style header)
  std::vector<char> inL;             // in-loop block flags (row + header)
  Instr hterm{};                     // the header terminator (a Cbr)
  std::vector<HPhi> hphis;           // ALL of h's phis, (pre,latch)-keyed
  uint32_t cond{NONE};               // the test's compared vreg (defined in h)
  uint32_t ind{NONE}, ind_idx{NONE}; // the induction phi / its hphis slot
  val_t i0{}, step_mag{}, bound{};
  bool step_sub{}, lhs_is_ind{};
  Instr test{};             // the header test instr (Lt/Le)
  uint32_t K{NONE};         // first EXITING test = the trip count
  std::vector<val_t> ivk{}; // induction value at test k (ivk[K] = exit)
  uint32_t loop_instrs{};
};

// membership flags + preheader/latch. Several in-loop preds are
// tolerated (the LAST is the latch); outside must be exactly one.
constexpr bool unr_loop_edges(Fn &fn, const An &A, UnrScan &S, uint32_t h,
                              UnrLoop &L) {
  auto row = A.loops.range()[h];
  if (row.empty())
    return false;        // not a natural-loop header row
  L.inL.assign(S.nb, 0); // pass-entry bound, like the original's nb
  for (ulen x : row)
    L.inL[(uint32_t)x] = 1;
  L.inL[h] = 1;
  uint32_t pre = NONE, latch = NONE, outside = 0;
  for (uint32_t p : fn.preds[h]) {
    if (L.inL[p])
      latch = p;
    else {
      pre = p;
      ++outside;
    }
  }
  if (outside != 1 || pre == NONE || latch == NONE)
    return false; // no unique preheader, or no back edge
  L.h = h;
  L.pre = pre;
  L.latch = latch;
  return true;
}

// the header must be non-empty (so end-1 is valid) and end in a Cbr.
constexpr bool unr_header_term(Fn &fn, UnrLoop &L) {
  if (fn.blocks[L.h].begin == fn.blocks[L.h].end)
    return false; // empty header
  L.hterm = fn.code[fn.blocks[L.h].end - 1];
  return L.hterm.k == K::Cbr;
}

// the ONLY exit edge is the header's: every in-loop block's outside
// successor must be the header's own edge to one consistent X (a
// `break` anywhere else makes the loop not unrollable).
constexpr bool unr_single_exit(Fn &fn, UnrScan &S, UnrLoop &L) {
  bool bad = false;
  uint32_t X = NONE;
  for (uint32_t b = 0; b < S.nb && !bad; ++b) {
    if (!L.inL[b])
      continue;
    for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
      if (s == NONE)
        continue;
      if (!L.inL[s]) {
        if (b != L.h) {
          bad = true; // a break: not unrollable
          break;
        }
        if (X == NONE)
          X = s; // the header's exit target
        else if (X != s) {
          bad = true; // two different exits from the header
          break;
        }
      }
    }
  }
  if (bad || X == NONE)
    return false;
  L.X = X;
  return true;
}

// arm polarity: which Cbr arm runs the body. exit_on_true = the
// TRUE arm leaves (a folded `break`); otherwise FALSE exits (the
// ordinary while shape). A self-loop header (body == h) is skipped.
constexpr bool unr_arms(UnrLoop &L) {
  uint32_t body = NONE;
  bool exit_on_true = false;
  if (L.hterm.t == L.X && L.hterm.f != NONE && L.inL[L.hterm.f]) {
    body = L.hterm.f;
    exit_on_true = true; // break-style: the TRUE arm leaves the loop
  } else if (L.hterm.f == L.X && L.hterm.t != NONE && L.inL[L.hterm.t]) {
    body = L.hterm.t;
    exit_on_true = false; // continue-style: TRUE runs the body,
  }                       // FALSE exits (the ordinary while shape)
  else
    return false; // both arms in-loop: no header exit at all
  if (body == L.h)
    return false; // self-loop header: skip this rare shape
  L.body = body;
  L.exit_on_true = exit_on_true;
  return true;
}

// ALL of h's phis must carry BOTH the pre and the latch key (the
// block has exactly those two preds); one unkeyed phi kills the
// loop. Same logic as loop_shape.hpp's header_phis, kept local.
constexpr bool unr_gather_hphis(Fn &fn, UnrLoop &L) {
  uint32_t nphi = 0;
  for (uint32_t i = fn.blocks[L.h].begin; i < fn.blocks[L.h].end; ++i) {
    const Instr &I = fn.code[i];
    if (I.k != K::Phi)
      break; // phis-first invariant: the phi run ends here
    ++nphi;
    uint32_t pa = (I.t == L.pre) ? I.a : ((I.f == L.pre) ? I.b2 : NONE);
    uint32_t la = (I.t == L.latch) ? I.a : ((I.f == L.latch) ? I.b2 : NONE);
    if (pa == NONE || la == NONE)
      continue; // this phi is not (pre,latch)-keyed
    L.hphis.push_back({I.dst, pa, la});
  }
  return nphi && L.hphis.size() == nphi;
}

// the induction step: `phi + c` / `c + phi` (Add, either operand
// order) or `phi - c` (Sub, only D.a == phi) with c a nonzero Const.
constexpr bool unr_step_match(const Instr &D, uint32_t pdst, UnrScan &S,
                              val_t &mag, bool &sub) {
  if (D.b == B::Add) {
    if (D.a == pdst && D.b2 < S.nv && S.kc[D.b2] && S.kv[D.b2] != 0) {
      mag = S.kv[D.b2];
      return true;
    }
    if (D.b2 == pdst && D.a < S.nv && S.kc[D.a] && S.kv[D.a] != 0) {
      mag = S.kv[D.a];
      return true;
    }
  } else if (D.b == B::Sub) {
    if (D.a == pdst && D.b2 < S.nv && S.kc[D.b2] && S.kv[D.b2] != 0) {
      mag = S.kv[D.b2];
      sub = true;
      return true;
    }
  }
  return false;
}

// the header test (cond's def, in h) is Lt/Le and compares THIS phi
// against a Const, either operand order.
constexpr bool unr_test_uses_ind(const Instr &C, uint32_t pdst, UnrScan &S) {
  if (C.k != K::Bin || (C.b != B::Lt && C.b != B::Le))
    return false;
  if (C.a == pdst && C.b2 < S.nv && S.kc[C.b2])
    return true;
  return C.b2 == pdst && C.a < S.nv && S.kc[C.a];
}

// one candidate phi: its latch arg must be defined in the loop by a
// Bin Add/Sub whose other operand is a nonzero Const (the step);
// the test must use this phi; the pre arg (i0) must be a Const.
constexpr bool unr_try_ind(Fn &fn, UnrScan &S, UnrLoop &L, size_t pi) {
  const HPhi &P = L.hphis[pi];
  uint32_t Dst = P.latch_arg;
  if (Dst >= S.nv || S.defi[Dst] == NONE || !L.inL[S.defb[Dst]])
    return false; // latch arg not defined inside the loop
  const Instr D = fn.code[S.defi[Dst]];
  if (D.k != K::Bin)
    return false;
  val_t mag = 0;
  bool sub = false;
  if (!unr_step_match(D, P.dst, S, mag, sub))
    return false;
  const Instr C = fn.code[S.defi[L.cond]];
  if (!unr_test_uses_ind(C, P.dst, S))
    return false;
  if (!S.kc[P.pre_arg])
    return false; // i0 must be constant
  L.ind = P.dst;
  L.ind_idx = (uint32_t)pi;
  L.step_mag = mag;
  L.step_sub = sub;
  return true;
}

// the induction search: FIRST matching phi in hphis order. On hit,
// materialize the test's orientation (lhs_is_ind, bound) and i0.
constexpr bool unr_find_ind(Fn &fn, UnrScan &S, UnrLoop &L) {
  L.cond = L.hterm.a;
  if (L.cond == NONE || L.cond >= S.nv || S.defi[L.cond] == NONE ||
      S.defb[L.cond] != L.h)
    return false; // the test value is not header-defined
  for (size_t pi = 0; pi < L.hphis.size() && L.ind == NONE; ++pi)
    unr_try_ind(fn, S, L, pi);
  if (L.ind == NONE)
    return false;
  const HPhi &IP = L.hphis[L.ind_idx];
  L.i0 = S.kv[IP.pre_arg];
  L.test = fn.code[S.defi[L.cond]];
  L.lhs_is_ind = (L.test.a == L.ind);
  L.bound = L.lhs_is_ind ? S.kv[L.test.b2] : S.kv[L.test.a];
  return true;
}

// ---- the plan: trip count + growth cap ------------------------------------
// run the loop's own arithmetic: iv advances by the wrapping step,
// the test is evaluated exactly as the Cbr would. ivk[k] is pushed
// BEFORE the exit check, so ivk[K] is the value left in i at exit.
// The cap break (k >= limit) leaves K == NONE: too long to unroll.
constexpr bool unr_trip(UnrLoop &L, uint32_t limit) {
  val_t iv = L.i0;
  std::vector<val_t> ivk{};
  uint32_t K = NONE;
  for (uint32_t k = 0;; ++k) {
    val_t lhs = L.lhs_is_ind ? iv : L.bound;
    val_t rhs = L.lhs_is_ind ? L.bound : iv;
    bool tv = (L.test.b == B::Lt) ? (lhs < rhs) : (lhs <= rhs);
    // continue-style loops run while the test is TRUE; break-style
    // headers run while it is FALSE. K = the first EXITING test.
    bool exit_now = L.exit_on_true ? tv : !tv;
    ivk.push_back(iv);
    if (exit_now) {
      K = k;
      break;
    }
    if (k >= limit)
      break; // more than `limit` iterations: skip
    iv = L.step_sub ? w_sub(iv, L.step_mag).v : w_add(iv, L.step_mag).v;
  }
  if (K == NONE || K > limit) // K > limit: unreachable, kept as belt
    return false;             // and braces exactly as the original
  L.K = K;
  L.ivk = std::move(ivk);
  return true;
}

// code growth cap: (K+1) copies of the loop body must stay small.
constexpr bool unr_growth_ok(Fn &fn, UnrScan &S, UnrLoop &L) {
  uint32_t loop_instrs = 0;
  for (uint32_t b = 0; b < S.nb; ++b)
    if (L.inL[b])
      loop_instrs += fn.blocks[b].end - fn.blocks[b].begin;
  L.loop_instrs = loop_instrs;
  return (L.K + 1) * loop_instrs <= 400;
}

// ---- the safety scan: outside uses -----------------------------------------
// ld[v] = v's LAST def is inside this loop. Such values may not be
// used outside -- EXCEPT h-defined ones (defb == h): those are what
// unroll rewrites to the tail's values, so they are allowed anywhere
// outside, and at the exit block X additionally on the h-keyed phi
// slots only.
constexpr void unr_mark_loopdefs(UnrScan &S, UnrLoop &L,
                                 std::vector<char> &ld) {
  ld.assign(S.nv, 0);
  for (uint32_t v = 0; v < S.nv; ++v)
    if (S.defi[v] != NONE && L.inL[S.defb[v]])
      ld[v] = 1;
}

// strict form: a loop value that is not h-defined may never escape.
constexpr bool unr_use_bad(uint32_t v, std::vector<char> &ld, UnrScan &S,
                           UnrLoop &L) {
  return v != NONE && v < S.nv && ld[v] && S.defb[v] != L.h;
}

// a phi outside the loop: strict on both args; if that flags bad at
// the exit block X, RELAX and re-check -- args keyed to preds other
// than h must not be loop values AT ALL (h-defined included), while
// the h-keyed arg (the edge the rewrite will retarget) may carry one.
constexpr bool unr_phi_escapes(const Instr &I, uint32_t b, UnrScan &S,
                               UnrLoop &L, std::vector<char> &ld) {
  bool bad = unr_use_bad(I.a, ld, S, L) || unr_use_bad(I.b2, ld, S, L);
  if (bad && b == L.X) {
    bad = false;
    if (I.t != NONE && I.t != L.h && I.a != NONE && I.a < S.nv && ld[I.a])
      bad = true;
    if (I.f != NONE && I.f != L.h && I.b2 != NONE && I.b2 < S.nv && ld[I.b2])
      bad = true;
  }
  return bad;
}

constexpr bool unr_plain_escapes(const Instr &I, UnrScan &S, UnrLoop &L,
                                 std::vector<char> &ld) {
  if (unr_use_bad(I.a, ld, S, L) || unr_use_bad(I.b2, ld, S, L))
    return true;
  if (unr_use_bad(I.c2, ld, S, L))
    return true;
  for (uint32_t v : I.args)
    if (unr_use_bad(v, ld, S, L))
      return true;
  return false;
}

constexpr bool unr_outside_uses_ok(Fn &fn, UnrScan &S, UnrLoop &L) {
  std::vector<char> ld;
  unr_mark_loopdefs(S, L, ld);
  for (uint32_t b = 0; b < S.nb; ++b) {
    if (L.inL[b])
      continue; // only uses OUTSIDE the loop matter here
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Phi) {
        if (unr_phi_escapes(I, b, S, L, ld))
          return false;
      } else if (unr_plain_escapes(I, S, L, ld))
        return false;
    }
  }
  return true;
}

// ---- the commit: maps, emission, rewiring ----------------------------------
// bmap[k][b]: the new block of copy k for in-loop block b (the tail
// copy K holds the header only). fresh[k][v]: the new vreg of copy k
// for loop-defined vreg v (the tail: h-defined only). phi_idx: vreg
// -> its hphis slot. entry[k][pi]: the value header phi pi holds at
// copy k -- the induction is its Const i_k; an accumulator is copy
// k-1's def of its latch arg, chained. mapv redirects one use.
struct UnrMaps {
  uint32_t nv{};
  std::vector<std::vector<uint32_t>> bmap, fresh, entry;
  std::vector<uint32_t> phi_idx;
};

constexpr uint32_t unr_mapv(uint32_t v, uint32_t k, UnrMaps &M) {
  if (v == NONE)
    return NONE;
  if (v < M.nv && M.phi_idx[v] != NONE)
    return M.entry[k][M.phi_idx[v]]; // a header phi: its entry value
  if (v < M.nv && M.fresh[k][v] != NONE)
    return M.fresh[k][v]; // a loop def: this copy's private vreg
  return v;               // defined outside the loop: shared as-is
}

// K == 0: the test is provably false on entry. The header terminator
// becomes Br X in place; the unreachable body dies in the next
// unreach/phi-degen round and the phis degenerate to their pre args.
constexpr bool unr_zero_trip(Fn &fn, UnrLoop &L) {
  Instr &T = fn.code[fn.blocks[L.h].end - 1];
  T.k = K::Br;
  T.t = L.X;
  T.f = NONE;
  T.a = NONE;
  fn.succ[2 * L.h] = L.X;
  fn.succ[2 * L.h + 1] = NONE;
  fn.recompute_preds();
  return true;
}

// fresh blocks first (k-major, block id ascending), then fresh vregs
// (k-major, vreg id ascending) -- the allocation order is part of
// the output (vreg numbering feeds the register audit table).
constexpr void unr_build_maps(Fn &fn, UnrScan &S, UnrLoop &L, UnrMaps &M) {
  M.nv = S.nv;
  M.bmap.assign(L.K + 1, std::vector<uint32_t>(S.nb, NONE));
  M.fresh.assign(L.K + 1, std::vector<uint32_t>(S.nv, NONE));
  for (uint32_t k = 0; k <= L.K; ++k)
    for (uint32_t b = 0; b < S.nb; ++b) {
      if (!L.inL[b])
        continue;
      if (k == L.K && b != L.h)
        continue; // tail: header only
      uint32_t id = (uint32_t)fn.blocks.size();
      fn.blocks.push_back({});
      fn.succ.push_back(NONE);
      fn.succ.push_back(NONE);
      M.bmap[k][b] = id;
    }
  for (uint32_t k = 0; k <= L.K; ++k)
    for (uint32_t v = 0; v < S.nv; ++v) {
      if (S.defi[v] == NONE || !L.inL[S.defb[v]])
        continue;
      if (k == L.K && S.defb[v] != L.h)
        continue; // tail: h-defined values only
      M.fresh[k][v] = new_anon_vreg(fn, fn.base_of(v));
    }
}

constexpr void unr_fill_entry(UnrLoop &L, UnrMaps &M) {
  M.phi_idx.assign(M.nv, NONE);
  for (size_t pi = 0; pi < L.hphis.size(); ++pi)
    M.phi_idx[L.hphis[pi].dst] = (uint32_t)pi;
  M.entry.assign(L.K + 1, std::vector<uint32_t>(L.hphis.size(), NONE));
  for (uint32_t k = 0; k <= L.K; ++k) {
    // induction const first (it IS the entry value of the induction)
    M.entry[k][L.ind_idx] = M.fresh[k][L.ind];
    for (size_t pi = 0; pi < L.hphis.size(); ++pi) {
      if ((uint32_t)pi == L.ind_idx)
        continue;
      M.entry[k][pi] = (k == 0) ? L.hphis[pi].pre_arg
                                : unr_mapv(L.hphis[pi].latch_arg, k - 1, M);
    }
  }
}

// one terminator edge of a body copy: the edge to h is the back edge
// (it becomes the NEXT copy's header); edges to this copy's blocks
// follow it; anything else stays as-is (defensive: after the exit
// scan no body edge leaves the loop).
constexpr uint32_t unr_edge(uint32_t t, uint32_t k, UnrLoop &L, UnrMaps &M) {
  if (t == L.h)
    return M.bmap[k + 1][L.h];
  return (t != NONE && M.bmap[k][t] != NONE) ? M.bmap[k][t] : t;
}

constexpr void unr_emit_term(Fn &fn, UnrLoop &L, UnrMaps &M, const Instr &I,
                             uint32_t k) {
  Instr c = I;
  if (I.k == K::Ret || I.k == K::Exit) {
    c.a = unr_mapv(c.a, k, M);
    fn.code.push_back(c);
  } else if (I.k == K::Br || I.k == K::Cbr) {
    c.a = unr_mapv(c.a, k, M);
    c.t = unr_edge(I.t, k, L, M);
    if (I.k == K::Cbr)
      c.f = unr_edge(I.f, k, L, M);
    fn.code.push_back(c);
  } else {
    fn.code.push_back(I); // unreachable-block junk: copy as-is
  }
}

constexpr void unr_remap_phi(Instr &c, UnrScan &S, UnrMaps &M, uint32_t k) {
  if (c.dst != NONE && c.dst < S.nv)
    c.dst = M.fresh[k][c.dst];
  c.a = unr_mapv(c.a, k, M);
  c.b2 = unr_mapv(c.b2, k, M);
  if (c.t != NONE && M.bmap[k][c.t] != NONE)
    c.t = M.bmap[k][c.t]; // pred keys follow this copy
  if (c.f != NONE && M.bmap[k][c.f] != NONE)
    c.f = M.bmap[k][c.f];
}

constexpr void unr_remap_plain(Instr &c, UnrScan &S, UnrMaps &M, uint32_t k) {
  if (c.dst != NONE && c.dst < S.nv)
    c.dst = M.fresh[k][c.dst];
  c.a = unr_mapv(c.a, k, M);
  c.b2 = unr_mapv(c.b2, k, M);
  if (c.c2 != NONE)
    c.c2 = unr_mapv(c.c2, k, M);
  for (uint32_t &av : c.args)
    av = unr_mapv(av, k, M);
}

// one body block of copy k: every instr remapped into the copy's
// private names; the terminator's edges follow unr_edge.
constexpr void unr_emit_body_block(Fn &fn, UnrScan &S, UnrLoop &L, UnrMaps &M,
                                   uint32_t b, uint32_t k) {
  for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
    const Instr &I = fn.code[i];
    if (i + 1 == fn.blocks[b].end) { // the terminator
      unr_emit_term(fn, L, M, I, k);
      continue;
    }
    Instr c = I;
    if (I.k == K::Phi)
      unr_remap_phi(c, S, M, k);
    else
      unr_remap_plain(c, S, M, k);
    fn.code.push_back(c);
  }
}

// succ bookkeeping for one body copy block. NOTE: the succ0 line
// reads bmap[k][succ0(b)] WITHOUT succ0's != NONE guard -- the
// succ1 line below carries it. The original's asymmetry, kept.
constexpr void unr_copy_succs(Fn &fn, UnrLoop &L, UnrMaps &M, uint32_t b,
                              uint32_t k, uint32_t Bk) {
  fn.succ[2 * Bk] = (M.bmap[k][fn.succ0(b)] != NONE && L.inL[fn.succ0(b)])
                        ? M.bmap[k][fn.succ0(b)]
                        : fn.succ0(b);
  fn.succ[2 * Bk + 1] = (fn.succ1(b) != NONE && L.inL[fn.succ1(b)] &&
                         M.bmap[k][fn.succ1(b)] != NONE)
                            ? M.bmap[k][fn.succ1(b)]
                            : fn.succ1(b);
  if (b == L.latch)
    fn.succ[2 * Bk] = M.bmap[k + 1][L.h]; // the back edge: next copy
}

constexpr void unr_emit_body_copies(Fn &fn, UnrScan &S, UnrLoop &L, UnrMaps &M,
                                    uint32_t k) {
  for (uint32_t b = 0; b < S.nb; ++b) { // original block order
    if (!L.inL[b] || b == L.h)
      continue;
    uint32_t Bk = M.bmap[k][b];
    fn.blocks[Bk].begin = (uint32_t)fn.code.size();
    unr_emit_body_block(fn, S, L, M, b, k);
    fn.blocks[Bk].end = (uint32_t)fn.code.size();
    unr_copy_succs(fn, L, M, b, k, Bk);
  }
}

// header copy k: [Const i_k] + h's non-phi instrs + Br(body/X).
// The induction phi BECOMES the Const; the other phis became the
// entry values (their uses were redirected by mapv at emission).
constexpr void unr_emit_header_copy(Fn &fn, UnrScan &S, UnrLoop &L, UnrMaps &M,
                                    uint32_t k) {
  uint32_t Hk = M.bmap[k][L.h];
  bool tail = (k == L.K);
  fn.blocks[Hk].begin = (uint32_t)fn.code.size();
  fn.code.push_back({.k = K::Const, .dst = M.fresh[k][L.ind], .c = L.ivk[k]});
  for (uint32_t i = fn.blocks[L.h].begin; i + 1 < fn.blocks[L.h].end; ++i) {
    const Instr &I = fn.code[i];
    if (I.k == K::Phi)
      continue; // phis are gone in the copies (see the entry table)
    Instr c = I;
    unr_remap_plain(c, S, M, k);
    fn.code.push_back(c);
  }
  uint32_t tgt = tail ? L.X : M.bmap[k][L.body];
  fn.code.push_back({.k = K::Br, .t = tgt});
  fn.blocks[Hk].end = (uint32_t)fn.code.size();
  fn.succ[2 * Hk] = tgt;
  fn.succ[2 * Hk + 1] = NONE;
}

constexpr void unr_emit_copies(Fn &fn, UnrScan &S, UnrLoop &L, UnrMaps &M) {
  for (uint32_t k = 0; k <= L.K; ++k) { // append-only: ranges stay valid
    unr_emit_header_copy(fn, S, L, M, k);
    if (k == L.K)
      continue; // the tail is header-only
    unr_emit_body_copies(fn, S, L, M, k);
  }
}

constexpr void unr_rewire_preheader(Fn &fn, UnrLoop &L, UnrMaps &M) {
  for (uint32_t i = fn.blocks[L.pre].begin; i < fn.blocks[L.pre].end; ++i) {
    Instr &I = fn.code[i];
    if (I.k == K::Br && I.t == L.h)
      I.t = M.bmap[0][L.h];
    else if (I.k == K::Cbr) {
      if (I.t == L.h)
        I.t = M.bmap[0][L.h];
      if (I.f == L.h)
        I.f = M.bmap[0][L.h];
    }
  }
  if (fn.succ[2 * L.pre] == L.h)
    fn.succ[2 * L.pre] = M.bmap[0][L.h];
  if (fn.succ[2 * L.pre + 1] == L.h)
    fn.succ[2 * L.pre + 1] = M.bmap[0][L.h];
}

// the exit block's phis: the h-edge now arrives from the tail, and
// its arg becomes the tail's value of what the phi carried.
constexpr void unr_rekey_exit_phis(Fn &fn, UnrLoop &L, UnrMaps &M) {
  for (uint32_t i = fn.blocks[L.X].begin; i < fn.blocks[L.X].end; ++i) {
    Instr &I = fn.code[i];
    if (I.k != K::Phi)
      break;
    if (I.t == L.h) {
      I.t = M.bmap[L.K][L.h];
      I.a = unr_mapv(I.a, L.K, M);
    } else if (I.f == L.h) {
      I.f = M.bmap[L.K][L.h];
      I.b2 = unr_mapv(I.b2, L.K, M);
    }
  }
}

// uses outside the loop: h-defined values (the only ones the safety
// scan let through) become the TAIL's values (mapv at copy K).
constexpr void unr_rewrite_uses(Fn &fn, UnrScan &S, UnrLoop &L, UnrMaps &M) {
  for (uint32_t b = 0; b < S.nb; ++b) {
    if (L.inL[b])
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.a != NONE && I.a < S.nv && S.defb[I.a] == L.h)
        I.a = unr_mapv(I.a, L.K, M);
      if (I.b2 != NONE && I.b2 < S.nv && S.defb[I.b2] == L.h)
        I.b2 = unr_mapv(I.b2, L.K, M);
      if (I.c2 != NONE && I.c2 < S.nv && S.defb[I.c2] == L.h)
        I.c2 = unr_mapv(I.c2, L.K, M);
      for (uint32_t &av : I.args)
        if (av < S.nv && S.defb[av] == L.h)
          av = unr_mapv(av, L.K, M);
    }
  }
}

constexpr bool unr_commit(Fn &fn, UnrScan &S, UnrLoop &L, OptStats &os) {
  os.unrolls++; // counted BEFORE the K==0 shortcut, as the original
  if (L.K == 0)
    return unr_zero_trip(fn, L);
  UnrMaps M;
  unr_build_maps(fn, S, L, M);
  unr_fill_entry(L, M);
  unr_emit_copies(fn, S, L, M);
  unr_rewire_preheader(fn, L, M);
  unr_rekey_exit_phis(fn, L, M);
  unr_rewrite_uses(fn, S, L, M);
  fn.recompute_preds();
  return true; // one unroll per call: the driver re-analyzes
}

// ---- the shape bundle (the driver's per-header gateway) ---------------------
constexpr bool unr_shape(Fn &fn, const An &A, UnrScan &S, uint32_t h,
                         UnrLoop &L) {
  if (!unr_loop_edges(fn, A, S, h, L))
    return false; // no (preheader, latch) pair around this header
  if (!unr_header_term(fn, L))
    return false; // empty header or not a conditional header
  if (!unr_single_exit(fn, S, L))
    return false; // a break, or no header exit at all
  if (!unr_arms(L))
    return false; // no in-loop arm / self-loop
  if (!unr_gather_hphis(fn, L))
    return false;                // a phi is not (pre,latch)-keyed
  return unr_find_ind(fn, S, L); // no counted induction: keep loop
}
