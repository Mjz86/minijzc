#pragma once
// === loop_shape.hpp ========================================
// shared natural-loop recognition: the scan tables (const/def maps),
// the innermost-first header order, and the canonical loop shape that
// unroll / lcf / unswitch all require:
//   pre ----> h: [phis] instrs... cbr(cond) --body--> ... --> latch
//   h --exit--> X          (the header Cbr's exit arm leaves the loop)
// Extracted from the (previously triplicated) inline scans of unroll
// and lcf; the new passes (unswitch, lsimp) build on it too.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"

// ---- the scan tables (one pass over the fn) -------------------------
struct LoopScan {
  uint32_t nb{}, nv{};
  std::vector<char> kc{};               // vreg's def is a Const
  std::vector<val_t> kv{};              // ... and its value
  std::vector<uint32_t> defi{}, defb{}; // defining instr / block
  std::vector<uint32_t> headers{};      // loop headers, innermost first
};

constexpr LoopScan loop_scan(const Fn &fn, const An &A) {
  LoopScan S{};
  S.nb = fn.nblocks();
  S.nv = fn.nvars + (uint32_t)fn.version_base.size();
  S.kc.assign(S.nv, 0);
  S.kv.assign(S.nv, 0);
  S.defi.assign(S.nv, NONE);
  S.defb.assign(S.nv, NONE);
  for (uint32_t b = 0; b < S.nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.dst != NONE && I.dst < S.nv) {
        S.defi[I.dst] = i;
        S.defb[I.dst] = b;
        if (I.k == K::Const) {
          S.kc[I.dst] = 1;
          S.kv[I.dst] = I.c;
        }
      }
    }
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h)
      S.headers.push_back(h);
  for (size_t i = 0; i + 1 < S.headers.size(); ++i)
    for (size_t j = i + 1; j < S.headers.size(); ++j)
      if (A.nest[S.headers[j]] > A.nest[S.headers[i]])
        std::swap(S.headers[i], S.headers[j]);
  return S;
}

// inL flags for one natural loop (header + body), sized for all blocks
constexpr void in_loop_flags(const Fn &fn, const An &A, uint32_t h,
                             std::vector<char> &inL) {
  inL.assign(fn.nblocks(), 0);
  for (ulen x : A.loops.range()[h])
    inL[(uint32_t)x] = 1;
  inL[h] = 1;
}

// one header phi, keyed to (pre, latch)
struct HPhi {
  uint32_t dst, pre_arg, latch_arg;
};

// all of h's phis keyed to (pre, latch). ok = every phi carries BOTH
// keys (the block has exactly the two preds); callers bail otherwise.
constexpr bool header_phis(const Fn &fn, uint32_t h, uint32_t pre,
                           uint32_t latch, std::vector<HPhi> &out) {
  out.clear();
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
    out.push_back({I.dst, pa, la});
  }
  return nphi && out.size() == nphi;
}

// the canonical loop shape around header h (see the file header).
// ok = recognized: unique outside pred (the preheader), unique latch,
// Cbr header terminator with exactly one arm leaving the loop to X and
// one arm staying inside (the body). The only-exit-edge property (no
// breaks) is NOT required here — callers that need it (unroll, lcf)
// check it themselves; unswitch allows internal exit edges.
struct LoopShape {
  uint32_t pre{NONE}, latch{NONE}, X{NONE}, body{NONE};
  bool ok{};
};

constexpr LoopShape loop_shape_of(const Fn &fn, const An &A,
                                  const std::vector<char> &inL, uint32_t h) {
  (void)A;
  LoopShape sh{};
  uint32_t outside = 0, inside = 0;
  for (uint32_t p : fn.preds[h]) {
    if (inL[p]) {
      sh.latch = p;
      ++inside;
    } else {
      sh.pre = p;
      ++outside;
    }
  }
  if (inside != 1 || outside != 1 || sh.pre == NONE ||
      fn.blocks[h].begin == fn.blocks[h].end)
    return sh;
  const Instr hterm = fn.code[fn.blocks[h].end - 1];
  if (hterm.k != K::Cbr)
    return sh;
  // arms: one stays in the loop (body), one leaves (X)
  if (hterm.t != NONE && inL[hterm.t] && hterm.f != NONE && !inL[hterm.f]) {
    sh.body = hterm.t;
    sh.X = hterm.f;
  } else if (hterm.f != NONE && inL[hterm.f] && hterm.t != NONE &&
             !inL[hterm.t]) {
    sh.body = hterm.f;
    sh.X = hterm.t;
  } else
    return sh;
  sh.ok = true;
  return sh;
}
