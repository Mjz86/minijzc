#pragma once
// === licm.hpp ========================================
// loop-invariant code motion.
//
// THEORY (when hoisting one iteration's work to the preheader is
// sound):
//   Hoisting moves an instruction OUT of the loop; it then executes
//   ONCE, before the loop, even if the loop body would have run it
//   many times -- or ZERO times. That is observable unless the
//   instruction is PURE, so purity is requirement (1): no traps,
//   no memory, no stdin, no output. Hence Alloc (fresh handle per
//   execution), Load/Store (mutable memory), Len (can change when
//   an Alloc lands in between), Input (consumes the tape) and Call
//   (may print/trap/read) are never invariant; Div/Mod need the
//   CONTEXT-FREE safety (constant divisor outside {0,-1}) because
//   nt proofs are control-flow-sensitive -- see the inline note at
//   the Bin case.
//   (2) OPERANDS: each operand must be loop-invariant -- defined
//   outside the loop with its def DOMINATING the preheader (the
//   value is then available at the hoist point on every path), or
//   a Const (position-independent; if the Const is defined INSIDE
//   the loop it is duplicated into the preheader, since the
//   original would not dominate the hoist).
//   (3) PREHEADER: only loops with a unique outside pred qualify;
//   the hoist lands right before the preheader's terminator --
//   after its phis and after every operand def living in pre.
//   SCAN ORDER: the header FIRST (the while lowering computes the
//   loop condition in the header; an invariant header test is the
//   classic unswitch enabler), then the body in member-row order.
//   Rounds: up to 10 fixpoint rounds per loop, each recomputing
//   the def tables (a hoist removes a def from the loop, which can
//   make more operands invariant). The surgery is mid-vector (erase
//   at i, insert at pe, two range-fixup sweeps), unlike the
//   append-only emitters elsewhere in the tree.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "../opt_generic.hpp"

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
    // unique outside pred? (the generic rule)
    const auto [pre_k, outside_k] =
        opt_ns::find_unique_outside_pred(fn.preds[h], inL);
    const uint32_t pre = (uint32_t)pre_k;
    if (outside_k != 1 || pre == NONE)
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
      // ---- the eligibility mask (the POLICY half): pure instrs with a
      // def, not control/print/phis, nt-independent div safety (a CONSTANT
      // divisor outside {0, -1} is the one context-free safety -- nt
      // proofs are control-flow-sensitive), no arena/stdin/call. The
      // invariance predicate and the scan discipline (header FIRST --
      // an invariant header test is the classic unswitch enabler -- then
      // the member row) are the kernel's (opt_ns::
      // calculate_first_invariant_site); it returns ONE site, the pass
      // hoists it and recomputes its tables (a hoist can make more
      // operands invariant).
      auto U = [](uint32_t v) {
        return v == NONE ? opt_ns::none_v : (uintlen_t)v;
      };
      const uint32_t n = (uint32_t)fn.code.size();
      std::vector<char> eligible(n, 0);
      std::vector<std::vector<uintlen_t>> sargs(n);
      std::vector<std::vector<uintlen_t>> sites_of_block(fn.nblocks());
      for (uint32_t bb = 0; bb < fn.nblocks(); ++bb)
        for (uint32_t i = fn.blocks[bb].begin; i < fn.blocks[bb].end; ++i) {
          sites_of_block[bb].push_back(i);
          const Instr &I = fn.code[i];
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
          eligible[i] = 1;
          sargs[i] = {U(I.a), U(I.b2)};
        }
      std::vector<uintlen_t> defb_k(nv, opt_ns::none_v);
      for (uint32_t v = 0; v < nv; ++v)
        if (defb[v] != NONE)
          defb_k[v] = defb[v];
      const auto pick = opt_ns::calculate_first_invariant_site<V>(
          row, h, pre, inL, defb_k, kc, A.ib, A.ie, sites_of_block, sargs,
          eligible);
      bool changed = false;
      if (pick.first != opt_ns::none_v) {
        const uint32_t B = (uint32_t)pick.first;
        const uint32_t i = (uint32_t)pick.second;
        Instr &I = fn.code[i];
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
      }
      if (!changed)
        break;
      changed_any = true;
    }
  }
  return changed_any;
}
