#pragma once
// === lsimp.hpp ========================================
// loop canonicalization (README wishlist: "loop simplification /
// canonicalization proper"): two local normalizations that shrink loop
// headers to the exact shape the restructuring passes want.
//
// 1. trivial header-phi elimination: a header phi whose LATCH argument
//    is the phi itself (x' = x: the loop body stopped changing x —
//    typically after dce/copyprop removed the update) carries the
//    preheader value through every iteration. Every use (inside and
//    outside the loop) becomes the pre_arg and the phi dies. This is
//    the loop-carried analogue of copyprop, and it must run as a loop
//    pass: tier-0 copyprop cannot see through the back edge.
//
// 2. degenerate latch collapse: a latch block that only contains phis
//    and the unconditional back-edge `br` merges into the header when
//    the header is its only other... (not done here: splice already
//    collapses single-pred/single-succ pairs, which covers the common
//    shapes; what remains is rare and not worth the CFG surgery).
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/phi_norm.hpp"
#include "../analysis/an.hpp"
#include "loop_shape.hpp"

constexpr bool opt_lsimp(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks() || !fn.nblocks())
    return false;
  const uint32_t nb = fn.nblocks();
  bool changed_any = false;
  for (uint32_t h = 0; h < A.n; ++h) {
    if (A.inner[h] != h)
      continue; // headers only
    auto row = A.loops.range()[h];
    if (row.empty())
      continue;
    std::vector<char> inL(nb, 0);
    for (ulen x : row)
      inL[(uint32_t)x] = 1;
    inL[h] = 1;
    // unique outside pred + unique latch (same shape the others want)
    uint32_t pre = NONE, latch = NONE, outside = 0, inside = 0;
    for (uint32_t p : fn.preds[h]) {
      if (inL[p]) {
        latch = p;
        ++inside;
      } else {
        pre = p;
        ++outside;
      }
    }
    if (inside != 1 || outside != 1 || pre == NONE || latch == NONE)
      continue;
    // trivial phis: latch arg == dst (the value never changes)
    for (uint32_t i = fn.blocks[h].begin; i < fn.blocks[h].end; ++i) {
      Instr &P = fn.code[i];
      if (P.k != K::Phi)
        break; // phis are block-first: done with them
      uint32_t la = (P.t == latch) ? P.a : ((P.f == latch) ? P.b2 : NONE);
      uint32_t pa = (P.t == pre) ? P.a : ((P.f == pre) ? P.b2 : NONE);
      if (la != P.dst || pa == NONE)
        continue;
      // every use of P.dst -> pa (P.dst is dead everywhere it matters:
      // its only "def" is the phi itself)
      for (uint32_t b = 0; b < nb; ++b) {
        for (uint32_t j = fn.blocks[b].begin; j < fn.blocks[b].end; ++j) {
          Instr &I = fn.code[j];
          auto rw = [&](uint32_t &v) {
            if (v == P.dst)
              v = pa;
          };
          if (I.k == K::Phi) {
            // args only (the keys are pred ids, not vregs)
            rw(I.a);
            rw(I.b2);
          } else {
            rw(I.a);
            rw(I.b2);
            rw(I.c2);
            for (uint32_t &av : I.args)
              rw(av);
          }
        }
      }
      P.k = K::Copy; // phi -> copy(pre_arg); tier-0 copyprop + dce
      P.a = pa;      // dissolve the rest
      P.b2 = NONE;
      P.t = NONE;
      P.f = NONE;
      os.lsimps++;
      changed_any = true;
      normalize_phis(fn); // restore the phis-first invariant
      break;              // one phi per call: rescan (indexes moved)
    }
    if (changed_any)
      break; // one rewrite per call: the driver re-analyzes
  }
  return changed_any;
}
