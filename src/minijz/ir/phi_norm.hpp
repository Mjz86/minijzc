#pragma once
// === phi_norm.hpp ========================================
// the phis-first block invariant. Phi instrs are the first instrs of
// every block; consumers (passes, printers, backends) scan until the
// first non-phi. Passes that convert phis to Copy/Const in place break
// that ordering — this restores it by stably moving the remaining phis
// to the front. (Moved out of lcf.hpp when more passes needed it.)
//
// THEORY (why the invariant is load-bearing): every consumer in the
// tree identifies a block's phi run with the same two-line idiom
// (`if (I.k != K::Phi) break` / `skip while Phi`) -- SSA placement
// (an.hpp 8c), the loop passes' header-phi gathers, destruct's
// parallel-copy lowering, and the backends' phi-skipping emitters.
// A phi sitting mid-block would be silently INVISIBLE to all of
// them. The restore is STABLE (phis keep their relative order,
// non-phis likewise): order among phis carries no semantics, but
// stability keeps consecutive pipeline rounds from churning the
// code vector -- and keeps round-to-round IR diffs byte-stable.

#include "instr.hpp"

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
