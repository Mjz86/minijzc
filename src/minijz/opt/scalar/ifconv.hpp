#pragma once
// === ifconv.hpp ========================================
// if-conversion: diamond CFGs -> selects.

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/analysis/intervals.hpp"

// ============================================================================
// 9e6. IF-CONVERSION of provably-binary selects (ifconv)
// ============================================================================
// `cbr c ? X1 : X0` where both arms just materialize a CONSTANT (a Const
// def + a jump to the same continuation, or a Ret of a Const) and the
// range analysis proves c in [0,1]: the branch is a SELECT, and
//   dst = K0 + (K1 - K0) * c
// is exact (c in {0,1}). This is the last step of the is_even/is_odd
// pipeline: the countdown fold leaves `cbr selN ? ret1 : ret0`, the range
// analysis proves selN = n&1 in [0,1], and the whole mutual recursion has
// become `print(input() & 1)`.
constexpr bool opt_ifconv(Fn &fn, OptStats &os) {
  const uint32_t nb = fn.nblocks();
  if (!nb) return false;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return false;
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  // def lookup
  std::vector<uint32_t> defi(nv, NONE), defb(nv, NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      if (fn.code[i].dst != NONE && fn.code[i].dst < nv) {
        defi[fn.code[i].dst] = i;
        defb[fn.code[i].dst] = b;
      }
  // range analysis for the [0,1] proof — computed LAZILY on the first
  // candidate (the cheap structural pre-checks below reject most blocks
  // without ever paying for the analysis)
  An ARA{};
  Ranges RA{};
  bool ra_done = false;
  auto ensure_ra = [&]() {
    if (ra_done) return;
    ra_done = true;
    ARA = analyze(fn);
    RA = range_analyze(fn, ARA);
  };
  bool changed = false;
  for (uint32_t b = 0; b < nb; ++b) {
    const Instr &tm = fn.code[fn.blocks[b].end - 1];
    if (tm.k != K::Cbr || tm.a == NONE || tm.a >= nv) continue;
    if (tm.t == NONE || tm.f == NONE || tm.t == tm.f) continue;
    // cond range [0,1] AT THE CBR: walk this block's transfers from its
    // entry state (the cond may be defined inside the block, e.g. n & 1)
    Iv cond = {INT64_MIN, INT64_MAX, false};
    ensure_ra();
    if (RA.ok && b < RA.nb && tm.a < RA.nv) {
      std::vector<Iv> st(RA.entry.begin() + (size_t)b * RA.nv,
                         RA.entry.begin() + (size_t)(b + 1) * RA.nv);
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
        if (fn.code[i].k != K::Phi)
          iv_transfer(fn, i, RA.nv, st);
      cond = st[tm.a];
    }
    if (cond.bot || cond.lo < 0 || cond.hi > 1) continue;
    // arms: X_t / X_f: one Const def + br N (same N), no phis at N feeding...
    auto arm = [&](uint32_t xb, val_t &K, uint32_t &nx) -> bool {
      if (xb >= nb) return false;
      const Instr &at = fn.code[fn.blocks[xb].end - 1];
      if (at.k != K::Br || at.t == NONE) return false;
      nx = at.t;
      uint32_t nconst = 0;
      val_t kk = 0;
      for (uint32_t i = fn.blocks[xb].begin; i + 1 < fn.blocks[xb].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Const) {
          ++nconst;
          kk = I.c;
        } else
          return false;
      }
      if (nconst != 1) return false;
      K = kk;
      return true;
    };
    val_t Kt = 0, Kf = 0;
    uint32_t nt_ = NONE, nf = NONE;
    if (!arm(tm.t, Kt, nt_)) continue;
    if (!arm(tm.f, Kf, nf)) continue;
    if (nt_ != nf) continue; // must converge immediately
    // SOUNDNESS: each arm block must be reachable ONLY through this select.
    // The convergence phi's args are keyed by the ARM BLOCKS, so any OTHER
    // predecessor of an arm block also contributes to the phi (with the
    // same const, but through a path that never executes this select); the
    // arithmetic below is defined only on the select's own paths, so such
    // a block would leave the value undefined there (this exact shape
    // occurs when one short-circuit && feeds another: the inner select's
    // false arm doubles as the outer select's false target).
    {
      const auto &pt_ = fn.preds[tm.t];
      const auto &pf_ = fn.preds[tm.f];
      if (pt_.size() != 1 || pt_[0] != b) continue;
      if (pf_.size() != 1 || pf_[0] != b) continue;
    }
    const uint32_t N = nt_;
    // N's first phi (if any) must merge EXACTLY the two arm consts — its
    // dst is the select result. If N has no phi, the arms' consts are
    // dead and the branch is pure control: still convertible only if both
    // arms' consts are unused... keep it simple: require the phi.
    uint32_t pi = fn.blocks[N].begin;
    if (pi >= fn.blocks[N].end || fn.code[pi].k != K::Phi) continue;
    const Instr &P = fn.code[pi];
    // args keyed from the arm blocks
    auto argfrom = [&](uint32_t key) -> uint32_t {
      if (P.t == key) return P.a;
      if (P.f == key) return P.b2;
      return NONE;
    };
    uint32_t at_ = argfrom(tm.t), af_ = argfrom(tm.f);
    if (at_ == NONE || af_ == NONE) continue;
    // the phi args must be exactly the arms' Const vregs
    if (defi[at_] == NONE || defi[af_] == NONE) continue;
    if (fn.code[defi[at_]].k != K::Const || fn.code[defi[af_]].k != K::Const)
      continue;
    // (defb of the consts must be the arm blocks)
    if (defb[at_] != tm.t || defb[af_] != tm.f) continue;
    // ---- convert: phiDst = Kf + (Kt - Kf) * c ------------------------------
    std::vector<Instr> pre{};
    uint32_t d = P.dst;
    uint32_t kf = new_anon_vreg(fn, 0);
    pre.push_back({.k = K::Const, .dst = kf, .c = Kf});
    uint32_t diff = new_anon_vreg(fn, 0);
    pre.push_back({.k = K::Const, .dst = diff,
                   .c = (val_t)((uint64_t)Kt - (uint64_t)Kf)});
    uint32_t m = new_anon_vreg(fn, 0);
    pre.push_back(
        {.k = K::Bin, .b = B::Mul, .dst = m, .a = tm.a, .b2 = diff});
    pre.push_back({.k = K::Bin, .b = B::Add, .dst = d, .a = kf, .b2 = m});
    // insert before b's terminator; terminator becomes br N
    uint32_t be = fn.blocks[b].end - 1;
    fn.code.insert(fn.code.begin() + (long)be, pre.begin(), pre.end());
    const uint32_t add = (uint32_t)pre.size();
    for (uint32_t xb = 0; xb < nb; ++xb) {
      if (xb == b) {
        fn.blocks[xb].end += add;
        continue;
      }
      if (fn.blocks[xb].begin >= be + 1) fn.blocks[xb].begin += add;
      if (fn.blocks[xb].end > be) fn.blocks[xb].end += add;
    }
    {
      Instr &bt = fn.code[fn.blocks[b].end - 1];
      bt.k = K::Br;
      bt.a = NONE;
      bt.f = NONE;
      bt.t = N;
      fn.succ[2 * b] = N;
      fn.succ[2 * b + 1] = NONE;
    }
    // DELETE the phi at N: its dst is now defined by the arithmetic in b
    // (leaving it would give the vreg TWO defs, and once the old arms go
    // unreachable the phi's pred keys all die and unreach would "degenerate"
    // it to an undef Const 0 -- clobbering the select result)
    {
      uint32_t del = fn.blocks[N].begin;
      fn.code.erase(fn.code.begin() + (long)del);
      for (uint32_t xb = 0; xb < fn.nblocks(); ++xb) {
        if (xb == N) {
          fn.blocks[xb].end -= 1;
          continue;
        }
        if (fn.blocks[xb].begin > del) fn.blocks[xb].begin -= 1;
        if (fn.blocks[xb].end > del) fn.blocks[xb].end -= 1;
      }
    }
    os.ifconvs++;
    changed = true;
    break; // one per call: the fixpoint driver re-runs
  }
  return changed;
}
