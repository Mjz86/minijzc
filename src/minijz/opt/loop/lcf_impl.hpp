#pragma once
// === lcf_impl.hpp ========================================
// the LCF SUPPORT layer, split out of lcf.hpp (the code-quality
// continuation, the cdfold treatment): the pass-wide scan tables, the
// per-loop shape matchers (one small constexpr function per phase of
// the former 640-line driver), the plan gates, and the emit/rewire
// tail. The slim driver -- the per-header loop -- stays in lcf.hpp.
//
// Byte-identical contract (what a refactor must NOT perturb):
//   * every block scan is bounded by `S.nb`, the PASS-ENTRY block
//     count, exactly as the original's `nb` was (no fn.nblocks()
//     substitutions).
//   * the fallback simulator's operand-const check reads ONLY
//     I.a/I.b2/I.c2 -- NOT I.args (the body is pure, but the
//     asymmetry is the original's and is preserved, not "fixed").
//   * LcfRtIn.C is left unset (nullptr), exactly as the old inline
//     dispatcher did -- lcf_rt_fold never dereferences it.
//   * the analytic scan stops at the FIRST failing phi but still
//     records that phi's evo entry (later entries keep the
//     {false,0,0,0} default) -- evo is observable via lcf_rt's gates.
//   * the vreg allocation order of the emitters (finalv fv, then
//     per-shape c/t/c2) is part of the output: the register audit
//     table regenerates byte-identically only if it is preserved.
//   * header order = the nest-descending selection sort below, and
//     one fold per call (the driver returns true immediately).
//
// NOTE on loop_shape.hpp: its shared LoopScan/in_loop_flags/
// header_phis look similar but are NOT equivalent -- LoopScan marks
// kc/kv from the LAST def (lcf requires ndefs==1) and carries no
// uses/ndefs tables; loop_shape_of requires exactly ONE internal pred
// (lcf tolerates several back-edges, using the last as the latch) and
// derives no exit polarity. lcf keeps its own scans on purpose.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/phi_norm.hpp"
#include "../analysis/an.hpp"
#include "../linear/linform.hpp"
#include "loop_shape.hpp" // HPhi (the shared header-phi record)
#include "lcf_sim.hpp"    // LcfSimIn + lcf_sim_fold + the budgets
#include "lcf_rt.hpp"     // LcfEvo + LcfRtIn + lcf_rt_fold

// ---- the series algebra (a^n, S(n), T(n), all mod 2^64) -----------------
// lcf_pow has no callers today (kept: it documents the a^N in K1 and is
// the natural companion of lcf_geo, the emitter's engine).
constexpr uint64_t lcf_pow(uint64_t a, uint64_t n) { // a^n mod 2^64
  uint64_t r = 1;
  while (n) {
    if (n & 1)
      r *= a;
    a *= a;
    n >>= 1;
  }
  return r;
}
// S(n) = sum_{j<n} a^j, T(n) = sum_{j<n} j*a^j, out P = a^n, all mod 2^64,
// exact by doubling: S(2m)=S(m)(1+a^m); T(2m)=T(m)(1+a^m)+m*a^m*S(m);
// S(2m+1)=S(2m)+a^(2m); T(2m+1)=T(2m)+(2m)*a^(2m)
constexpr void lcf_geo(uint64_t a, uint64_t n, uint64_t &S, uint64_t &T,
                       uint64_t &P) {
  S = 0;
  T = 0;
  P = 1;
  if (!n)
    return;
  uint64_t m = 0;
  int hi = 63;
  while (!((n >> hi) & 1))
    --hi;
  for (int bit = hi; bit >= 0; --bit) {
    const uint64_t Sm = S, Tm = T, Pm = P;
    S = Sm * (Pm + 1);
    T = Tm * (Pm + 1) + Pm * Sm * m;
    P = Pm * Pm;
    m <<= 1;
    if ((n >> bit) & 1) {
      S += P;
      T += m * P;
      P *= a;
      ++m;
    }
  }
}

// ---- the pass-wide scan (built once per opt_lcf call) --------------------
struct LcfScan {
  uint32_t nb{}, nv{};          // pass-entry block count / vreg count
  std::vector<char> kc;         // vreg's ONLY def is a Const
  std::vector<val_t> kv;        // ... and its value
  std::vector<uint32_t> defi{}; // last defining instr (pass-entry scan)
  std::vector<uint32_t> defb{}; // ... and its block
  std::vector<uint32_t> uses{}, ndefs{};
  std::vector<uint32_t> headers{}; // loop headers, nest-descending
};

constexpr void lcf_bump_uses(const Instr &I, LcfScan &S) {
  auto bump = [&](uint32_t v) {
    if (v != NONE && v < S.nv)
      ++S.uses[v];
  };
  bump(I.a);
  bump(I.b2);
  bump(I.c2);
  for (uint32_t v : I.args)
    bump(v);
}

constexpr void lcf_count_defs_uses(Fn &fn, LcfScan &S) {
  S.kc.assign(S.nv, 0);
  S.kv.assign(S.nv, 0);
  S.defi.assign(S.nv, NONE);
  S.defb.assign(S.nv, NONE);
  S.uses.assign(S.nv, 0);
  S.ndefs.assign(S.nv, 0);
  for (uint32_t b = 0; b < S.nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < S.nv) {
        ++S.ndefs[I.dst];
        S.defi[I.dst] = i;
        S.defb[I.dst] = b;
      }
      lcf_bump_uses(I, S);
    }
}

constexpr void lcf_mark_consts(Fn &fn, LcfScan &S) {
  for (uint32_t b = 0; b < S.nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const && I.dst != NONE && I.dst < S.nv &&
          S.ndefs[I.dst] == 1) {
        S.kc[I.dst] = 1;
        S.kv[I.dst] = I.c;
      }
    }
}

constexpr void lcf_header_list(const An &A, LcfScan &S) {
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h)
      S.headers.push_back(h);
  for (size_t i = 0; i + 1 < S.headers.size(); ++i)
    for (size_t j = i + 1; j < S.headers.size(); ++j)
      if (A.nest[S.headers[j]] > A.nest[S.headers[i]])
        std::swap(S.headers[i], S.headers[j]);
}

constexpr bool lcf_scan(Fn &fn, const An &A, LcfScan &S) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  S.nb = fn.nblocks();
  S.nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!S.nv)
    return false;
  lcf_count_defs_uses(fn, S);
  lcf_mark_consts(fn, S);
  lcf_header_list(A, S);
  return true;
}

// ---- the per-loop shape (filled by the matchers below) -------------------
struct LcfLoop {
  uint32_t h{}, pre{}, latch{}, X{}, body{};
  Instr hterm{}, C{}; // the header cbr / the induction compare
  bool exit_on_true{}, lhs_is_ind{}, i0_const{}, bound_const{};
  uint32_t ind{NONE}, ind_idx{NONE}, cond{NONE}, Bv{NONE};
  val_t step_mag{};
  bool step_sub{};
  uint64_t N{~0ull};
  bool ct_trip{}, loop_maytrap{}, analytic{};
  std::vector<char> inL{};
  std::vector<HPhi> hphis{};
  std::vector<LcfEvo> evo{};
};

// ---- shape phase 1: in-loop flags + the two entry edges ------------------
constexpr std::vector<char> lcf_inloop(const An &A, const LcfScan &S,
                                       uint32_t h) {
  std::vector<char> inL(S.nb, 0);
  for (ulen x : A.loops.range()[h])
    inL[(uint32_t)x] = 1;
  inL[h] = 1;
  return inL;
}

constexpr bool lcf_unique_entry(Fn &fn, LcfLoop &L) {
  uint32_t pre = NONE, latch = NONE, outside = 0;
  for (uint32_t p : fn.preds[L.h]) {
    if (L.inL[p])
      latch = p; // several back-edges: the LAST is the latch (as ever)
    else {
      pre = p;
      ++outside;
    }
  }
  if (outside != 1 || pre == NONE || latch == NONE)
    return false;
  L.pre = pre;
  L.latch = latch;
  return true;
}

constexpr bool lcf_loop_edges(Fn &fn, const An &A, const LcfScan &S,
                              LcfLoop &L) {
  if (A.loops.range()[L.h].empty())
    return false;
  L.inL = lcf_inloop(A, S, L.h);
  if (!lcf_unique_entry(fn, L))
    return false;
  if (fn.blocks[L.h].begin == fn.blocks[L.h].end)
    return false;
  L.hterm = fn.code[fn.blocks[L.h].end - 1];
  return L.hterm.k == K::Cbr;
}

// ---- shape phase 2: the unique exit (leaving ONLY from the header) -------
constexpr bool lcf_block_exit_ok(Fn &fn, const LcfLoop &L, uint32_t b,
                                 uint32_t &X) {
  for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
    if (s == NONE)
      continue;
    if (!L.inL[s]) {
      if (b != L.h)
        return false; // an inner exit edge: the loop is not foldable
      if (X == NONE)
        X = s;
      else if (X != s)
        return false; // two different exits
    }
  }
  return true;
}

constexpr bool lcf_single_exit(Fn &fn, const LcfScan &S, LcfLoop &L) {
  uint32_t X = NONE;
  bool bad = false;
  for (uint32_t b = 0; b < S.nb && !bad; ++b)
    if (L.inL[b])
      bad = !lcf_block_exit_ok(fn, L, b, X);
  if (bad || X == NONE)
    return false;
  L.X = X;
  return true;
}

// ---- shape phase 3: the arms (which side is the body, exit polarity) -----
constexpr bool lcf_arms(LcfLoop &L) {
  if (L.hterm.t == L.X && L.hterm.f != NONE && L.inL[L.hterm.f]) {
    L.body = L.hterm.f;
    L.exit_on_true = true; // break-style: TRUE leaves (a folded `break`
  } // became the header test: constant while-tests
  else if (L.hterm.f == L.X && L.hterm.t != NONE && L.inL[L.hterm.t]) {
    L.body = L.hterm.t;
    L.exit_on_true = false; // continue-style: the ordinary while shape
  } else
    return false;
  return L.body != L.h; // self-loop header: rare shape, skip
}

// ---- shape phase 4: the header phis, keyed (pre, latch) ------------------
constexpr void lcf_one_hphi(Fn &fn, LcfLoop &L, const Instr &I) {
  uint32_t pa = (I.t == L.pre) ? I.a : ((I.f == L.pre) ? I.b2 : NONE);
  uint32_t la = (I.t == L.latch) ? I.a : ((I.f == L.latch) ? I.b2 : NONE);
  if (pa == NONE || la == NONE)
    return;
  L.hphis.push_back({I.dst, pa, la});
}

constexpr bool lcf_gather_hphis(Fn &fn, LcfLoop &L) {
  uint32_t nphi = 0;
  for (uint32_t i = fn.blocks[L.h].begin; i < fn.blocks[L.h].end; ++i) {
    const Instr &I = fn.code[i];
    if (I.k != K::Phi)
      break;
    ++nphi;
    lcf_one_hphi(fn, L, I);
  }
  return nphi && L.hphis.size() == nphi;
}

// ---- shape phase 5: the induction phi + the header test ------------------
constexpr bool lcf_step_match(const LcfScan &S, const Instr &D, uint32_t dst,
                              val_t &mag, bool &sub) {
  mag = 0;
  sub = false;
  if (D.k != K::Bin)
    return false;
  if (D.b == B::Add) {
    if (D.a == dst && D.b2 < S.nv && S.kc[D.b2] && S.kv[D.b2] != 0) {
      mag = S.kv[D.b2];
      return true;
    }
    if (D.b2 == dst && D.a < S.nv && S.kc[D.a] && S.kv[D.a] != 0) {
      mag = S.kv[D.a];
      return true;
    }
  } else if (D.b == B::Sub) {
    if (D.a == dst && D.b2 < S.nv && S.kc[D.b2] && S.kv[D.b2] != 0) {
      mag = S.kv[D.b2];
      sub = true;
      return true;
    }
  }
  return false;
}

constexpr bool lcf_cond_uses_ind(Fn &fn, const LcfScan &S, const LcfLoop &L,
                                 uint32_t v) {
  const Instr C = fn.code[S.defi[L.cond]];
  if (C.k != K::Bin || (C.b != B::Lt && C.b != B::Le))
    return false;
  if (C.a == v && C.b2 < S.nv)
    return true;
  return C.b2 == v && C.a < S.nv;
}

constexpr void lcf_try_ind_phi(Fn &fn, const LcfScan &S, LcfLoop &L,
                               uint32_t pi) {
  const uint32_t la = L.hphis[pi].latch_arg;
  if (la >= S.nv || S.defi[la] == NONE || !L.inL[S.defb[la]])
    return;
  const Instr D = fn.code[S.defi[la]];
  if (!lcf_step_match(S, D, L.hphis[pi].dst, L.step_mag, L.step_sub))
    return;
  if (!lcf_cond_uses_ind(fn, S, L, L.hphis[pi].dst))
    return;
  L.ind = L.hphis[pi].dst;
  L.ind_idx = pi;
}

constexpr bool lcf_find_ind(Fn &fn, const LcfScan &S, LcfLoop &L) {
  if (L.cond == NONE || L.cond >= S.nv || S.defi[L.cond] == NONE ||
      S.defb[L.cond] != L.h)
    return false; // the test value is not header-defined: skip the loop
  // entirely (the original `continue`d here -- the general simulator is
  // NOT tried: it needs the same header-defined test)
  for (size_t pi = 0; pi < L.hphis.size() && L.ind == NONE; ++pi)
    lcf_try_ind_phi(fn, S, L, (uint32_t)pi);
  return true; // ind may still be NONE: the caller tries the simulator
}

// ---- the shape match (all phases; ind may stay NONE) ---------------------
constexpr bool lcf_shape(Fn &fn, const An &A, const LcfScan &S, uint32_t h,
                         LcfLoop &L) {
  L.h = h;
  if (!lcf_loop_edges(fn, A, S, L))
    return false;
  if (!lcf_single_exit(fn, S, L))
    return false;
  if (!lcf_arms(L))
    return false;
  if (!lcf_gather_hphis(fn, L))
    return false;
  L.cond = L.hterm.a;
  return lcf_find_ind(fn, S, L);
}

// ---- no counted induction: the general any-condition simulator -----------
constexpr void lcf_fill_sim_in(const LcfScan &S, const LcfLoop &L,
                               LcfSimIn &SI) {
  SI.h = L.h;
  SI.pre = L.pre;
  SI.latch = L.latch;
  SI.X = L.X;
  SI.body = L.body;
  SI.hterm = L.hterm;
  SI.exit_on_true = L.exit_on_true; // the simulator evaluates the loop's
  // own polarity (break-style headers exit on TRUE)
  SI.hphis = &L.hphis;
  SI.inL = &L.inL;
  SI.kc = &S.kc;
  SI.kv = &S.kv;
  SI.defi = &S.defi;
  SI.defb = &S.defb;
}

constexpr bool lcf_sim_call(Fn &fn, const LcfScan &S, const LcfLoop &L,
                            OptStats &os) {
  LcfSimIn SI{};
  lcf_fill_sim_in(S, L, SI);
  return lcf_sim_fold(fn, os, SI);
}

// ---- the bound operands ---------------------------------------------------
constexpr bool lcf_bound_outside(const LcfScan &S, const LcfLoop &L) {
  if (L.Bv == NONE || L.Bv >= S.nv)
    return false;
  // a runtime bound must be defined outside the loop (otherwise the
  // trip count depends on the body and no closed form exists)
  return !(S.defi[L.Bv] != NONE && L.inL[S.defb[L.Bv]]);
}

constexpr bool lcf_bound(Fn &fn, const LcfScan &S, LcfLoop &L) {
  const HPhi IP = L.hphis[L.ind_idx];
  L.C = fn.code[S.defi[L.cond]];
  L.lhs_is_ind = (L.C.a == L.ind);
  L.Bv = L.lhs_is_ind ? L.C.b2 : L.C.a; // the bound operand
  L.i0_const = S.kc[IP.pre_arg];
  L.bound_const = (L.Bv != NONE && L.Bv < S.nv && S.kc[L.Bv]);
  return L.bound_const || lcf_bound_outside(S, L);
}

// ---- exact compile-time trip count N (when both sides are constant) ------
constexpr bool lcf_exits_now(const LcfLoop &L, val_t iv, val_t bound) {
  val_t lhs = L.lhs_is_ind ? iv : bound;
  val_t rhs = L.lhs_is_ind ? bound : iv;
  bool tv = (L.C.b == B::Lt) ? (lhs < rhs) : (lhs <= rhs);
  // continue-style runs while TRUE; break-style headers run while
  // FALSE (the test is an exit condition). N = first EXIT, either way
  return L.exit_on_true ? tv : !tv;
}

constexpr void lcf_trip_count(const LcfScan &S, LcfLoop &L) {
  L.N = ~0ull;
  if (!(L.i0_const && L.bound_const))
    return;
  val_t bound = S.kv[L.Bv];
  val_t iv = S.kv[L.hphis[L.ind_idx].pre_arg];
  for (uint64_t k = 0;; ++k) {
    if (lcf_exits_now(L, iv, bound)) {
      L.N = k;
      return;
    }
    if (k >= LCF_TRIP_BUDGET)
      return;
    iv = L.step_sub ? w_sub(iv, L.step_mag).v : w_add(iv, L.step_mag).v;
  }
}

// ---- zero trips: test provably false -> exit now -------------------------
constexpr void lcf_zero_trip(Fn &fn, const LcfLoop &L) {
  Instr &T = fn.code[fn.blocks[L.h].end - 1];
  T.k = K::Br;
  T.t = L.X;
  T.f = NONE;
  T.a = NONE;
  fn.succ[2 * L.h] = L.X;
  fn.succ[2 * L.h + 1] = NONE;
  fn.recompute_preds();
}

// ---- safety gates --------------------------------------------------------
constexpr bool lcf_block_uses_ok(Fn &fn, const LcfScan &S, const LcfLoop &L,
                                 const std::vector<char> &phid, uint32_t b) {
  for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
    const Instr &I = fn.code[i];
    // outside uses may reference ONLY header phi dsts
    auto chk = [&](uint32_t v) {
      return !(v != NONE && v < S.nv && S.defi[v] != NONE && L.inL[S.defb[v]] &&
               !phid[v]);
    };
    if (!chk(I.a) || !chk(I.b2) || !chk(I.c2))
      return false;
    for (uint32_t v : I.args)
      if (!chk(v))
        return false;
  }
  return true;
}

constexpr bool lcf_outside_uses_ok(Fn &fn, const LcfScan &S, const LcfLoop &L) {
  std::vector<char> phid(S.nv, 0);
  for (const HPhi &p : L.hphis)
    phid[p.dst] = 1;
  for (uint32_t b = 0; b < S.nb; ++b) {
    if (L.inL[b])
      continue;
    if (!lcf_block_uses_ok(fn, S, L, phid, b))
      return false;
  }
  return true;
}

constexpr bool lcf_pre_avoids_exit(const Fn &fn, const LcfLoop &L) {
  // pre must not already reach the exit (phi rekey would duplicate preds)
  for (uint32_t p : fn.preds[L.X])
    if (p == L.pre)
      return false;
  return true;
}

constexpr bool lcf_body_op_ok(const LcfLoop &L, uint32_t b, const Instr &I) {
  if (I.k == K::Phi)
    return b == L.h;
  return I.k == K::Const || I.k == K::Copy || I.k == K::Bin;
}

constexpr bool lcf_block_pure(Fn &fn, const LcfLoop &L, uint32_t b) {
  for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
    const Instr &I = fn.code[i];
    if (i + 1 == fn.blocks[b].end) { // terminator
      if (b == L.h)
        return I.k == K::Cbr;
      return I.k == K::Br;
    }
    if (!lcf_body_op_ok(L, b, I))
      return false;
  }
  return true;
}

constexpr bool lcf_body_pure(Fn &fn, const LcfScan &S, const LcfLoop &L) {
  // header: phis + Const/Copy/Bin + the Cbr; other blocks: Const/Copy/Bin
  // ending in Br — no memory, calls, prints, or inner branches
  for (uint32_t b = 0; b < S.nb; ++b) {
    if (!L.inL[b])
      continue;
    if (!lcf_block_pure(fn, L, b))
      return false;
  }
  return true;
}

constexpr bool lcf_block_maytrap(Fn &fn, const LcfScan &S, const LcfLoop &L,
                                 uint32_t b) {
  for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
    const Instr &I = fn.code[i];
    if (I.k != K::Bin || (I.b != B::Div && I.b != B::Mod))
      continue;
    if (I.nt)
      continue; // proven trap-free by the range analysis
    if (!(I.b2 < S.nv && S.kc[I.b2] && S.kv[I.b2] != 0))
      return true;
  }
  return false;
}

constexpr bool lcf_maytrap_scan(Fn &fn, const LcfScan &S, const LcfLoop &L) {
  // the analytic closed form cannot preserve a runtime trap (it deletes
  // the loop); such loops are only foldable by the trap-checking
  // SIMULATOR, or once trap-elimination has marked every Div/Mod `nt`
  for (uint32_t b = 0; b < S.nb; ++b) {
    if (!L.inL[b])
      continue;
    if (lcf_block_maytrap(fn, S, L, b))
      return true;
  }
  return false;
}

// ---- the analytic scan: every phi must evolve as x' = a*x + b*i + g -----
constexpr LcfEvo lcf_lin_to_evo(const LinForm &Lf, uint32_t dst, uint32_t ind) {
  LcfEvo e{true, 0, 0, (uint64_t)Lf.c};
  for (const auto &p : Lf.t) {
    if (p.first == dst)
      e.a = (uint64_t)p.second;
    else if (p.first == ind)
      e.b = (uint64_t)p.second;
    else
      e.ok = false;
  }
  return e;
}

constexpr void lcf_one_evo(Fn &fn, const LcfScan &S, LcfLoop &L,
                           const std::vector<char> &scope, size_t pi) {
  LinForm Lf{};
  std::vector<uint32_t> absr{};
  // analysis mode (checkpoint 20): the induction update absorbs even
  // though the feedback phi + this recurrence both read it (multi-use);
  // reading the linear form is all the scan does, so sharing is sound.
  // This is what produces b != 0 evolutions -- s' = s + i + 1 for
  // `s = s + i; i = i + 1;` -- feeding the T(n) series (analytic) and
  // the Tv construction (runtime) that previously never fired: the
  // single-use restriction made every induction-dependent recurrence
  // a non-phi leaf term and the scan rejected it.
  lin_expand(fn, S.nv, S.defi, S.defb, S.kc, S.kv, S.uses, S.ndefs, scope, NONE,
             Lf, (val_t)1, L.hphis[pi].latch_arg, absr, true);
  (void)absr; // informational: the whole loop is removed anyway
  L.evo[pi] = lcf_lin_to_evo(Lf, L.hphis[pi].dst, L.ind);
  L.analytic &= L.evo[pi].ok;
}

constexpr void lcf_analyze(Fn &fn, const LcfScan &S, LcfLoop &L) {
  L.evo.assign(L.hphis.size(), {false, 0, 0, 0});
  L.analytic = true;
  std::vector<char> scope(S.nb, 0);
  for (uint32_t b = 0; b < S.nb; ++b)
    scope[b] = L.inL[b];
  for (size_t pi = 0; pi < L.hphis.size() && L.analytic; ++pi)
    lcf_one_evo(fn, S, L, scope, pi); // stops at the first failure, as
} // the original did (evo[pi] is still recorded)

// ---- runtime trip count: the guarded closed form (lcf_rt.hpp) ------------
constexpr bool lcf_exit_has_phis(Fn &fn, const LcfLoop &L) {
  for (uint32_t i = fn.blocks[L.X].begin; i < fn.blocks[L.X].end; ++i)
    if (fn.code[i].k == K::Phi)
      return true;
  return false;
}

constexpr bool lcf_rt_ready(Fn &fn, const LcfLoop &L) {
  if (L.step_sub || L.step_mag < 1 || !L.lhs_is_ind)
    return false;
  if (fn.preds[L.X].size() != 1 || fn.preds[L.X][0] != L.h)
    return false; // exit must have exactly one pred (the header)
  if (lcf_exit_has_phis(fn, L))
    return false;
  const Instr &pt = fn.code[fn.blocks[L.pre].end - 1];
  if (pt.k != K::Br || pt.t != L.h)
    return false; // also idempotence
  for (const LcfEvo &e : L.evo)
    if (!e.ok || e.a != 1)
      return false; // additive recurrences only
  return true;
}

constexpr void lcf_fill_rt_in(LcfScan &S, const LcfLoop &L, LcfRtIn &RI) {
  RI.h = L.h;
  RI.pre = L.pre;
  RI.X = L.X;
  RI.IPi = L.ind_idx;
  RI.Bv = L.Bv;
  RI.step_mag = L.step_mag;
  RI.cmp = L.C.b;
  RI.hphis = &L.hphis;
  RI.inL = &L.inL;
  RI.kc = &S.kc;
  RI.kv = &S.kv;
  RI.evo = &L.evo;
  RI.defb = &S.defb; // the guard materializes an in-loop Const bound
  // (RI.C stays unset, exactly as the original dispatcher left it)
}

constexpr bool lcf_rt_call(Fn &fn, LcfScan &S, const LcfLoop &L, OptStats &os) {
  LcfRtIn RI{};
  lcf_fill_rt_in(S, L, RI);
  return lcf_rt_fold(fn, os, RI);
}

// ---- the emitters --------------------------------------------------------
constexpr void lcf_splice(Fn &fn, const LcfScan &S, uint32_t pre,
                          const std::vector<Instr> &batch) {
  uint32_t pe = fn.blocks[pre].end - 1; // terminator (pre-insert)
  fn.code.insert(fn.code.begin() + (long)pe, batch.begin(), batch.end());
  const uint32_t k = (uint32_t)batch.size();
  for (uint32_t x = 0; x < S.nb; ++x) {
    if (x == pre) {
      fn.blocks[x].end += k;
      continue;
    }
    if (fn.blocks[x].begin >= pe + 1)
      fn.blocks[x].begin += k;
    if (fn.blocks[x].end >= pe + 1)
      fn.blocks[x].end += k;
  }
}

constexpr void lcf_push_scaled(Fn &fn, std::vector<Instr> &batch, uint32_t fv,
                               const HPhi &p, uint64_t K1, uint64_t K0) {
  uint32_t c1 = new_anon_vreg(fn, 0);
  uint32_t t = new_anon_vreg(fn, fn.base_of(p.dst));
  batch.push_back({.k = K::Const, .dst = c1, .c = (val_t)K1});
  batch.push_back(
      {.k = K::Bin, .b = B::Mul, .dst = t, .a = p.pre_arg, .b2 = c1});
  uint32_t c2 = new_anon_vreg(fn, 0);
  batch.push_back({.k = K::Const, .dst = c2, .c = (val_t)K0});
  batch.push_back({.k = K::Bin, .b = B::Add, .dst = fv, .a = t, .b2 = c2});
}

constexpr void lcf_push_closed(Fn &fn, std::vector<Instr> &batch,
                               std::vector<uint32_t> &finalv, size_t pi,
                               const HPhi &p, uint64_t K1, uint64_t K0) {
  const uint32_t fv = new_anon_vreg(fn, fn.base_of(p.dst));
  finalv[pi] = fv;
  if (K1 == 1 && K0 == 0) {
    batch.push_back({.k = K::Copy, .dst = fv, .a = p.pre_arg});
    return;
  }
  if (K1 == 1) {
    uint32_t c = new_anon_vreg(fn, 0);
    batch.push_back({.k = K::Const, .dst = c, .c = (val_t)K0});
    batch.push_back(
        {.k = K::Bin, .b = B::Add, .dst = fv, .a = p.pre_arg, .b2 = c});
    return;
  }
  if (K0 == 0) {
    uint32_t c = new_anon_vreg(fn, 0);
    batch.push_back({.k = K::Const, .dst = c, .c = (val_t)K1});
    batch.push_back(
        {.k = K::Bin, .b = B::Mul, .dst = fv, .a = p.pre_arg, .b2 = c});
    return;
  }
  lcf_push_scaled(fn, batch, fv, p, K1, K0);
}

constexpr void lcf_emit_one_closed(Fn &fn, const LcfLoop &L,
                                   std::vector<Instr> &batch,
                                   std::vector<uint32_t> &finalv, size_t pi,
                                   uint64_t Nm1, uint64_t i0u, uint64_t du) {
  const LcfEvo &e = L.evo[pi];
  uint64_t S = 0, T = 0, P = 0;
  lcf_geo(e.a, L.N, S, T, P);
  const uint64_t K1 = P; // a^N
  const uint64_t Cc = e.b * i0u + e.g;
  const uint64_t Bc = e.b * du;
  const uint64_t K0 = (Cc + Bc * Nm1) * S - Bc * T;
  lcf_push_closed(fn, batch, finalv, pi, L.hphis[pi], K1, K0);
}

constexpr void lcf_emit_closed(Fn &fn, const LcfScan &S, const LcfLoop &L,
                               std::vector<Instr> &batch,
                               std::vector<uint32_t> &finalv) {
  // closed form: x_N = K1*x0 + K0 with K1 = a^N and the series constants
  const uint64_t Nm1 = L.N - 1;
  const uint64_t i0u = (uint64_t)S.kv[L.hphis[L.ind_idx].pre_arg];
  const uint64_t du =
      L.step_sub ? (uint64_t)0 - (uint64_t)L.step_mag : (uint64_t)L.step_mag;
  for (size_t pi = 0; pi < L.hphis.size(); ++pi)
    lcf_emit_one_closed(fn, L, batch, finalv, pi, Nm1, i0u, du);
}

// ---- fallback: exact compile-time simulation of all N iterations ---------
constexpr bool lcf_phi_inputs_const(const LcfScan &S, const LcfLoop &L) {
  for (const HPhi &p : L.hphis)
    if (!(p.pre_arg < S.nv && S.kc[p.pre_arg]))
      return false;
  return true;
}

constexpr uint64_t lcf_loop_instrs(Fn &fn, const LcfScan &S, const LcfLoop &L) {
  uint64_t n = 0;
  for (uint32_t b = 0; b < S.nb; ++b)
    if (L.inL[b])
      n += fn.blocks[b].end - fn.blocks[b].begin;
  return n;
}

constexpr bool lcf_block_operands_const(Fn &fn, const LcfScan &S,
                                        const LcfLoop &L, uint32_t b) {
  for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
    const Instr &I = fn.code[i];
    // every outside-defined operand must be a known constant
    // (a/b2/c2 only -- the original never read I.args here)
    auto ck = [&](uint32_t v) {
      return v == NONE || v >= S.nv ||
             (S.defi[v] != NONE && L.inL[S.defb[v]]) || S.kc[v];
    };
    if (!ck(I.a) || !ck(I.b2) || !ck(I.c2))
      return false;
  }
  return true;
}

constexpr bool lcf_loop_operands_const(Fn &fn, const LcfScan &S,
                                       const LcfLoop &L) {
  for (uint32_t b = 0; b < S.nb; ++b) {
    if (!L.inL[b])
      continue;
    if (!lcf_block_operands_const(fn, S, L, b))
      return false;
  }
  return true;
}

constexpr bool lcf_body_chain(Fn &fn, const LcfScan &S, const LcfLoop &L,
                              std::vector<uint32_t> &chain) {
  uint32_t cur = L.body, guard = 0;
  while (cur != L.h && guard++ <= S.nb) { // the straight-line chain, from
    chain.push_back(cur);                 // `body` to the latch
    cur = fn.succ0(cur);
  }
  return cur == L.h;
}

constexpr bool lcf_eval1(std::vector<val_t> &val, const Instr &I) {
  switch (I.k) {
  case K::Const:
    val[I.dst] = I.c;
    return true;
  case K::Copy:
    val[I.dst] = val[I.a];
    return true;
  case K::Bin: {
    Rv r = w_bin(I.b, val[I.a], val[I.b2]);
    if (r.trap)
      return false;
    val[I.dst] = r.v;
    return true;
  }
  default:
    return false;
  }
}

constexpr bool lcf_sim_header_pass(Fn &fn, const LcfLoop &L,
                                   std::vector<val_t> &val) {
  for (uint32_t i = fn.blocks[L.h].begin; i + 1 < fn.blocks[L.h].end; ++i) {
    if (fn.code[i].k == K::Phi)
      continue;
    if (!lcf_eval1(val, fn.code[i]))
      return false;
  }
  return true;
}

constexpr bool lcf_sim_chain_pass(Fn &fn, const LcfLoop &L,
                                  const std::vector<uint32_t> &chain,
                                  std::vector<val_t> &val) {
  for (uint32_t cb : chain)
    for (uint32_t i = fn.blocks[cb].begin; i < fn.blocks[cb].end; ++i)
      if (!lcf_eval1(val, fn.code[i]))
        return false;
  return true;
}

constexpr void lcf_latch_transfer(const LcfLoop &L, std::vector<val_t> &val,
                                  std::vector<val_t> &nxt) {
  for (size_t pi = 0; pi < L.hphis.size(); ++pi)
    nxt[pi] = val[L.hphis[pi].latch_arg];
  for (size_t pi = 0; pi < L.hphis.size(); ++pi)
    val[L.hphis[pi].dst] = nxt[pi];
}

constexpr bool lcf_sim_iterations(Fn &fn, const LcfLoop &L,
                                  const std::vector<uint32_t> &chain,
                                  std::vector<val_t> &val,
                                  std::vector<val_t> &nxt) {
  for (uint64_t it = 0; it < L.N; ++it) {
    if (!lcf_sim_header_pass(fn, L, val))
      return false;
    if (!lcf_sim_chain_pass(fn, L, chain, val))
      return false;
    lcf_latch_transfer(L, val, nxt);
  }
  return true;
}

constexpr void lcf_emit_sim_batch(Fn &fn, const LcfLoop &L,
                                  const std::vector<val_t> &val,
                                  std::vector<Instr> &batch,
                                  std::vector<uint32_t> &finalv) {
  for (size_t pi = 0; pi < L.hphis.size(); ++pi) {
    const uint32_t fv = new_anon_vreg(fn, fn.base_of(L.hphis[pi].dst));
    finalv[pi] = fv;
    batch.push_back({.k = K::Const, .dst = fv, .c = val[L.hphis[pi].dst]});
  }
}

constexpr bool lcf_sim_n(Fn &fn, const LcfScan &S, const LcfLoop &L,
                         OptStats &os, std::vector<Instr> &batch,
                         std::vector<uint32_t> &finalv) {
  if (!lcf_phi_inputs_const(S, L))
    return false;
  if (L.N * lcf_loop_instrs(fn, S, L) > LCF_SIM_BUDGET)
    return false;
  if (!lcf_loop_operands_const(fn, S, L))
    return false;
  std::vector<uint32_t> chain{};
  if (!lcf_body_chain(fn, S, L, chain))
    return false;
  std::vector<val_t> val(S.nv, 0);
  for (const HPhi &p : L.hphis)
    val[p.dst] = S.kv[p.pre_arg];
  std::vector<val_t> nxt(L.hphis.size());
  if (!lcf_sim_iterations(fn, L, chain, val, nxt))
    return false; // a trap aborted: the loop stays
  lcf_emit_sim_batch(fn, L, val, batch, finalv);
  lcf_splice(fn, S, L.pre, batch);
  os.lcf_sims++;
  return true;
}

// ---- the rewire tail (shared by both emitters) ---------------------------
constexpr void lcf_rewire_preheader(Fn &fn, const LcfLoop &L) {
  for (uint32_t i = fn.blocks[L.pre].begin; i < fn.blocks[L.pre].end; ++i) {
    Instr &I = fn.code[i];
    if (I.k == K::Br && I.t == L.h)
      I.t = L.X;
    else if (I.k == K::Cbr) {
      if (I.t == L.h)
        I.t = L.X;
      if (I.f == L.h)
        I.f = L.X;
    }
  }
  if (fn.succ[2 * L.pre] == L.h)
    fn.succ[2 * L.pre] = L.X;
  if (fn.succ[2 * L.pre + 1] == L.h)
    fn.succ[2 * L.pre + 1] = L.X;
}

constexpr void lcf_rewrite_block_uses(Fn &fn, const LcfScan &S,
                                      const LcfLoop &L,
                                      const std::vector<uint32_t> &finalv,
                                      uint32_t b) {
  for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
    Instr &I = fn.code[i];
    auto rw = [&](uint32_t &v) { // outside uses of phis -> final values
      if (v == NONE || v >= S.nv)
        return;
      for (size_t pi = 0; pi < L.hphis.size(); ++pi)
        if (v == L.hphis[pi].dst) {
          v = finalv[pi];
          return;
        }
    };
    rw(I.a);
    rw(I.b2);
    rw(I.c2);
    for (uint32_t &av : I.args)
      rw(av);
  }
}

constexpr void lcf_rewrite_uses(Fn &fn, const LcfScan &S, const LcfLoop &L,
                                const std::vector<uint32_t> &finalv) {
  for (uint32_t b = 0; b < S.nb; ++b) {
    if (L.inL[b])
      continue;
    lcf_rewrite_block_uses(fn, S, L, finalv, b);
  }
}

constexpr void lcf_rekey_exit_phis(Fn &fn, const LcfLoop &L) {
  for (uint32_t i = fn.blocks[L.X].begin; i < fn.blocks[L.X].end; ++i) {
    Instr &I = fn.code[i]; // exit phis: the h-edge now departs from pre
    if (I.k != K::Phi)
      break;
    if (I.t == L.h)
      I.t = L.pre;
    else if (I.f == L.h)
      I.f = L.pre;
  }
}

constexpr void lcf_rewire(Fn &fn, const LcfScan &S, const LcfLoop &L,
                          const std::vector<uint32_t> &finalv) {
  lcf_rewire_preheader(fn, L);        // preheader jumps straight to the exit
  lcf_rewrite_uses(fn, S, L, finalv); // loop dies, uses point at finals
  lcf_rekey_exit_phis(fn, L);         // the h-edge now departs from pre
  fn.recompute_preds();
}

// ---- the plan: bound/trip gates, then emit or runtime-version -------------
enum class LcfGo : uint8_t { Skip, Folded, Emit };

constexpr LcfGo lcf_plan(Fn &fn, LcfScan &S, LcfLoop &L, OptStats &os) {
  if (!lcf_bound(fn, S, L))
    return LcfGo::Skip;
  lcf_trip_count(S, L);
  L.ct_trip = (L.N != ~0ull);
  if (L.exit_on_true && !L.ct_trip)
    return LcfGo::Skip; // runtime closed forms assume continue-style
  // tests; break-style headers would need the guard inverted: rare
  // shape, not worth the surgery: leave the loop
  if (L.ct_trip && L.N == 0) {
    lcf_zero_trip(fn, L);
    return LcfGo::Folded;
  }
  if (!lcf_outside_uses_ok(fn, S, L) || !lcf_pre_avoids_exit(fn, L) ||
      !lcf_body_pure(fn, S, L))
    return LcfGo::Skip;
  L.loop_maytrap = lcf_maytrap_scan(fn, S, L);
  lcf_analyze(fn, S, L);
  if (L.ct_trip)
    return LcfGo::Emit;
  if (!lcf_rt_ready(fn, L) || !lcf_rt_call(fn, S, L, os))
    return LcfGo::Skip; // rt_fold returning false also means: next loop
  return LcfGo::Folded;
}

constexpr bool lcf_emit_and_rewire(Fn &fn, const LcfScan &S, const LcfLoop &L,
                                   OptStats &os) {
  std::vector<uint32_t> finalv(L.hphis.size(), NONE);
  std::vector<Instr> batch{};
  bool folded = false;
  if (L.analytic && !L.loop_maytrap) {
    lcf_emit_closed(fn, S, L, batch, finalv);
    lcf_splice(fn, S, L.pre, batch);
    os.lcf_loops++;
    folded = true;
  } else
    folded = lcf_sim_n(fn, S, L, os, batch, finalv);
  if (!folded)
    return false;
  lcf_rewire(fn, S, L, finalv);
  return true;
}
