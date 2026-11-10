#pragma once
// === lcf_rt.hpp ========================================
// RUNTIME trip count closed forms: `while (i < B) { pure body; i += d; }`
// with constant d and a RUNTIME B (or a constant bound too big to step
// at compile time) gets a guard block that computes the trip count at
// runtime + a closed-form block; the guard fails exactly when the
// induction would leave the signed range before reaching B (the honest
// answer is then the original loop, kept as the fallback), so the
// closed form only ever runs when exact. The vacuous-guard proof
// (constant i0 >= 0, unit step, pure+nt body) drops guard AND fallback.
// Extracted from lcf.hpp (checkpoint 18's code-quality split); the
// driver-visible counter is OptStats::lcf_rt.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "loop_shape.hpp"

// one affine recurrence x' = a*x + b*i + g (the analytic scan's result;
// shared by the runtime and constant-trip paths in lcf.hpp)
struct LcfEvo {
  bool ok;
  uint64_t a, b, g;
};

// ---- lcf_rt_fold: the guarded/unguarded runtime closed form ---------------
// Call contract (lcf.hpp enforces these before calling):
//   * continue-style header test (NOT exit_on_true)
//   * counted induction `i' with constant step d >= 1, lhs_is_ind
//   * X's only pred is the header (no phis at X)
//   * the preheader is a dedicated br h
//   * every phi evolves additively (evo[].a == 1, evo[].ok)
//   * IP = hphis[IPi] is the induction phi; C is the header compare
// Returns true when the loop was versioned (caller returns; the
// fixpoint driver re-analyzes).
struct LcfRtIn {
  uint32_t h{}, pre{}, X{}, IPi{};
  uint32_t Bv{};     // the bound operand vreg
  val_t step_mag{};  // constant step magnitude (>= 1, not step_sub)
  bool lhs_is_ind{}; // compare operand order
  B cmp{};           // Lt or Le
  const Instr *C{};  // the header compare instr (operands C.a/C.b2)
  const std::vector<HPhi> *hphis{};
  const std::vector<char> *inL{};
  const std::vector<char> *kc{};
  const std::vector<val_t> *kv{};
  const std::vector<LcfEvo> *evo{};
};
constexpr bool lcf_rt_fold(Fn &fn, OptStats &os, const LcfRtIn &In) {
  const std::vector<HPhi> &hphis = *In.hphis;
  const std::vector<char> &inL = *In.inL, &kc = *In.kc;
  const std::vector<val_t> &kv = *In.kv;
  const std::vector<LcfEvo> &evo = *In.evo;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = (uint32_t)kc.size();
  const uint32_t h = In.h, pre = In.pre, X = In.X;
  const HPhi &IP = hphis[In.IPi];
  const bool is_le = (In.cmp == B::Le);
  const uint32_t i0v = IP.pre_arg;
  // ---- vacuous-guard proof --------------------------------------------
  // With a CONSTANT i0 >= 0 and unit step, the induction can never wrap
  // before the exit: whenever the condition c0 holds, D = B - i0 lands
  // in (0, MAX] (no wrap, since B <= MAX and i0 >= 0), so `dneg' is
  // false; and iN = i0 + N reproduces B bit-exactly (N = Dm, and
  // i0 + (B - i0) = B in the mod-2^64 ring), so `bneg' is false too.
  // The guard is therefore identically false for EVERY runtime B. If
  // additionally the loop body is pure arithmetic that provably cannot
  // trap (Div/Mod carry `nt') and observes nothing, the closed form is
  // exact unconditionally: emit it WITHOUT the guard and WITHOUT the
  // fallback loop (the now-unreachable loop is removed by unreach, and
  // its merge-phi args degenerate away).
  bool body_pure = true;
  for (uint32_t b2 = 0; b2 < nb && body_pure; ++b2) {
    if (!inL[b2])
      continue;
    for (uint32_t i2 = fn.blocks[b2].begin; i2 < fn.blocks[b2].end; ++i2) {
      const Instr &BI = fn.code[i2];
      if (BI.k == K::Phi || BI.k == K::Const || BI.k == K::Copy ||
          BI.k == K::Br || BI.k == K::Cbr)
        continue;
      if (BI.k == K::Bin) {
        if ((BI.b == B::Div || BI.b == B::Mod) && !BI.nt) {
          body_pure = false; // a MAY-trap div/mod must run for real
          break;
        }
        continue;
      }
      body_pure = false; // Print/Call/Input/Alloc/Load/Store/Len/Ret
      break;
    }
  }
  const bool vacuous = body_pure && IP.pre_arg != NONE && IP.pre_arg < nv &&
                       kc[IP.pre_arg] && kv[IP.pre_arg] >= 0 &&
                       In.step_mag == 1;
  const uint32_t G = fn.nblocks();
  const uint32_t CF = G + 1;
  fn.blocks.push_back({});
  fn.blocks.push_back({});
  fn.succ.push_back(NONE);
  fn.succ.push_back(NONE);
  fn.succ.push_back(NONE);
  fn.succ.push_back(NONE);
  std::vector<Instr> gi{}, cfi{};
  auto konst = [&](std::vector<Instr> &out, val_t c) {
    uint32_t v = new_anon_vreg(fn, 0);
    out.push_back({.k = K::Const, .dst = v, .c = c});
    return v;
  };
  auto bin = [&](std::vector<Instr> &out, B b, uint32_t dst, uint32_t a,
                 uint32_t b2) {
    out.push_back({.k = K::Bin, .b = b, .dst = dst, .a = a, .b2 = b2});
  };
  auto nt = [&]() { return new_anon_vreg(fn, 0); };
  // ---- guard block G: N, iN, gok ----
  const uint32_t z0 = konst(gi, 0);
  const uint32_t c0 = nt();
  bin(gi, is_le ? B::Le : B::Lt, c0, i0v, In.Bv);
  const uint32_t Dv = nt();
  bin(gi, B::Sub, Dv, In.Bv, i0v);
  const uint32_t msk = nt();
  bin(gi, B::Sub, msk, z0, c0);
  const uint32_t Dm = nt();
  bin(gi, B::And, Dm, Dv, msk);
  const uint32_t kd = konst(gi, In.step_mag);
  const uint32_t qv = nt();
  bin(gi, B::Div, qv, Dm, kd);
  const uint32_t Nv = nt();
  if (is_le) {
    bin(gi, B::Add, Nv, qv, c0);
  } else {
    const uint32_t rv = nt();
    bin(gi, B::Mod, rv, Dm, kd);
    const uint32_t rnz = nt();
    bin(gi, B::Ne, rnz, rv, z0);
    bin(gi, B::Add, Nv, qv, rnz);
  }
  if (vacuous) { // closed form only: no guard, no fallback loop
    gi.push_back({.k = K::Br, .t = CF});
  } else {
    const uint32_t diN = nt();
    bin(gi, B::Mul, diN, Nv, kd);
    const uint32_t iN = nt();
    bin(gi, B::Add, iN, i0v, diN);
    const uint32_t dneg = nt();
    bin(gi, B::Lt, dneg, Dv, z0);
    const uint32_t bneg = nt();
    bin(gi, is_le ? B::Le : B::Lt, bneg, iN, In.Bv);
    const uint32_t ov = nt();
    bin(gi, B::Or, ov, dneg, bneg);
    const uint32_t badv = nt();
    bin(gi, B::And, badv, c0, ov);
    const uint32_t gok = nt();
    bin(gi, B::Eq, gok, badv, z0);
    gi.push_back({.k = K::Cbr, .a = gok, .t = CF, .f = h});
  }
  // ---- closed-form block CF ----
  uint32_t Tv = NONE;
  for (const LcfEvo &e : *In.evo) // T is needed iff some increment uses i
    if (e.b != 0) {
      const uint32_t one = konst(cfi, 1);
      const uint32_t e0 = nt();
      bin(cfi, B::And, e0, Nv, one);
      const uint32_t two = nt();
      bin(cfi, B::Add, two, e0, e0);
      const uint32_t Nm = nt();
      bin(cfi, B::Sub, Nm, Nv, e0);
      const uint32_t h2 = nt();
      bin(cfi, B::LShr, h2, Nm, one);
      const uint32_t t1 = nt();
      bin(cfi, B::Add, t1, Nm, two);
      const uint32_t o1 = nt();
      bin(cfi, B::Sub, o1, t1, one);
      Tv = nt();
      bin(cfi, B::Mul, Tv, h2, o1);
      break;
    }
  std::vector<uint32_t> cfv(hphis.size(), NONE);
  for (size_t pi = 0; pi < hphis.size(); ++pi) {
    const LcfEvo &e = evo[pi];
    const uint32_t x0 = hphis[pi].pre_arg;
    if (e.b == 0 && e.g == 0) {
      cfv[pi] = x0; // untouched by the loop
      continue;
    }
    uint32_t Cv = NONE; // b*i0 + g
    if (e.b == 0) {
      Cv = konst(cfi, (val_t)e.g);
    } else {
      uint32_t bi = i0v;
      if (e.b != 1) {
        uint32_t kb = konst(cfi, (val_t)e.b);
        uint32_t t = nt();
        bin(cfi, B::Mul, t, i0v, kb);
        bi = t;
      }
      if (e.g == 0) {
        Cv = bi;
      } else {
        uint32_t kg = konst(cfi, (val_t)e.g);
        Cv = nt();
        bin(cfi, B::Add, Cv, bi, kg);
      }
    }
    uint32_t acc = x0;
    { // acc += C * N
      uint32_t cn = nt();
      bin(cfi, B::Mul, cn, Cv, Nv);
      uint32_t a2 = nt();
      bin(cfi, B::Add, a2, acc, cn);
      acc = a2;
    }
    if (e.b != 0) { // acc += (b*d) * T
      uint32_t kbd = konst(cfi, (val_t)(e.b * (uint64_t)In.step_mag));
      uint32_t bt = nt();
      bin(cfi, B::Mul, bt, Tv, kbd);
      uint32_t a3 = nt();
      bin(cfi, B::Add, a3, acc, bt);
      acc = a3;
    }
    cfv[pi] = acc;
  }
  cfi.push_back({.k = K::Br, .t = X});
  // ---- commit: append G/CF, rewrite outside uses, insert merge phis ----
  fn.blocks[G].begin = (uint32_t)fn.code.size();
  for (const Instr &x : gi)
    fn.code.push_back(x);
  fn.blocks[G].end = (uint32_t)fn.code.size();
  fn.blocks[CF].begin = (uint32_t)fn.code.size();
  for (const Instr &x : cfi)
    fn.code.push_back(x);
  fn.blocks[CF].end = (uint32_t)fn.code.size();
  fn.succ[2 * G] = CF;
  fn.succ[2 * G + 1] = vacuous ? NONE : h; // vacuous: G never enters h
  fn.succ[2 * CF] = X;
  fn.succ[2 * CF + 1] = NONE;
  { // outside uses of the loop phis -> merge-phi values (below); done
    // BEFORE the phis exist so their own loop-side args stay untouched
    std::vector<uint32_t> phidst(hphis.size(), NONE);
    for (size_t pi = 0; pi < hphis.size(); ++pi)
      phidst[pi] = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
    for (uint32_t b = 0; b < nb; ++b) {
      if (inL[b])
        continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        auto rw = [&](uint32_t &v) {
          if (v == NONE || v >= nv)
            return;
          for (size_t pi = 0; pi < hphis.size(); ++pi)
            if (v == hphis[pi].dst) {
              v = phidst[pi];
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
    std::vector<Instr> xphis{};
    for (size_t pi = 0; pi < hphis.size(); ++pi)
      xphis.push_back({.k = K::Phi,
                       .dst = phidst[pi],
                       .a = hphis[pi].dst,
                       .b2 = cfv[pi],
                       .t = h,
                       .f = CF});
    uint32_t xpos = fn.blocks[X].begin;
    fn.code.insert(fn.code.begin() + (long)xpos, xphis.begin(), xphis.end());
    const uint32_t k = (uint32_t)xphis.size();
    for (uint32_t b = 0; b < fn.nblocks(); ++b) {
      if (b == X) {
        fn.blocks[b].end += k;
        continue;
      }
      if (fn.blocks[b].begin > xpos)
        fn.blocks[b].begin += k;
      if (fn.blocks[b].end > xpos)
        fn.blocks[b].end += k;
    }
  }
  { // preheader now enters the guard instead of the header
    for (uint32_t i = fn.blocks[pre].begin; i < fn.blocks[pre].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Br && I.t == h)
        I.t = G;
    }
    if (fn.succ[2 * pre] == h)
      fn.succ[2 * pre] = G;
    if (fn.succ[2 * pre + 1] == h)
      fn.succ[2 * pre + 1] = G;
    // the header phis key their entry arg by `pre`; that edge is now
    // G->h, so the keys must follow -- otherwise SSA destruction drops
    // the entry values on the (only) path into the fallback loop and
    // the loop would start from garbage phi state
    for (uint32_t i = fn.blocks[h].begin; i < fn.blocks[h].end; ++i) {
      Instr &P = fn.code[i];
      if (P.k != K::Phi)
        break;
      if (P.t == pre)
        P.t = G;
      else if (P.f == pre)
        P.f = G;
    }
  }
  fn.recompute_preds();
  os.lcf_rt++;
  return true; // one loop per call: the fixpoint driver re-analyzes
}
