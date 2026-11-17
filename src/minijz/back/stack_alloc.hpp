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
//     destination -- the library kernel now (opt/escape.hpp's
//     calculate_copy_closure; the rescan fixpoint this file
//     hand-rolled until the lift).
//   * escape scan over the FINAL closure: the library kernel too
//     (scan_position_escapes); THIS file's whole job is the POSITION
//     POLICY -- which operand positions of which instr kinds may
//     legally see a handle: Copy sources and the HANDLE positions of
//     Load/Store/Len are safe; any other use -- an index, a stored
//     value, a phi, Bin/Cbr/Print/Call/Ret/Exit/Input operands --
//     makes the numeric handle observable and pins the array to the
//     arena.
//   * survivors become Fn::StkArr{root, len, vregs} records in
//     fn.stk_arrays (the array is named after its root vreg).
//
// DATA STRUCTURES: ndefs/kc/kv (nv-sized), the per-site operand and
// position-safety rows (materialized once for all candidates), ali
// (the alias bitmap, per candidate); StkArr itself lives in
// ir/instr.hpp.
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
#include "../opt/opt_generic.hpp"

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
  // ---- the kernel tables, materialized ONCE for every candidate -----------
  // copy_src/copy_dst: the identity-preserving moves. operands/safe: per
  // site, the read positions with the handle-position policy (1 = a member
  // may legally appear there). none_v operands ride along -- the kernel's
  // range checks skip them, exactly like the old bad() reader's.
  std::vector<ulen> copy_src{}, copy_dst{};
  std::vector<std::vector<ulen>> operands{};
  std::vector<std::vector<char>> safe{};
  operands.resize(fn.code.size());
  safe.resize(fn.code.size());
  for (const Instr &I : fn.code) {
    const uint32_t i = uint32_t(&I - fn.code.data());
    auto push = [&](uint32_t v, char ok) {
      operands[i].push_back((v == NONE) ? opt_ns::none_v : ulen(v));
      safe[i].push_back(ok);
    };
    switch (I.k) {
    case K::Copy: // source: alias extension (ok); dst cannot re-define
      if (I.a != NONE && I.a < nv && I.dst < nv) {
        copy_src.push_back(I.a);
        copy_dst.push_back(I.dst);
      }
      push(I.a, 1);
      break;
    case K::Load: // handle position ok; the INDEX is a value use
      push(I.a, 1);
      push(I.b2, 0);
      break;
    case K::Store: // handle ok; index and stored value are value uses
      push(I.a, 1);
      push(I.b2, 0);
      push(I.c2, 0);
      break;
    case K::Len: // handle position ok
      push(I.a, 1);
      break;
    default: // Bin/Cbr/Print/Call/Ret/Phi/Exit/Input/Alloc: every
      // operand observes the value (an Alloc's length included)
      push(I.a, 0);
      push(I.b2, 0);
      push(I.c2, 0);
      for (uint32_t v : I.args)
        push(v, 0);
      break;
    }
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
      if (I.dst == NONE || I.dst >= nv)
        continue; // defensive: no home to name
      // alias closure: the alloc dst + transitive Copy destinations
      const std::vector<ulen> seeds{ulen(I.dst)};
      const std::vector<char> ali =
          opt_ns::calculate_copy_closure(copy_src, copy_dst, seeds, nv);
      // escape scan over the FINAL alias set
      if (opt_ns::scan_position_escapes(operands, safe, ali))
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
