#pragma once
// === lcf_sim.hpp ========================================
// the GENERAL loop simulator of the LCF pass: a loop whose test is not
// a counted `<`/`<=` induction comparison (e.g. `while (h != 0)` --
// Euclid's GCD), but whose every loop input is a known constant and
// whose body is straight-line pure math, has its whole execution
// determined: header test + body chain are simulated per iteration
// with the program's exact wrapping/trapping arithmetic; a trap or
// budget overflow aborts (the loop stays). This is the pass that folds
// the GCD demo to the constant 21 -- it needs no induction variable
// at all, just constants and purity. Extracted from lcf.hpp
// (checkpoint 18's code-quality split); the counter is
// OptStats::lcf_gensims.
// ---- ALGORITHM --------------------------------------------------------
// lcf_sim_fold(In), all gates before any edit: (1) every hphi's
// pre_arg is a known constant (kc); (2) purity -- header = phis +
// Const/Copy/Bin + Cbr, other in-loop blocks + Br only; (3) outside
// uses reference ONLY header phi dsts; (4) pre does not already
// reach X (the rekey would dup preds); (5) the body chain from
// In.body to h via succ0 is straight-line; (6) every outside-defined
// operand used inside is a constant. Then the loop: per iteration,
// bounded by LCF_TRIP_BUDGET (2^21 trips), eval the header's
// non-phi instrs, test the header condition with In's polarity
// (exit_on_true: break-style headers exit on TRUE), eval the chain,
// count steps against LCF_SIM_BUDGET (40M), then swap the phis from
// the nxt snapshot. done && !trap -> commit: the finalv consts
// splice into the preheader; pre's br retargets h -> X (the loop
// dies); outside uses rewire to finalv; X's phis rekey h -> pre;
// ---- DATA STRUCTURES --------------------------------------------------
// LcfSimIn (h/pre/latch/X/body/hterm/exit_on_true + the shared
// tables), val/nxt, eval, chain, finalv, batch, and the budgets.
// ---- THEORY: exact N-iteration simulation -----------------------------
// Every loop input is a known constant and the body is straight-
// line pure math, so the whole execution is determined; the
// simulator evaluates it with w_bin, the program's own wrapping /
// trapping arithmetic, so the result is bit-exact by construction.
// A trap (w_bin sets r.trap) or a budget overflow aborts and the
// loop stays, untouched. The nxt snapshot gives one iteration the
// loop-carried semantics: every hphi's new value is the OLD
// latch_arg of the same iteration, exactly as the phi node defines.
// ---- WHAT IT ENABLES --------------------------------------------------
// Folds loops with no counted induction at all (the GCD demo folds
// to the constant 21); the counter is os.lcf_gensims.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "loop_shape.hpp"

// budgets shared by the simulator (here) and lcf.hpp's trip counters
inline constexpr uint64_t LCF_TRIP_BUDGET = (uint64_t)1 << 21; // ~2.1M trips
inline constexpr uint64_t LCF_SIM_BUDGET = 40000000ull;        // sim steps

// ---- lcf_sim_fold: fold one loop by exact simulation ---------------------
// Call contract (lcf.hpp enforces the loop shape before calling):
//   * h's cbr tested an SSA value; body is the in-loop arm
//   * hphis keyed (pre, latch); the loop has no other entry
// Returns true when the loop was folded to constants.
struct LcfSimIn {
  uint32_t h{}, pre{}, latch{}, X{}, body{};
  Instr hterm{};       // h's terminator (Cbr on hterm.a)
  bool exit_on_true{}; // checkpoint 19: break-style headers (a folded
  // `break` became the test: the TRUE arm leaves) exit on test-TRUE, not
  // on test-false — the simulator must evaluate the polarity it is given,
  // the way the analytic trip counter does (the checkpoint-17 fix covered
  // only that side; the body-if unswitch bait reshapes loops this way and
  // exposed the simulator's)
  const std::vector<HPhi> *hphis{};
  const std::vector<char> *inL{};
  const std::vector<char> *kc{};
  const std::vector<val_t> *kv{};
  const std::vector<uint32_t> *defi{}, *defb{};
};
constexpr bool lcf_sim_fold(Fn &fn, OptStats &os, const LcfSimIn &In) {
  const std::vector<HPhi> &hphis = *In.hphis;
  const std::vector<char> &inL = *In.inL, &kc = *In.kc;
  const std::vector<val_t> &kv = *In.kv;
  const std::vector<uint32_t> &defi = *In.defi, &defb = *In.defb;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = (uint32_t)kc.size();
  const uint32_t h = In.h, pre = In.pre, X = In.X, body = In.body;
  bool ok = true;
  for (const HPhi &p : hphis)
    if (!(p.pre_arg < nv && kc[p.pre_arg]))
      ok = false;
  { // purity: header = phis + Const/Copy/Bin + Cbr; others + Br only
    for (uint32_t b2 = 0; b2 < nb && ok; ++b2) {
      if (!inL[b2])
        continue;
      for (uint32_t i = fn.blocks[b2].begin; i < fn.blocks[b2].end; ++i) {
        const Instr &I = fn.code[i];
        if (i + 1 == fn.blocks[b2].end) {
          if (b2 == h) {
            if (I.k != K::Cbr)
              ok = false;
          } else if (I.k != K::Br)
            ok = false;
          break;
        }
        if (I.k == K::Phi) {
          if (b2 != h)
            ok = false;
        } else if (I.k == K::Const || I.k == K::Copy) {
        } else if (I.k == K::Bin) {
        } else
          ok = false;
        if (!ok)
          break;
      }
    }
  }
  { // outside uses may reference ONLY header phi dsts
    std::vector<char> phid(nv, 0);
    for (const HPhi &p : hphis)
      phid[p.dst] = 1;
    for (uint32_t b2 = 0; b2 < nb && ok; ++b2) {
      if (inL[b2])
        continue;
      for (uint32_t i = fn.blocks[b2].begin; i < fn.blocks[b2].end && ok; ++i) {
        const Instr &I = fn.code[i];
        auto chk = [&](uint32_t v) {
          if (v != NONE && v < nv && defi[v] != NONE && inL[defb[v]] &&
              !phid[v])
            ok = false;
        };
        chk(I.a);
        chk(I.b2);
        chk(I.c2);
        for (uint32_t v : I.args)
          chk(v);
      }
    }
  }
  { // pre must not already reach the exit (phi rekey would dup preds)
    for (uint32_t p : fn.preds[X])
      if (p == pre)
        ok = false;
  }
  std::vector<uint32_t> chain{};
  if (ok) { // the straight-line body chain from `body` to the latch
    uint32_t cur2 = body, guard = 0;
    while (cur2 != h && guard++ <= nb) {
      chain.push_back(cur2);
      cur2 = fn.succ0(cur2);
    }
    if (cur2 != h)
      ok = false;
  }
  if (ok) { // every outside-defined operand used inside is a constant
    for (uint32_t b2 = 0; b2 < nb && ok; ++b2) {
      if (!inL[b2])
        continue;
      for (uint32_t i = fn.blocks[b2].begin; i < fn.blocks[b2].end && ok; ++i) {
        const Instr &I = fn.code[i];
        auto ck = [&](uint32_t v) {
          if (v != NONE && v < nv && !(defi[v] != NONE && inL[defb[v]]) &&
              !kc[v])
            ok = false;
        };
        ck(I.a);
        ck(I.b2);
        ck(I.c2);
      }
    }
  }
  if (!ok)
    return false;
  {
    std::vector<val_t> val(nv, 0);
    for (const HPhi &p : hphis)
      val[p.dst] = kv[p.pre_arg];
    std::vector<val_t> nxt(hphis.size());
    bool trap = false, done = false;
    uint64_t steps = 0;
    auto eval = [&](const Instr &I) -> bool {
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
    };
    for (uint64_t it = 0; it < LCF_TRIP_BUDGET && !trap && !done; ++it) {
      for (uint32_t i = fn.blocks[h].begin; i + 1 < fn.blocks[h].end; ++i) {
        if (fn.code[i].k == K::Phi)
          continue;
        if (!eval(fn.code[i]))
          trap = true;
      }
      if (trap)
        break;
      if (In.exit_on_true ? (val[In.hterm.a] != 0)
                          : (val[In.hterm.a] == 0)) { // the header test
        done = true; // exits NOW (break-style: on TRUE; the ordinary while
        break;       // shape: on FALSE) — current phis are the finals
      }
      for (uint32_t cb : chain) {
        for (uint32_t i = fn.blocks[cb].begin; i + 1 < fn.blocks[cb].end;
             ++i) // skip the trailing Br (purity-checked above)
          if (!eval(fn.code[i]))
            trap = true;
        if (trap)
          break;
      }
      if (trap)
        break;
      if ((steps += 1 + chain.size()) > LCF_SIM_BUDGET)
        break;
      for (size_t pi = 0; pi < hphis.size(); ++pi)
        nxt[pi] = val[hphis[pi].latch_arg];
      for (size_t pi = 0; pi < hphis.size(); ++pi)
        val[hphis[pi].dst] = nxt[pi];
    }
    if (!(done && !trap))
      return false;
    // ---- commit: consts into the preheader; the loop dies --------
    std::vector<uint32_t> finalv(hphis.size(), NONE);
    std::vector<Instr> batch{};
    for (size_t pi = 0; pi < hphis.size(); ++pi) {
      const uint32_t fv = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
      finalv[pi] = fv;
      batch.push_back({.k = K::Const, .dst = fv, .c = val[hphis[pi].dst]});
    }
    { // splice into the preheader, before its terminator
      uint32_t pe = fn.blocks[pre].end - 1;
      fn.code.insert(fn.code.begin() + (long)pe, batch.begin(), batch.end());
      const uint32_t kk = (uint32_t)batch.size();
      for (uint32_t x = 0; x < nb; ++x) {
        if (x == pre) {
          fn.blocks[x].end += kk;
          continue;
        }
        if (fn.blocks[x].begin >= pe + 1)
          fn.blocks[x].begin += kk;
        if (fn.blocks[x].end >= pe + 1)
          fn.blocks[x].end += kk;
      }
    }
    os.lcf_gensims++;
    // rewire: the preheader jumps straight to the exit; loop dies
    for (uint32_t i = fn.blocks[pre].begin; i < fn.blocks[pre].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Br && I.t == h)
        I.t = X;
      else if (I.k == K::Cbr) {
        if (I.t == h)
          I.t = X;
        if (I.f == h)
          I.f = X;
      }
    }
    if (fn.succ[2 * pre] == h)
      fn.succ[2 * pre] = X;
    if (fn.succ[2 * pre + 1] == h)
      fn.succ[2 * pre + 1] = X;
    for (uint32_t b2 = 0; b2 < nb; ++b2) {
      if (inL[b2])
        continue;
      for (uint32_t i = fn.blocks[b2].begin; i < fn.blocks[b2].end; ++i) {
        Instr &I = fn.code[i];
        auto rw = [&](uint32_t &v) {
          if (v == NONE || v >= nv)
            return;
          for (size_t pi = 0; pi < hphis.size(); ++pi)
            if (v == hphis[pi].dst) {
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
    for (uint32_t i = fn.blocks[X].begin; i < fn.blocks[X].end; ++i) {
      Instr &I = fn.code[i]; // exit phis: h-edge departs from pre now
      if (I.k != K::Phi)
        break;
      if (I.t == h)
        I.t = pre;
      else if (I.f == h)
        I.f = pre;
    }
    fn.recompute_preds();
    return true; // one loop per call: the driver re-analyzes
  }
}
