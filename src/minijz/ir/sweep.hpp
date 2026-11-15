#pragma once
// === sweep.hpp ========================================
// the shared survivor sweep: compact fn.code keeping the instrs whose
// keep byte is set, rewriting every block range. The identical
// twelve-line tail of dce/cse/gvn/copyprop/post_destruct_peephole,
// factored once (the house dedup precedent, and the same KEEP-mask
// convention as ssa_ns::calculate_reflow_plan); returns the number of
// dropped instrs so each caller feeds its own stat counter.
#include "instr.hpp"

constexpr uint32_t sweep_survivors(Fn &fn, const std::vector<char> &keep) {
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size());
  uint32_t dropped = 0;
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    const uint32_t ob = fn.blocks[b].begin, oe = fn.blocks[b].end;
    fn.blocks[b].begin = (uint32_t)nc.size();
    for (uint32_t i = ob; i < oe; ++i) {
      if (keep[i]) {
        nc.push_back(fn.code[i]);
        continue;
      }
      ++dropped;
    }
    fn.blocks[b].end = (uint32_t)nc.size();
  }
  fn.code = std::move(nc);
  return dropped;
}
