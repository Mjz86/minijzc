#pragma once
// === pred_norm.hpp ========================================
// multi-predecessor normalization (binary merge trees) shared by SSA
// construction and destruction.

#include "instr.hpp"

// ---- 6z. multi-predecessor normalization (binary merge trees)
// ---------------- A Phi in this IR carries exactly TWO arguments (keyed by the
// predecessor ids in t/f), so every merge point must have at most two
// predecessors. `continue` adds a second back edge to a loop header and `break`
// out of nested arms adds exit edges, so break/continue programs can produce 3+
// pred blocks; SSA construction would then place 2-arg phis with missing
// edges. This pass rewrites each >2-pred block's extra incoming edges
// through a right-leaning chain of fresh 2-pred merge blocks:
//     preds p0 p1 p2 .. p(k-1)  of B
//     ->  p0 -> B,  p1,p2 -> M1,  M1,p3 -> M2, .. M(k-3),p(k-1) -> M(k-2) -> B
// Each chain block is a pure `br`; standard phi placement at the IDF then
// lowers every value merge into a chain of 2-arg phis, and the loop
// analyses see a single (merge) latch again. Runs until fixpoint (each
// chain block has exactly 2 preds, so one round per offending block).
constexpr void normalize_multi_preds(Fn &fn) {
  for (bool again = true; again;) {
    again = false;
    fn.recompute_preds();
    const uint32_t nb = fn.nblocks();
    for (uint32_t b = 0; b < nb && !again; ++b) {
      if (fn.preds[b].size() <= 2)
        continue;
      const std::vector<uint32_t> ps = fn.preds[b]; // copy: succ mutates
      const uint32_t k = (uint32_t)ps.size();
      // fresh chain blocks M1..M(k-2) (M[j], 1-based)
      std::vector<uint32_t> M(k, NONE);
      for (uint32_t j = 1; j + 1 < k; ++j) {
        M[j] = fn.nblocks();
        fn.blocks.push_back({});
        fn.succ.push_back(NONE);
        fn.succ.push_back(NONE);
      }
      // their code (appended; existing ranges stay valid)
      for (uint32_t j = 1; j + 1 < k; ++j) {
        uint32_t tgt = (j + 2 == k) ? b : M[j + 1];
        fn.blocks[M[j]].begin = (uint32_t)fn.code.size();
        fn.code.push_back({.k = K::Br, .t = tgt});
        fn.blocks[M[j]].end = (uint32_t)fn.code.size();
        fn.succ[2 * M[j]] = tgt;
      }
      // redirect the extra predecessor edges: p1,p2 -> M1; pj -> M(j-1)
      auto redirect = [&](uint32_t p, uint32_t to) {
        for (int s = 0; s < 2; ++s)
          if (fn.succ[2 * p + (uint32_t)s] == b) {
            fn.succ[2 * p + (uint32_t)s] = to;
            for (uint32_t i = fn.blocks[p].begin; i < fn.blocks[p].end; ++i) {
              Instr &I = fn.code[i];
              if (I.k == K::Br && I.t == b)
                I.t = to;
              else if (I.k == K::Cbr) {
                if (I.t == b)
                  I.t = to;
                if (I.f == b)
                  I.f = to;
              }
            }
          }
      };
      redirect(ps[1], M[1]);
      if (k > 2)
        redirect(ps[2], M[1]);
      for (uint32_t j = 3; j < k; ++j)
        redirect(ps[j], M[j - 1]);
      again = true; // rescan with fresh preds
    }
  }
  fn.recompute_preds();
}
