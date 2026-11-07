#pragma once
// === alias.hpp ========================================
// handle aliasing: which vreg provably names exactly one allocation.

#include "minijz/ir/instr.hpp"

// ============================================================================
// 9d3. HANDLE ALIAS ANALYSIS (allocation-identity + value ranges)
// ============================================================================
// The arena is one flat int64 array; a handle is the index of an
// allocation's first slot. alias_analyze computes, for every vreg, whether
// it provably holds the handle of EXACTLY ONE allocation site:
//    single[v] = id   for the Alloc dst, Copies of it, and Phis whose live
//                     args are all that same id
//    single[v] = NONE otherwise ("obscure": constants, arithmetic results,
//                     loads, params, input(), call results, mixed phis --
//                     the value may name ANY array, or no array at all).
// Two program-wide gates are derived from this (see prog_handle_gates):
//   lift_ok   -- no Load/Store anywhere reads through an obscure handle.
//                Otherwise a runtime value could observe the stale arena
//                contents of a promoted (mem2reg'd) array, so promotion is
//                disabled globally. (Promotion KEEPS the allocation in the
//                arena, so handle VALUES and lengths never change.)
//   stkarr_ok -- additionally no handle value is observable outside handle
//                position and no Len reads an obscure handle. Moving an
//                array OUT of the arena (the C backend's stack arrays)
//                shifts every LATER handle value; that must be provably
//                unobservable. [This also fixes a soundness bug: printing
//                the second of two arrays used to print a shifted handle
//                in the C backend when the first was stack-allocated.]
struct AliasInfo {
  std::vector<uint32_t> single{}; // vreg -> alloc id, NONE = obscure
  uint32_t nallocs{};
};

constexpr AliasInfo alias_analyze(const Fn &fn) {
  AliasInfo AI{};
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  AI.single.assign(nv, NONE);
  if (!nv) return AI;
  for (const Instr &I : fn.code)
    if (I.k == K::Alloc && I.dst != NONE && I.dst < nv)
      AI.single[I.dst] = AI.nallocs++;
  // Copies and Phis reference each other (loop phis), so iterate to a
  // fixpoint. Each vreg has ONE def, so single[v] is a pure function of its
  // def instr; the loop just resolves the reference chains. Transitions are
  // monotone NONE->id (an id, once assigned, is stable), so this terminates.
  bool grew = true;
  for (uint32_t guard = 0; grew && guard < 64; ++guard) {
    grew = false;
    for (const Instr &I : fn.code) {
      if (I.dst == NONE || I.dst >= nv) continue;
      if (I.k == K::Copy) {
        if (I.a != NONE && I.a < nv &&
            AI.single[I.dst] != AI.single[I.a]) {
          AI.single[I.dst] = AI.single[I.a];
          grew = true;
        }
      } else if (I.k == K::Phi) {
        // single iff BOTH args exist, are that same single, and are not
        // obscure; an undef (NONE) arg is treated as obscure (conservative)
        bool ok = I.a != NONE && I.a < nv && AI.single[I.a] != NONE &&
                  I.b2 != NONE && I.b2 < nv &&
                  AI.single[I.b2] == AI.single[I.a];
        uint32_t want = ok ? AI.single[I.a] : NONE;
        if (AI.single[I.dst] != want) {
          AI.single[I.dst] = want;
          grew = true;
        }
      }
    }
  }
  return AI;
}

// Program-wide gate computation over every function (pre-optimization):
// the optimizer only ever removes or duplicates instructions, and copies
// propagate single-ness, so a gate that holds now holds for the whole run.
constexpr void prog_handle_gates(const std::vector<Fn> &fns, bool &lift_ok,
                                 bool &stkarr_ok) {
  lift_ok = true;
  stkarr_ok = true;
  for (const Fn &fn : fns) {
    if (!fn.nblocks()) continue;
    AliasInfo AI = alias_analyze(fn);
    const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
    auto sgl = [&](uint32_t v) -> bool {
      return v != NONE && v < nv && AI.single[v] != NONE;
    };
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Load || I.k == K::Store) {
          if (!sgl(I.a)) lift_ok = false; // could hit ANY array's contents
        } else if (I.k == K::Len) {
          if (!sgl(I.a)) stkarr_ok = false; // length table would shift
        }
        if (!stkarr_ok) continue;
        // observable uses of a handle VALUE (numbers that shift when an
        // earlier allocation leaves the arena)
        switch (I.k) {
        case K::Bin:   // arithmetic/comparison on a handle
        case K::Cbr:   // branching on a handle value
        case K::Print: // printing a handle
        case K::Ret:   // returning a handle to the caller
        case K::Alloc: // a handle as an allocation size
          if (sgl(I.a) || sgl(I.b2)) stkarr_ok = false;
          break;
        case K::Load:  // handle used as an INDEX
          if (sgl(I.b2)) stkarr_ok = false;
          break;
        case K::Store: // handle used as an index or stored as a value
          if (sgl(I.b2) || sgl(I.c2)) stkarr_ok = false;
          break;
        case K::Call:  // handle passed to a function
          for (uint32_t v : I.args)
            if (sgl(v)) stkarr_ok = false;
          break;
        default: break; // Copy/Phi: plumbing; Len handle position: fine
        }
      }
  }
}

// The array arena: identical bump-allocator semantics in EVERY backend
// (interpreters, bytecode VM, emitted C, emitted x86-64). A handle is the
// index of the array's first element; allocation is monotonic (no free), so
// handle values, aliasing, and allocation order are bit-identical everywhere.
// CAP must match MJZ_ARENA_CAP emitted into the C/asm backends.
inline constexpr val_t MJZ_ARENA_CAP = 2097152; // 2^21 int64 slots = 16 MiB

// ============================================================================
