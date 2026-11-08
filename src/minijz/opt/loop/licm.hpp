#pragma once
// === licm.hpp ========================================
// loop-invariant code motion.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"

// --- 9d. loop-invariant code motion
// ------------------------------------------- Uses the lib's natural loops +
// dominance intervals:
//   - loop bodies from calculate_natural_loops_with_header_...
//   - ordering by nesting depth from calculate_nesting_of_natural_loop_headers
//   - "def dominates preheader" via the dominance intervals
// Hoist pure non-trapping instrs whose operands are all defined outside the
// loop (and dominate the preheader) into the preheader. Only loops with a
// unique outside predecessor are eligible (preheader must dominate header).
constexpr bool opt_licm(Fn &fn, const An &A, OptStats &os) {
  if (A.n != fn.nblocks())
    return false;
  bool changed_any = false;
  // headers sorted innermost-first (nest count descending)
  std::vector<uint32_t> headers;
  for (uint32_t h = 0; h < A.n; ++h)
    if (A.inner[h] == h)
      headers.push_back(h);
  for (size_t i = 0; i + 1 < headers.size(); ++i) // tiny sorts are fine here
    for (size_t j = i + 1; j < headers.size(); ++j)
      if (A.nest[headers[j]] > A.nest[headers[i]])
        std::swap(headers[i], headers[j]);
  for (uint32_t h : headers) {
    auto row = A.loops.range()[h];
    if (row.empty())
      continue;
    bool inL_flag_dummy = false;
    (void)inL_flag_dummy;
    std::vector<char> inL(A.n, 0);
    for (ulen x : row)
      inL[(uint32_t)x] = 1;
    inL[h] = 1;
    // unique outside pred?
    uint32_t pre = NONE;
    uint32_t outside = 0;
    for (uint32_t p : fn.preds[h]) {
      if (!inL[p]) {
        pre = p;
        ++outside;
      }
    }
    if (outside != 1 || pre == NONE)
      continue;
    // def block of every vreg (recomputed each hoist round)
    for (int round = 0; round < 10; ++round) {
      const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
      std::vector<uint32_t> defb(nv, NONE);
      for (uint32_t b = 0; b < fn.nblocks(); ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst != NONE)
            defb[fn.code[i].dst] = b;
      auto invariant = [&](uint32_t v) -> bool {
        if (v == NONE)
          return true;
        if (v >= nv)
          return false;
        uint32_t d = defb[v];
        if (d == NONE)
          return false;
        if (inL[d])
          return false;             // defined inside: not invariant
        return A.dominates(d, pre); // reachable into preheader
      };
      bool changed = false;
      for (ulen xb : row) {
        uint32_t B = (uint32_t)xb;
        if (B == h)
          continue; // header holds the phi+cond
        for (uint32_t i = fn.blocks[B].begin; i < fn.blocks[B].end; ++i) {
          Instr &I = fn.code[i];
          if (I.dst == NONE)
            continue; // control/print: no
          if (I.k == K::Phi || I.k == K::Cbr || I.k == K::Br ||
              I.k == K::Print || I.k == K::Exit)
            continue;
          if (I.k == K::Bin && (I.b == B::Div || I.b == B::Mod) && !I.nt)
            continue; // may trap: no hoist (nt: proven safe, hoistable)
          if (I.k == K::Const)
            continue; // nothing to gain
          // arena/stdin ops are NOT loop-invariant: Alloc returns a fresh
          // handle per execution, Load/Store touch mutable memory, Len can
          // change when an Alloc lands in between, Input consumes the tape.
          if (I.k == K::Alloc || I.k == K::Load || I.k == K::Store ||
              I.k == K::Len || I.k == K::Input)
            continue;
          if (I.k == K::Call)
            continue; // callee may print/trap/read stdin: never invariant
          if (!invariant(I.a) || !invariant(I.b2))
            continue;
          // hoist: move to the preheader, right before its terminator.
          // The terminator is always the last instr, so this is guaranteed
          // to be after any phis AND after every operand def that lives in
          // the preheader itself.
          uint32_t pe_old = fn.blocks[pre].end - 1; // terminator, pre-erase
          Instr copy = I;
          fn.code.erase(fn.code.begin() + (long)i); // remove from B
          // insertion index in the post-erase vector:
          uint32_t pe = (pe_old > i) ? pe_old - 1 : pe_old;
          fn.code.insert(fn.code.begin() + (long)pe, copy);
          // range fixup 1: everything after the erased index shifts down
          for (uint32_t x = 0; x < fn.nblocks(); ++x) {
            if (fn.blocks[x].begin > i)
              fn.blocks[x].begin--;
            if (fn.blocks[x].end > i)
              fn.blocks[x].end--;
          }
          // range fixup 2: the hoisted instr now lives inside `pre` (before
          // its terminator); blocks after it shift up
          for (uint32_t x = 0; x < fn.nblocks(); ++x) {
            if (x == pre) {
              fn.blocks[x].end++;
              continue;
            }
            if (fn.blocks[x].begin >= pe + 1)
              fn.blocks[x].begin++;
            if (fn.blocks[x].end >= pe + 1)
              fn.blocks[x].end++;
          }
          os.hoisted++;
          changed = true;
          break; // rescan this block
        }
        if (changed)
          break;
      }
      if (!changed)
        break;
      changed_any = true;
    }
  }
  return changed_any;
}
