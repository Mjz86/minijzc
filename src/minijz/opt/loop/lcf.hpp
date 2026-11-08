#pragma once
// === lcf.hpp ========================================
// closed-form loop evaluation: affine recurrences -> series, exact
// compile-time simulation of non-affine bodies.

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/analysis/an.hpp"
#include "minijz/opt/linear/linform.hpp"

// --- 9d5. loop closed-form evaluation (constant trip counts) -----------------
// A counted loop with a constant trip count N whose body is pure scalar math
// (no memory, calls, prints, or inner control flow) does not need to run at
// all: every loop-carried variable x evolves as an AFFINE RECURRENCE
//     x_{k+1} = a*x_k + b*i_k + g      (i = the induction, i_k = i0 + d*k)
// whose closed form is a pair of compile-time constants (K1, K0):
//     x_N = K1 * x0 + K0,
//     K1 = a^N,  K0 = (C + B*(N-1))*S - B*T,  C = b*i0 + g,  B = b*d,
//     S(n) = sum_{j<n} a^j (geometric series),  T(n) = sum_{j<n} j*a^j
// — clang-style additive (a=1) AND geometric (a!=1) series, computed EXACTLY
// mod 2^64 by bit-doubling (no modular inverses: wrapping loops are fine).
// The loop is replaced by `x = K1*x0 + K0` in the preheader (x0 may be a
// runtime value!). Bodies that are not affine (x*x, mutual dependencies)
// fall back to exact compile-time SIMULATION of the N iterations (traps
// abort the rewrite and keep the loop). One loop per call; the fixpoint
// driver re-analyzes (inner loops fold first, enabling their parents).
inline constexpr uint64_t LCF_TRIP_BUDGET = (uint64_t)1 << 21; // ~2.1M trips
inline constexpr uint64_t LCF_SIM_BUDGET = 40000000ull;        // sim steps
constexpr uint64_t lcf_pow(uint64_t a, uint64_t n) {           // a^n mod 2^64
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
constexpr bool opt_lcf(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv)
    return false;
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  std::vector<uint32_t> defi(nv, NONE), defb(nv, NONE);
  std::vector<uint32_t> uses(nv, 0), ndefs(nv, 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < nv) {
        ++ndefs[I.dst];
        defi[I.dst] = i;
        defb[I.dst] = b;
      }
      auto bump = [&](uint32_t v) {
        if (v != NONE && v < nv)
          ++uses[v];
      };
      bump(I.a);
      bump(I.b2);
      bump(I.c2);
      for (uint32_t v : I.args)
        bump(v);
    }
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const && I.dst != NONE && I.dst < nv && ndefs[I.dst] == 1) {
        kc[I.dst] = 1;
        kv[I.dst] = I.c;
      }
    }
  std::vector<uint32_t> headers;
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h)
      headers.push_back(h);
  for (size_t i = 0; i + 1 < headers.size(); ++i)
    for (size_t j = i + 1; j < headers.size(); ++j)
      if (A.nest[headers[j]] > A.nest[headers[i]])
        std::swap(headers[i], headers[j]);
  for (uint32_t h : headers) {
    auto row = A.loops.range()[h];
    if (row.empty())
      continue;
    std::vector<char> inL(nb, 0);
    for (ulen x : row)
      inL[(uint32_t)x] = 1;
    inL[h] = 1;
    uint32_t pre = NONE, latch = NONE, outside = 0;
    for (uint32_t p : fn.preds[h]) {
      if (inL[p])
        latch = p;
      else {
        pre = p;
        ++outside;
      }
    }
    if (outside != 1 || pre == NONE || latch == NONE)
      continue;
    if (fn.blocks[h].begin == fn.blocks[h].end)
      continue;
    const Instr hterm = fn.code[fn.blocks[h].end - 1];
    if (hterm.k != K::Cbr)
      continue;
    uint32_t X = NONE, body = NONE;
    {
      bool bad = false;
      for (uint32_t b = 0; b < nb && !bad; ++b) {
        if (!inL[b])
          continue;
        for (uint32_t s : {fn.succ0(b), fn.succ1(b)}) {
          if (s == NONE)
            continue;
          if (!inL[s]) {
            if (b != h) {
              bad = true;
              break;
            }
            if (X == NONE)
              X = s;
            else if (X != s) {
              bad = true;
              break;
            }
          }
        }
      }
      if (bad || X == NONE)
        continue;
    }
    if (hterm.t == X && hterm.f != NONE && inL[hterm.f])
      body = hterm.f;
    else if (hterm.f == X && inL[hterm.t])
      body = hterm.t;
    else
      continue;
    if (body == h)
      continue; // self-loop header: rare shape, skip
    struct HPhi {
      uint32_t dst, pre_arg, latch_arg;
    };
    std::vector<HPhi> hphis;
    {
      uint32_t nphi = 0;
      for (uint32_t i = fn.blocks[h].begin; i < fn.blocks[h].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k != K::Phi)
          break;
        ++nphi;
        uint32_t pa = (I.t == pre) ? I.a : ((I.f == pre) ? I.b2 : NONE);
        uint32_t la = (I.t == latch) ? I.a : ((I.f == latch) ? I.b2 : NONE);
        if (pa == NONE || la == NONE)
          continue;
        hphis.push_back({I.dst, pa, la});
      }
      if (!nphi || hphis.size() != nphi)
        continue;
    }
    // ---- induction phi + header test (same shape unroll requires) ----------
    uint32_t ind = NONE, ind_idx = NONE, cond = hterm.a;
    val_t step_mag = 0;
    bool step_sub = false;
    if (cond == NONE || cond >= nv || defi[cond] == NONE || defb[cond] != h)
      continue;
    for (size_t pi = 0; pi < hphis.size() && ind == NONE; ++pi) {
      uint32_t L = hphis[pi].latch_arg;
      if (L >= nv || defi[L] == NONE || !inL[defb[L]])
        continue;
      const Instr D = fn.code[defi[L]];
      if (D.k != K::Bin)
        continue;
      val_t mag = 0;
      bool sub = false, okst = false;
      if (D.b == B::Add) {
        if (D.a == hphis[pi].dst && D.b2 < nv && kc[D.b2] && kv[D.b2] != 0) {
          mag = kv[D.b2];
          okst = true;
        } else if (D.b2 == hphis[pi].dst && D.a < nv && kc[D.a] &&
                   kv[D.a] != 0) {
          mag = kv[D.a];
          okst = true;
        }
      } else if (D.b == B::Sub) {
        if (D.a == hphis[pi].dst && D.b2 < nv && kc[D.b2] && kv[D.b2] != 0) {
          mag = kv[D.b2];
          sub = true;
          okst = true;
        }
      }
      if (!okst)
        continue;
      const Instr C = fn.code[defi[cond]];
      if (C.k != K::Bin || (C.b != B::Lt && C.b != B::Le))
        continue;
      bool uses_it = false;
      if (C.a == hphis[pi].dst && C.b2 < nv)
        uses_it = true;
      else if (C.b2 == hphis[pi].dst && C.a < nv)
        uses_it = true;
      if (!uses_it)
        continue;
      ind = hphis[pi].dst;
      ind_idx = (uint32_t)pi;
      step_mag = mag;
      step_sub = sub;
    }
    if (ind == NONE) {
      // ---- GENERAL SIMULATOR --------------------------------------------
      // The test is not a counted `<`/`<=` induction comparison (e.g.
      // `while (h != 0)` -- Euclid's GCD). If every loop input is a known
      // constant and the body is straight-line pure math, the loop's whole
      // execution is determined: simulate header test + body chain per
      // iteration with the program's exact wrapping/trapping arithmetic; a
      // trap or budget overflow aborts (the loop stays). This is the pass
      // that folds the GCD demo to the constant 21 -- note it needs no
      // induction variable at all, just constants and purity.
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
          for (uint32_t i = fn.blocks[b2].begin; i < fn.blocks[b2].end && ok;
               ++i) {
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
          for (uint32_t i = fn.blocks[b2].begin; i < fn.blocks[b2].end && ok;
               ++i) {
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
      if (ok) {
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
          if (val[hterm.a] == 0) { // header test fails: exit with the
            done = true;           // current phi values as final values
            break;
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
        if (done && !trap) {
          // ---- commit: consts into the preheader; the loop dies --------
          std::vector<uint32_t> finalv(hphis.size(), NONE);
          std::vector<Instr> batch{};
          for (size_t pi = 0; pi < hphis.size(); ++pi) {
            const uint32_t fv = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
            finalv[pi] = fv;
            batch.push_back(
                {.k = K::Const, .dst = fv, .c = val[hphis[pi].dst]});
          }
          { // splice into the preheader, before its terminator
            uint32_t pe = fn.blocks[pre].end - 1;
            fn.code.insert(fn.code.begin() + (long)pe, batch.begin(),
                           batch.end());
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
      continue; // not general-simulable: try nothing else for this loop
    }
    const HPhi IP = hphis[ind_idx];
    const Instr C = fn.code[defi[cond]];
    const bool lhs_is_ind = (C.a == ind);
    const uint32_t Bv = lhs_is_ind ? C.b2 : C.a; // the bound operand
    const bool i0_const = kc[IP.pre_arg];
    const bool bound_const = (Bv != NONE && Bv < nv && kc[Bv]);
    // a runtime bound must be defined outside the loop (otherwise the trip
    // count depends on the body and no closed form exists)
    if (!bound_const) {
      if (Bv == NONE || Bv >= nv)
        continue;
      if (defi[Bv] != NONE && inL[defb[Bv]])
        continue;
    }
    // ---- exact compile-time trip count N (when both sides are constant) ----
    uint64_t N = ~0ull;
    if (i0_const && bound_const) {
      val_t i0 = kv[IP.pre_arg];
      val_t bound = kv[Bv];
      val_t iv = i0;
      for (uint64_t k = 0;; ++k) {
        val_t lhs = lhs_is_ind ? iv : bound;
        val_t rhs = lhs_is_ind ? bound : iv;
        bool go = (C.b == B::Lt) ? (lhs < rhs) : (lhs <= rhs);
        if (!go) {
          N = k;
          break;
        }
        if (k >= LCF_TRIP_BUDGET)
          break;
        iv = step_sub ? w_sub(iv, step_mag).v : w_add(iv, step_mag).v;
      }
    }
    const bool ct_trip = (N != ~0ull);
    if (ct_trip && N == 0) { // zero trips: test provably false -> exit now
      Instr &T = fn.code[fn.blocks[h].end - 1];
      T.k = K::Br;
      T.t = X;
      T.f = NONE;
      T.a = NONE;
      fn.succ[2 * h] = X;
      fn.succ[2 * h + 1] = NONE;
      fn.recompute_preds();
      return true;
    }
    // ---- safety: outside uses may reference ONLY header phi dsts ----------
    {
      std::vector<char> phid(nv, 0);
      for (const HPhi &p : hphis)
        phid[p.dst] = 1;
      bool bad = false;
      for (uint32_t b = 0; b < nb && !bad; ++b) {
        if (inL[b])
          continue;
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          auto chk = [&](uint32_t v) {
            if (v != NONE && v < nv && defi[v] != NONE && inL[defb[v]] &&
                !phid[v])
              bad = true;
          };
          chk(I.a);
          chk(I.b2);
          chk(I.c2);
          for (uint32_t v : I.args)
            chk(v);
          if (bad)
            break;
        }
      }
      if (bad)
        continue;
    }
    // pre must not already reach the exit (phi rekey would duplicate preds)
    {
      bool bad = false;
      for (uint32_t p : fn.preds[X])
        if (p == pre)
          bad = true;
      if (bad)
        continue;
    }
    // ---- body purity: straight-line scalar math only ----------------------
    // (header: phis + Const/Copy/Bin + the Cbr; other blocks: Const/Copy/Bin
    //  ending in Br — no memory, calls, prints, or inner branches)
    {
      bool ok = true;
      for (uint32_t b = 0; b < nb && ok; ++b) {
        if (!inL[b])
          continue;
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (i + 1 == fn.blocks[b].end) { // terminator
            if (b == h) {
              if (I.k != K::Cbr)
                ok = false;
            } else if (I.k != K::Br)
              ok = false;
            break;
          }
          if (I.k == K::Phi) {
            if (b != h)
              ok = false;
          } else if (I.k == K::Const || I.k == K::Copy) {
          } else if (I.k == K::Bin) {
          } else
            ok = false;
          if (!ok)
            break;
        }
      }
      if (!ok)
        continue;
    }
    // ---- may-trap scan: Div/Mod that is not PROVEN trap-free ------------
    // The analytic closed form cannot preserve a runtime trap (it deletes the
    // loop); such loops are only foldable by the trap-checking SIMULATOR, or
    // once trap-elimination has marked every Div/Mod in the loop `nt`.
    bool loop_maytrap = false;
    for (uint32_t b = 0; b < nb && !loop_maytrap; ++b) {
      if (!inL[b])
        continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k != K::Bin || (I.b != B::Div && I.b != B::Mod))
          continue;
        if (I.nt)
          continue; // proven trap-free by the range analysis
        if (!(I.b2 < nv && kc[I.b2] && kv[I.b2] != 0)) {
          loop_maytrap = true;
          break;
        }
      }
    }
    // ---- analytic: every phi must evolve as  x' = a*x + b*i + g ------------
    struct Evo {
      bool ok;
      uint64_t a, b, g;
    };
    std::vector<Evo> evo(hphis.size(), {false, 0, 0, 0});
    bool analytic = true;
    {
      std::vector<char> scope(nb, 0);
      for (uint32_t b = 0; b < nb; ++b)
        scope[b] = inL[b];
      for (size_t pi = 0; pi < hphis.size() && analytic; ++pi) {
        LinForm L{};
        std::vector<uint32_t> absr{};
        lin_expand(fn, nv, defi, defb, kc, kv, uses, ndefs, scope, NONE, L,
                   (val_t)1, hphis[pi].latch_arg, absr);
        (void)absr; // informational: the whole loop is removed anyway
        Evo e{true, 0, 0, (uint64_t)L.c};
        for (const auto &p : L.t) {
          if (p.first == hphis[pi].dst)
            e.a = (uint64_t)p.second;
          else if (p.first == ind)
            e.b = (uint64_t)p.second;
          else
            e.ok = false;
        }
        evo[pi] = e;
        analytic &= e.ok;
      }
    }
    std::vector<uint32_t> finalv(hphis.size(), NONE);
    bool folded = false;
    if (!ct_trip) {
      // ---- runtime trip count: guarded closed form + the loop as fallback -
      // Shape: `while (i < B) { pure body; i += d; }` with constant d >= 1
      // and a RUNTIME B (or a constant bound too big to step at compile
      // time). The trip count is computed at runtime together with a
      // wrap-safety guard; the guard fails exactly when the induction would
      // leave the signed range before reaching B (the honest answer is then
      // the original loop), so the closed form only ever runs when exact:
      //   c0 = (i0 < B);  D = B - i0 (wrapping);  Dm = c0 ? D : 0;
      //   N = ceil(Dm/d) [Lt]  or  Dm/d + c0 [Le];   iN = i0 + N*d;
      //   gok = !(c0 && ((D < 0) || (iN < B)))   [Lt; Le: iN <= B]
      // Every loop-carried x evolves additively (a == 1):
      //   x_N = x0 + (b*i0 + g)*N + (b*d) * N(N-1)/2
      // (T = N(N-1)/2 exactly mod 2^64 via the even-factor trick).
      bool rt_ok = !step_sub && step_mag >= 1 && lhs_is_ind;
      if (rt_ok) { // exit must have exactly one pred (the header), no phis
        if (fn.preds[X].size() != 1 || fn.preds[X][0] != h)
          rt_ok = false;
        for (uint32_t i = fn.blocks[X].begin; i < fn.blocks[X].end && rt_ok;
             ++i)
          if (fn.code[i].k == K::Phi)
            rt_ok = false;
        const Instr &pt = fn.code[fn.blocks[pre].end - 1];
        if (pt.k != K::Br || pt.t != h)
          rt_ok = false; // also idempotence
        for (const Evo &e : evo)
          if (!e.ok || e.a != 1)
            rt_ok = false; // additive recurrences only
      }
      if (!rt_ok)
        continue;
      // ---- vacuous-guard proof --------------------------------------------
      // With a CONSTANT i0 >= 0 and unit step, the induction can never wrap
      // before the exit: whenever the condition c0 holds, D = B - i0 lands
      // in (0, MAX] (no wrap, since B <= MAX and i0 >= 0), so `dneg' is
      // false; and iN = i0 + N reproduces B bit-exactly (N = Dm, and
      // i0 + (B - i0) = B in the mod-2^64 ring), so `bneg' is false too.
      // The guard is therefore identically false for EVERY runtime B. If
      // additionally the loop body is pure arithmetic that provably cannot
      // trap (Div/Mod carry `nt`) and observes nothing, the closed form is
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
                           step_mag == 1;
      const bool is_le = (C.b == B::Le);
      const uint32_t i0v = IP.pre_arg;
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
      bin(gi, is_le ? B::Le : B::Lt, c0, i0v, Bv);
      const uint32_t Dv = nt();
      bin(gi, B::Sub, Dv, Bv, i0v);
      const uint32_t msk = nt();
      bin(gi, B::Sub, msk, z0, c0);
      const uint32_t Dm = nt();
      bin(gi, B::And, Dm, Dv, msk);
      const uint32_t kd = konst(gi, step_mag);
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
        bin(gi, is_le ? B::Le : B::Lt, bneg, iN, Bv);
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
      for (const Evo &e : evo) // T is needed iff some increment uses i
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
        const Evo &e = evo[pi];
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
          uint32_t kbd = konst(cfi, (val_t)(e.b * (uint64_t)step_mag));
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
        fn.code.insert(fn.code.begin() + (long)xpos, xphis.begin(),
                       xphis.end());
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
    auto splice_into_preheader = [&](std::vector<Instr> &batch) {
      uint32_t pe = fn.blocks[pre].end - 1; // terminator (pre-insert)
      fn.code.insert(fn.code.begin() + (long)pe, batch.begin(), batch.end());
      const uint32_t k = (uint32_t)batch.size();
      for (uint32_t x = 0; x < nb; ++x) {
        if (x == pre) {
          fn.blocks[x].end += k;
          continue;
        }
        if (fn.blocks[x].begin >= pe + 1)
          fn.blocks[x].begin += k;
        if (fn.blocks[x].end >= pe + 1)
          fn.blocks[x].end += k;
      }
    };
    if (analytic && !loop_maytrap) {
      // closed form: x_N = K1*x0 + K0 with K1 = a^N and the series constants
      std::vector<Instr> batch{};
      const uint64_t Nm1 = N - 1;
      const uint64_t i0u = (uint64_t)kv[IP.pre_arg];
      const uint64_t du =
          step_sub ? (uint64_t)0 - (uint64_t)step_mag : (uint64_t)step_mag;
      for (size_t pi = 0; pi < hphis.size(); ++pi) {
        const Evo &e = evo[pi];
        uint64_t S = 0, T = 0, P = 0;
        lcf_geo(e.a, N, S, T, P);
        const uint64_t K1 = P; // a^N
        const uint64_t Cc = e.b * i0u + e.g;
        const uint64_t Bc = e.b * du;
        const uint64_t K0 = (Cc + Bc * Nm1) * S - Bc * T;
        const uint32_t x0 = hphis[pi].pre_arg;
        const uint32_t fv = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
        finalv[pi] = fv;
        if (K1 == 1 && K0 == 0) {
          batch.push_back({.k = K::Copy, .dst = fv, .a = x0});
        } else if (K1 == 1) {
          uint32_t c = new_anon_vreg(fn, 0);
          batch.push_back({.k = K::Const, .dst = c, .c = (val_t)K0});
          batch.push_back(
              {.k = K::Bin, .b = B::Add, .dst = fv, .a = x0, .b2 = c});
        } else if (K0 == 0) {
          uint32_t c = new_anon_vreg(fn, 0);
          batch.push_back({.k = K::Const, .dst = c, .c = (val_t)K1});
          batch.push_back(
              {.k = K::Bin, .b = B::Mul, .dst = fv, .a = x0, .b2 = c});
        } else {
          uint32_t c1 = new_anon_vreg(fn, 0);
          uint32_t t = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
          batch.push_back({.k = K::Const, .dst = c1, .c = (val_t)K1});
          batch.push_back(
              {.k = K::Bin, .b = B::Mul, .dst = t, .a = x0, .b2 = c1});
          uint32_t c2 = new_anon_vreg(fn, 0);
          batch.push_back({.k = K::Const, .dst = c2, .c = (val_t)K0});
          batch.push_back(
              {.k = K::Bin, .b = B::Add, .dst = fv, .a = t, .b2 = c2});
        }
      }
      splice_into_preheader(batch);
      os.lcf_loops++;
      folded = true;
    } else {
      // ---- fallback: exact compile-time simulation of all N iterations -----
      bool ok = true;
      for (const HPhi &p : hphis)
        if (!(p.pre_arg < nv && kc[p.pre_arg]))
          ok = false;
      uint64_t loop_instrs = 0;
      for (uint32_t b = 0; b < nb; ++b)
        if (inL[b])
          loop_instrs += fn.blocks[b].end - fn.blocks[b].begin;
      if (ok && N * loop_instrs > LCF_SIM_BUDGET)
        ok = false;
      if (ok) { // every outside-defined operand must be a known constant
        for (uint32_t b = 0; b < nb && ok; ++b) {
          if (!inL[b])
            continue;
          for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end && ok;
               ++i) {
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
      if (ok) { // the straight-line body chain, from `body` to the latch
        std::vector<uint32_t> chain{};
        uint32_t cur = body, guard = 0;
        while (cur != h && guard++ <= nb) {
          chain.push_back(cur);
          cur = fn.succ0(cur);
        }
        if (cur != h)
          ok = false;
        if (ok) {
          std::vector<val_t> val(nv, 0);
          for (const HPhi &p : hphis)
            val[p.dst] = kv[p.pre_arg];
          std::vector<val_t> nxt(hphis.size());
          bool trap = false;
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
          for (uint64_t it = 0; it < N && !trap; ++it) {
            for (uint32_t i = fn.blocks[h].begin; i + 1 < fn.blocks[h].end;
                 ++i) {
              if (fn.code[i].k == K::Phi)
                continue;
              if (!eval(fn.code[i]))
                trap = true;
            }
            if (trap)
              break;
            for (uint32_t cb : chain) {
              for (uint32_t i = fn.blocks[cb].begin; i < fn.blocks[cb].end; ++i)
                if (!eval(fn.code[i]))
                  trap = true;
              if (trap)
                break;
            }
            if (trap)
              break;
            for (size_t pi = 0; pi < hphis.size(); ++pi)
              nxt[pi] = val[hphis[pi].latch_arg];
            for (size_t pi = 0; pi < hphis.size(); ++pi)
              val[hphis[pi].dst] = nxt[pi];
          }
          if (!trap) {
            std::vector<Instr> batch{};
            for (size_t pi = 0; pi < hphis.size(); ++pi) {
              const uint32_t fv = new_anon_vreg(fn, fn.base_of(hphis[pi].dst));
              finalv[pi] = fv;
              batch.push_back(
                  {.k = K::Const, .dst = fv, .c = val[hphis[pi].dst]});
            }
            splice_into_preheader(batch);
            os.lcf_sims++;
            folded = true;
          }
        }
      }
    }
    if (!folded)
      continue;
    // ---- rewire: preheader jumps straight to the exit; loop dies -----------
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
    for (uint32_t b = 0; b < nb; ++b) { // outside uses of phis -> final values
      if (inL[b])
        continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
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
      Instr &I = fn.code[i]; // exit phis: the h-edge now departs from pre
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
  return false;
}

// After passes convert phis to Copy/Const in place, the "all phis first"
// block invariant breaks — consumers scan until the first non-phi. Restore
// it by stably moving remaining phis to the front of each block.
constexpr void normalize_phis(Fn &fn) {
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    uint32_t begin = fn.blocks[b].begin, end = fn.blocks[b].end;
    if (end - begin < 2)
      continue;
    bool need = false;
    for (uint32_t i = begin; i < end; ++i) {
      if (fn.code[i].k == K::Phi)
        continue;
      for (uint32_t j = i + 1; j < end; ++j)
        if (fn.code[j].k == K::Phi) {
          need = true;
          break;
        }
      break;
    }
    if (!need)
      continue;
    std::vector<Instr> phis{}, rest{};
    for (uint32_t i = begin; i < end; ++i)
      (fn.code[i].k == K::Phi ? phis : rest).push_back(fn.code[i]);
    uint32_t w = begin;
    for (const Instr &I : phis)
      fn.code[w++] = I;
    for (const Instr &I : rest)
      fn.code[w++] = I;
  }
}

// ============================================================================
