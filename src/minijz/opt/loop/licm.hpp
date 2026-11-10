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
    // def block/instr of every vreg (recomputed each hoist round)
    for (int round = 0; round < 10; ++round) {
      const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
      std::vector<uint32_t> defb(nv, NONE), defi(nv, NONE);
      std::vector<char> kc(nv, 0);
      std::vector<val_t> kv(nv, 0);
      for (uint32_t b = 0; b < fn.nblocks(); ++b)
        for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
          if (fn.code[i].dst != NONE) {
            defb[fn.code[i].dst] = b;
            defi[fn.code[i].dst] = i;
            if (fn.code[i].k == K::Const) {
              kc[fn.code[i].dst] = 1;
              kv[fn.code[i].dst] = fn.code[i].c;
            }
          }
      auto invariant = [&](uint32_t v) -> bool {
        if (v == NONE)
          return true;
        if (v >= nv)
          return false;
        uint32_t d = defb[v];
        if (d == NONE)
          return false;
        if (d != NONE && fn.code[defi[v]].k == K::Const)
          return true; // a Const's VALUE is position-independent; the
                       // hoist duplicates it into the preheader below
        if (inL[d])
          return false;             // defined inside: not invariant
        return A.dominates(d, pre); // reachable into preheader
      };
      bool changed = false;
      // scan order: the header FIRST (its phis and terminator are skipped
      // by the checks below; what remains can be invariant — the while-
      // lowering computes the loop condition IN the header, so `while
      // (f > 0)` with f never written inside leaves a pure invariant test
      // there). Hoisting it to the preheader is the classic unswitch
      // enabler AND saves one instr per iteration. Then the body blocks.
      for (uint32_t bi = 0; bi <= (uint32_t)row.size(); ++bi) {
        uint32_t B = (bi == 0) ? h : (uint32_t)row[bi - 1];
        for (uint32_t i = fn.blocks[B].begin; i < fn.blocks[B].end; ++i) {
          Instr &I = fn.code[i];
          if (I.dst == NONE)
            continue; // control/print: no
          if (I.k == K::Phi || I.k == K::Cbr || I.k == K::Br ||
              I.k == K::Print || I.k == K::Exit)
            continue;
          if (I.k == K::Bin && (I.b == B::Div || I.b == B::Mod)) {
            // nt proofs are CONTROL-FLOW-SENSITIVE: `z != 0 && 42 / z`
            // proves the division safe only under the guard. Hoisting
            // the div out of the guarded region breaks the proof's
            // premise and can manufacture a trap. The one context-free
            // safety is a CONSTANT divisor outside {0, -1}: no dividend
            // can trap then (INT64_MIN / -1 is the other trap source).
            const uint32_t dv = I.b2;
            const bool safe = dv != NONE && dv < nv && kc[dv] && kv[dv] != 0 &&
                              kv[dv] != (val_t)-1;
            if (!safe)
              continue;
          }
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
          // operands that are Consts defined INSIDE the loop (the while-
          // lowering materializes loop-test constants in the header) are
          // duplicated into the preheader: the value is the same, the
          // location is not (the original would not dominate the hoist)
          std::vector<Instr> extra{};
          auto dup_const = [&](uint32_t &v) {
            if (v == NONE || v >= nv)
              return;
            if (defb[v] != NONE && inL[defb[v]] &&
                fn.code[defi[v]].k == K::Const) {
              Instr dc = fn.code[defi[v]];
              dc.dst = new_anon_vreg(fn, fn.base_of(v));
              v = dc.dst;
              extra.push_back(dc);
            }
          };
          dup_const(copy.a);
          dup_const(copy.b2);
          fn.code.erase(fn.code.begin() + (long)i); // remove from B
          // insertion index in the post-erase vector:
          uint32_t pe = (pe_old > i) ? pe_old - 1 : pe_old;
          fn.code.insert(fn.code.begin() + (long)pe, copy);
          for (size_t e = extra.size(); e-- > 0;)
            fn.code.insert(fn.code.begin() + (long)pe, extra[e]);
          const uint32_t nadd = (uint32_t)extra.size() + 1;
          // range fixup 1: everything after the erased index shifts down
          for (uint32_t x = 0; x < fn.nblocks(); ++x) {
            if (fn.blocks[x].begin > i)
              fn.blocks[x].begin--;
            if (fn.blocks[x].end > i)
              fn.blocks[x].end--;
          }
          // range fixup 2: the hoisted instr (+ duplicated consts) now
          // live inside `pre` (before its terminator); blocks after them
          // shift up
          for (uint32_t x = 0; x < fn.nblocks(); ++x) {
            if (x == pre) {
              fn.blocks[x].end += nadd;
              continue;
            }
            if (fn.blocks[x].begin >= pe + 1)
              fn.blocks[x].begin += nadd;
            if (fn.blocks[x].end >= pe + 1)
              fn.blocks[x].end += nadd;
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
