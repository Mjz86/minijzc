#pragma once
// === stack_alloc.hpp ========================================
// stack-allocation analysis for the C backend (non-escaping constant
// arrays become C locals).
//
// ALGORITHM (analyze_stack_allocs -- one Fn, run on the SSA form):
//   * kc/kv: the single-def constant table (a vreg is a known const
//     only with exactly ONE def, and that def a Const).
//   * candidate Allocs: constant length, 1 <= len <=
//     MJZ_STK_ARR_MAX_ELEMS, within the MJZ_STK_ARR_TOTAL_ELEMS budget.
//   * alias closure ali: the alloc's dst plus every transitive Copy
//     destination -- a fixpoint rescan of fn.code until stable.
//   * escape scan over the FINAL closure: Copy sources and the HANDLE
//     positions of Load/Store/Len are safe; any other use -- an
//     index, a stored value, a phi, Bin/Cbr/Print/Call/Ret/Exit/Input
//     operands -- makes the numeric handle observable and pins the
//     array to the arena.
//   * survivors become Fn::StkArr{root, len, vregs} records in
//     fn.stk_arrays (the array is named after its root vreg).
//
// DATA STRUCTURES: ndefs/kc/kv (nv-sized), ali (the alias bitmap),
// total (the element budget); StkArr itself lives in ir/instr.hpp.
//
// THEORY: a provably non-escaping array's handle value is unobservable,
// so the C backend may keep it in a zero-filled stack local (memset at
// the allocation point, exactly like the arena) whose indices match
// the arena's -- bit-identical behavior with no arena traffic. Any
// observable use (a phi included: an older iteration's array could
// otherwise be observed after re-allocation) forces the arena home so
// every runner agrees; the budgets keep frames well under 8 MiB.
//
// CONSUMERS: compile_impl.hpp stashes the analysis on the SSA copy
// before destruction (vreg ids survive); emit_c_fn's arr_of map
// lowers the array's Alloc/Load/Store/Len to mjz_s<root>[] accesses.

#include "../ir/instr.hpp"

// --- 9f2. stack-allocation analysis (for the C backend) ---------------------
// An `array(n)` with constant n whose handle only ever flows (through Copies)
// to Load/Store/Len as the HANDLE operand — never into a phi (an older
// iteration's array could otherwise be observed after the same instruction
// re-allocated its slot), never into arithmetic/comparisons/print/return/call
// arguments, never stored as a value (array-of-arrays) — is provably local:
// its handle value is unobservable, so the C backend can keep it in a stack
// array (zero-filled at the allocation point, exactly like the arena).
// Handles ARE ordinary values: any other use makes the numeric value
// observable and pins the array to the arena (indices must match every
// other runner). Budget: per array <= 16384 elements and <= 1 MiB total,
// to keep native frames far below the default 8 MiB stack.
inline constexpr val_t MJZ_STK_ARR_MAX_ELEMS = 16384;
inline constexpr val_t MJZ_STK_ARR_TOTAL_ELEMS = 131072;
constexpr void analyze_stack_allocs(Fn &fn) {
  fn.stk_arrays.clear();
  const uint32_t nb = fn.nblocks();
  if (!nb)
    return;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv)
    return;
  std::vector<uint32_t> ndefs(nv, 0);
  for (const Instr &I : fn.code)
    if (I.dst != NONE && I.dst < nv)
      ++ndefs[I.dst];
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv && ndefs[I.dst] == 1) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  val_t total = 0;
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k != K::Alloc)
        continue;
      if (I.a == NONE || I.a >= nv || !kc[I.a])
        continue; // runtime length
      val_t len = kv[I.a];
      if (len < 0 || len == 0 || len > MJZ_STK_ARR_MAX_ELEMS)
        continue;
      if (total + len > MJZ_STK_ARR_TOTAL_ELEMS)
        continue;
      // alias closure: the alloc dst + transitive Copy destinations
      std::vector<char> ali(nv, 0);
      ali[I.dst] = 1;
      for (bool grew = true; grew;) {
        grew = false;
        for (const Instr &C : fn.code)
          if (C.k == K::Copy && C.a != NONE && C.a < nv && C.dst < nv &&
              ali[C.a] && !ali[C.dst]) {
            ali[C.dst] = 1;
            grew = true;
          }
      }
      // escape scan over the FINAL alias set
      bool esc = false;
      for (uint32_t b2 = 0; b2 < nb && !esc; ++b2)
        for (uint32_t j = fn.blocks[b2].begin; j < fn.blocks[b2].end && !esc;
             ++j) {
          const Instr &U = fn.code[j];
          auto bad = [&](uint32_t v) { return v != NONE && v < nv && ali[v]; };
          switch (U.k) {
          case K::Copy: // source: alias extension (ok); dst cannot re-define
            break;
          case K::Load: // handle position ok; the INDEX is a value use
            if (bad(U.b2))
              esc = true;
            break;
          case K::Store: // handle ok; index and stored value are value uses
            if (bad(U.b2) || bad(U.c2))
              esc = true;
            break;
          case K::Len: // handle position ok
            break;
          default: // Bin/Cbr/Print/Call/Ret/Phi/Exit/Input: any alias use
            if (bad(U.a) || bad(U.b2) || bad(U.c2))
              esc = true;
            for (uint32_t v : U.args)
              if (bad(v))
                esc = true;
            break;
          }
        }
      if (esc)
        continue;
      total += len;
      Fn::StkArr sa{};
      sa.root = I.dst;
      sa.len = len;
      for (uint32_t v = 0; v < nv; ++v)
        if (ali[v])
          sa.vregs.push_back(v);
      fn.stk_arrays.push_back(std::move(sa));
    }
}
