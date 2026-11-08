#pragma once
// === unroll.hpp ========================================
// full loop unrolling for constant trip counts.

#include "minijz/base/pass_cfg.hpp"
#include "minijz/ir/instr.hpp"
#include "minijz/opt/analysis/an.hpp"

// --- 9d2. loop unrolling
// ------------------------------------------------------- Full unroll of simple
// counted loops with a CONSTANT trip count, computed EXACTLY (the induction
// advances with the program's wrapping add/sub, so even wrap-around trip counts
// are honored; the count is capped at `limit`). Shape required (all from the
// lib's natural-loop analysis):
//   * one back edge: the header's unique in-loop predecessor (the latch)
//   * one outside predecessor (the preheader)
//   * the ONLY exit edge is the header's comparison (no `break`s)
//   * induction phi i = phi(i0, i+c) with constant i0 and c != 0
//   * header test `i < bound` / `i <= bound` (either operand order), bound
//     constant
// The loop becomes K body copies + a tail copy of the header (whose test is
// provably false -> Br exit). Header phis turn into per-copy constants (the
// induction) and chained values (accumulators); uses outside the loop are
// rewritten to the tail's exit values; the exit block's phis are rekeyed
// from the header to the tail. constprop + DCE then fold pure arithmetic
// loops into constants at compile time.
constexpr bool opt_unroll(Fn &fn, const An &A, OptStats &os, uint32_t limit) {
  if (!limit)
    return false;
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv)
    return false;
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  std::vector<uint32_t> defi(nv, NONE), defb(nv, NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < nv) {
        defi[I.dst] = i;
        defb[I.dst] = b;
      }
      if (I.k == K::Const && I.dst != NONE && I.dst < nv) {
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
    // unique outside pred (preheader) + unique latch
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
    // the only exit edge is the header's; find the exit and the body arm
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
            } // a break: not unrollable
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
      continue; // self-loop header: skip this rare shape
    // ---- header phis: (dst, pre_arg, latch_arg); ALL of them must match --
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
    // ---- find the induction phi + the header test ------------------------
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
      // the header test compares THIS phi against a constant bound
      const Instr C = fn.code[defi[cond]];
      if (C.k != K::Bin || (C.b != B::Lt && C.b != B::Le))
        continue;
      bool uses_it = false;
      if (C.a == hphis[pi].dst && C.b2 < nv && kc[C.b2])
        uses_it = true;
      else if (C.b2 == hphis[pi].dst && C.a < nv && kc[C.a])
        uses_it = true;
      if (!uses_it)
        continue;
      if (!kc[hphis[pi].pre_arg])
        continue; // i0 must be constant
      ind = hphis[pi].dst;
      ind_idx = (uint32_t)pi;
      step_mag = mag;
      step_sub = sub;
    }
    if (ind == NONE)
      continue;
    const HPhi IP = hphis[ind_idx];
    const val_t i0 = kv[IP.pre_arg];
    const Instr C = fn.code[defi[cond]];
    const bool lhs_is_ind = (C.a == ind);
    const val_t bound = lhs_is_ind ? kv[C.b2] : kv[C.a];
    uint32_t K = NONE;
    std::vector<val_t> ivk{}; // ivk[k] = induction value AT iteration k,
                              // including the failing K-th test (the exit
                              // value the loop leaves in i)
    {
      val_t iv = i0;
      for (uint32_t k = 0;; ++k) {
        val_t lhs = lhs_is_ind ? iv : bound;
        val_t rhs = lhs_is_ind ? bound : iv;
        bool go = (C.b == B::Lt) ? (lhs < rhs) : (lhs <= rhs);
        ivk.push_back(iv);
        if (!go) {
          K = k;
          break;
        }
        if (k >= limit)
          break; // more than `limit` iterations: skip
        iv = step_sub ? w_sub(iv, step_mag).v : w_add(iv, step_mag).v;
      }
    }
    if (K == NONE || K > limit)
      continue;
    uint32_t loop_instrs = 0;
    for (uint32_t b = 0; b < nb; ++b)
      if (inL[b])
        loop_instrs += fn.blocks[b].end - fn.blocks[b].begin;
    if ((K + 1) * loop_instrs > 400)
      continue; // code growth cap
    // ---- safety: outside uses may only reference header-defined values ----
    {
      std::vector<char> loopdef(nv, 0);
      for (uint32_t v = 0; v < nv; ++v)
        if (defi[v] != NONE && inL[defb[v]])
          loopdef[v] = 1;
      bool bad = false;
      for (uint32_t b = 0; b < nb && !bad; ++b) {
        if (inL[b])
          continue;
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          auto chk = [&](uint32_t v) {
            if (v != NONE && v < nv && loopdef[v] && defb[v] != h)
              bad = true;
          };
          if (I.k == K::Phi) { // only the exit edge from h may carry it
            chk(I.a);
            chk(I.b2);
            if (bad && b == X) {
              // args keyed to preds other than h must not be loop values
              bad = false;
              if (I.t != NONE && I.t != h && I.a != NONE && I.a < nv &&
                  loopdef[I.a])
                bad = true;
              if (I.f != NONE && I.f != h && I.b2 != NONE && I.b2 < nv &&
                  loopdef[I.b2])
                bad = true;
            }
          } else {
            chk(I.a);
            chk(I.b2);
            chk(I.c2);
            for (uint32_t v : I.args)
              chk(v);
          }
          if (bad)
            break;
        }
      }
      if (bad)
        continue;
    }
    // ================= commit =================
    os.unrolls++;
    if (K == 0) {
      // zero iterations: the (provably false) test becomes Br exit; the
      // unreachable body dies and the header phis degenerate to their
      // preheader args in the next unreachable/phi-degen round
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
    // block / vreg maps for copies 0..K (copy K = header-only tail)
    std::vector<std::vector<uint32_t>> bmap(K + 1,
                                            std::vector<uint32_t>(nb, NONE));
    std::vector<std::vector<uint32_t>> fresh(K + 1,
                                             std::vector<uint32_t>(nv, NONE));
    for (uint32_t k = 0; k <= K; ++k)
      for (uint32_t b = 0; b < nb; ++b) {
        if (!inL[b])
          continue;
        if (k == K && b != h)
          continue; // tail: header only
        uint32_t id = (uint32_t)fn.blocks.size();
        fn.blocks.push_back({});
        fn.succ.push_back(NONE);
        fn.succ.push_back(NONE);
        bmap[k][b] = id;
      }
    for (uint32_t k = 0; k <= K; ++k)
      for (uint32_t v = 0; v < nv; ++v) {
        if (defi[v] == NONE || !inL[defb[v]])
          continue;
        if (k == K && defb[v] != h)
          continue;
        fresh[k][v] = new_anon_vreg(fn, fn.base_of(v));
      }
    // entry value of each header phi at copy k (uses of the phi see this)
    //   induction: the constant i0 + k*step (materialized in the header)
    //   accumulator: k == 0 ? pre_arg : map(latch_arg, k-1)
    std::vector<std::vector<uint32_t>> entry(
        K + 1, std::vector<uint32_t>(hphis.size(), NONE));
    // map a vreg at copy k (uses): phi -> entry, loop def -> fresh, else v
    std::vector<uint32_t> phi_idx(nv, NONE);
    for (size_t pi = 0; pi < hphis.size(); ++pi)
      phi_idx[hphis[pi].dst] = (uint32_t)pi;
    auto mapv = [&](uint32_t v, uint32_t k) -> uint32_t {
      if (v == NONE)
        return NONE;
      if (v < nv && phi_idx[v] != NONE)
        return entry[k][phi_idx[v]];
      if (v < nv && fresh[k][v] != NONE)
        return fresh[k][v];
      return v;
    };
    // fill entry rows in k order (the induction gets its const vreg)
    for (uint32_t k = 0; k <= K; ++k) {
      // induction const first (it IS the entry value of the induction)
      entry[k][ind_idx] = fresh[k][ind];
      for (size_t pi = 0; pi < hphis.size(); ++pi) {
        if ((uint32_t)pi == ind_idx)
          continue;
        entry[k][pi] =
            (k == 0) ? hphis[pi].pre_arg : mapv(hphis[pi].latch_arg, k - 1);
      }
    }
    // emit copies (append-only: existing block ranges stay valid)
    auto remap_args_of = [&](Instr &c, uint32_t k) {
      for (uint32_t &av : c.args)
        av = mapv(av, k);
    };
    for (uint32_t k = 0; k <= K; ++k) {
      bool tail = (k == K);
      // header copy: [const i_k] + h's non-phi instrs + Br(body/X)
      {
        uint32_t Hk = bmap[k][h];
        fn.blocks[Hk].begin = (uint32_t)fn.code.size();
        fn.code.push_back({.k = K::Const, .dst = fresh[k][ind], .c = ivk[k]});
        for (uint32_t i = fn.blocks[h].begin; i + 1 < fn.blocks[h].end; ++i) {
          const Instr &I = fn.code[i];
          if (I.k == K::Phi)
            continue;
          Instr c = I;
          if (c.dst != NONE && c.dst < nv)
            c.dst = fresh[k][c.dst];
          c.a = mapv(c.a, k);
          c.b2 = mapv(c.b2, k);
          if (c.c2 != NONE)
            c.c2 = mapv(c.c2, k);
          remap_args_of(c, k);
          fn.code.push_back(c);
        }
        uint32_t tgt = tail ? X : bmap[k][body];
        fn.code.push_back({.k = K::Br, .t = tgt});
        fn.blocks[Hk].end = (uint32_t)fn.code.size();
        fn.succ[2 * Hk] = tgt;
        fn.succ[2 * Hk + 1] = NONE;
      }
      if (tail)
        continue;
      // body copies in original block order
      for (uint32_t b = 0; b < nb; ++b) {
        if (!inL[b] || b == h)
          continue;
        uint32_t Bk = bmap[k][b];
        fn.blocks[Bk].begin = (uint32_t)fn.code.size();
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
          const Instr &I = fn.code[i];
          if (i + 1 == fn.blocks[b].end) { // terminator
            if (I.k == K::Ret || I.k == K::Exit) {
              Instr c = I;
              c.a = mapv(c.a, k);
              fn.code.push_back(c);
            } else if (I.k == K::Br || I.k == K::Cbr) {
              Instr c = I;
              c.a = mapv(c.a, k);
              c.t = (I.t == h)
                        ? bmap[k + 1][h]
                        : ((I.t != NONE && bmap[k][I.t] != NONE) ? bmap[k][I.t]
                                                                 : I.t);
              if (I.k == K::Cbr)
                c.f = (I.f == h) ? bmap[k + 1][h]
                                 : ((I.f != NONE && bmap[k][I.f] != NONE)
                                        ? bmap[k][I.f]
                                        : I.f);
              fn.code.push_back(c);
            } else {
              fn.code.push_back(I); // unreachable-block junk: copy as-is
            }
            continue;
          }
          Instr c = I;
          if (I.k == K::Phi) {
            if (c.dst != NONE && c.dst < nv)
              c.dst = fresh[k][c.dst];
            c.a = mapv(c.a, k);
            c.b2 = mapv(c.b2, k);
            if (c.t != NONE && bmap[k][c.t] != NONE)
              c.t = bmap[k][c.t];
            if (c.f != NONE && bmap[k][c.f] != NONE)
              c.f = bmap[k][c.f];
          } else {
            if (c.dst != NONE && c.dst < nv)
              c.dst = fresh[k][c.dst];
            c.a = mapv(c.a, k);
            c.b2 = mapv(c.b2, k);
            if (c.c2 != NONE)
              c.c2 = mapv(c.c2, k);
            remap_args_of(c, k);
          }
          fn.code.push_back(c);
        }
        fn.blocks[Bk].end = (uint32_t)fn.code.size();
        fn.succ[2 * Bk] = (bmap[k][fn.succ0(b)] != NONE && inL[fn.succ0(b)])
                              ? bmap[k][fn.succ0(b)]
                              : fn.succ0(b);
        fn.succ[2 * Bk + 1] = (fn.succ1(b) != NONE && inL[fn.succ1(b)] &&
                               bmap[k][fn.succ1(b)] != NONE)
                                  ? bmap[k][fn.succ1(b)]
                                  : fn.succ1(b);
        // the back edge from this copy's latch goes to the NEXT copy
        if (b == latch)
          fn.succ[2 * Bk] = bmap[k + 1][h];
      }
    }
    // rewire the preheader edge h -> first copy
    for (uint32_t i = fn.blocks[pre].begin; i < fn.blocks[pre].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Br && I.t == h)
        I.t = bmap[0][h];
      else if (I.k == K::Cbr) {
        if (I.t == h)
          I.t = bmap[0][h];
        if (I.f == h)
          I.f = bmap[0][h];
      }
    }
    if (fn.succ[2 * pre] == h)
      fn.succ[2 * pre] = bmap[0][h];
    if (fn.succ[2 * pre + 1] == h)
      fn.succ[2 * pre + 1] = bmap[0][h];
    // exit-block phis: the h-edge now arrives from the tail
    for (uint32_t i = fn.blocks[X].begin; i < fn.blocks[X].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k != K::Phi)
        break;
      if (I.t == h) {
        I.t = bmap[K][h];
        I.a = mapv(I.a, K);
      } else if (I.f == h) {
        I.f = bmap[K][h];
        I.b2 = mapv(I.b2, K);
      }
    }
    // uses outside the loop -> tail values (h-defined only, per the scan)
    for (uint32_t b = 0; b < nb; ++b) {
      if (inL[b])
        continue;
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        Instr &I = fn.code[i];
        if (I.a != NONE && I.a < nv && defb[I.a] == h)
          I.a = mapv(I.a, K);
        if (I.b2 != NONE && I.b2 < nv && defb[I.b2] == h)
          I.b2 = mapv(I.b2, K);
        if (I.c2 != NONE && I.c2 < nv && defb[I.c2] == h)
          I.c2 = mapv(I.c2, K);
        for (uint32_t &av : I.args)
          if (av < nv && defb[av] == h)
            av = mapv(av, K);
      }
    }
    fn.recompute_preds();
    return true; // one unroll per call: the fixpoint driver re-analyzes
  }
  return false;
}

// --- 9d5. loop closed-form evaluation (constant trip counts) -----------------
