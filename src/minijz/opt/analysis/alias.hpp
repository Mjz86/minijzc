#pragma once
// === alias.hpp ========================================
// handle aliasing: which vreg provably names exactly one allocation.
//
// ALGORITHM: alias_analyze seeds single[Alloc.dst] with a fresh id
// (nallocs++), then runs a fixpoint loop (guard < 64) propagating
// Copy (single[dst] = single[a]) and Phi (single iff both args are
// in range, non-obscure and the SAME id; a NONE arg is obscure).
// prog_handle_gates then scans every fn before optimization: any
// Load/Store through an obscure handle clears lift_ok; any Len on
// an obscure handle, or any observable use of a handle VALUE (Bin,
// Cbr, Print, Ret, Alloc size, Load/Store index or stored value,
// Call args), clears stkarr_ok.
//
// DATA STRUCTURES: AliasInfo::single (vreg -> alloc id, NONE =
// obscure) and nallocs; lift_ok / stkarr_ok are the output gates.
//
// THEORY: the arena is one flat int64 array and a handle is the
// index of an allocation's first slot, so aliasing is an identity
// question. single[v] = id PROVES v names exactly that one site
// (the Alloc itself, its copies, phis whose live args all carry
// it); everything else stays "obscure" -- constants, arithmetic
// results, loads, params, input(), call results, mixed phis: a
// value that may name ANY array, or none. One def per vreg makes
// single[v] a pure function of its def; transitions are monotone
// NONE -> id, so the fixpoint terminates; and the optimizer only
// removes or duplicates instructions (copies propagate
// single-ness), so gates computed before it hold for the run.
//
// DOWNSTREAM: lift_ok gates mem2reg's array promotion (an obscure
// Load/Store could read a promoted array's stale arena contents);
// stkarr_ok lets the C backend demote arrays to stack slots, and
// trapelim reruns alias_analyze for its Load/Store bounds proofs.

#include "../../ir/instr.hpp"
#include "../opt_generic.hpp"

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
// The copy/phi fixpoint is the GENERIC KERNEL (opt_ns::alias_ns::
// resolve_single_ids, opt/opt_generic.hpp): this wrapper seeds the
// allocations, states each def in code order (Copy -> inherit, Phi ->
// ALL-args-agree, N-ary), and passes the compiler's historical patience
// cap (64 rounds) as policy. Two program-wide gates are derived from this
// (see prog_handle_gates):
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
  if (!nv)
    return AI;
  // the seed pass (the wrapper's half of the contract): every Alloc
  // dst names exactly its own allocation
  for (const Instr &I : fn.code)
    if (I.k == K::Alloc && I.dst != NONE && I.dst < nv)
      AI.single[I.dst] = AI.nallocs++;
  // the def records in CODE ORDER (the kernel's fixpoint scans this
  // list; Copies and Phis reference each other -- loop phis -- so the
  // reference chains need the rounds)
  std::vector<opt_ns::alias_def_t> defs{};
  defs.reserve(fn.code.size());
  for (const Instr &I : fn.code) {
    if (I.dst == NONE || I.dst >= nv)
      continue;
    if (I.k == K::Copy) {
      defs.push_back({.var = I.dst,
                      .kind = opt_ns::alias_copy_v,
                      .src = (I.a != NONE && I.a < nv) ? I.a
                                                       : opt_ns::none_v});
    } else if (I.k == K::Phi) {
      // the 2-slot phi args as an N-ary row (an absent arg is obscure
      // to the kernel, exactly the old conservative rule)
      std::vector<uintlen_t> args{};
      args.reserve(2);
      args.push_back((I.a != NONE && I.a < nv) ? (uintlen_t)I.a
                                                : opt_ns::none_v);
      args.push_back((I.b2 != NONE && I.b2 < nv) ? (uintlen_t)I.b2
                                                  : opt_ns::none_v);
      defs.push_back(
          {.var = I.dst, .kind = opt_ns::alias_phi_v, .args = std::move(args)});
    }
  }
  // the kernel: one def per vreg makes single[v] a pure function of
  // its def instr; transitions are monotone NONE->id (an id, once
  // assigned, is stable), so the rounds terminate. 64 = the
  // compiler's historical patience, kept as policy.
  std::vector<uintlen_t> single64(nv);
  for (uint32_t v = 0; v < nv; ++v)
    single64[v] = AI.single[v] == NONE ? opt_ns::none_v : AI.single[v];
  opt_ns::resolve_single_ids<V>(defs, single64, nv, 64);
  for (uint32_t v = 0; v < nv; ++v)
    AI.single[v] =
        (single64[v] == opt_ns::none_v) ? NONE : (uint32_t)single64[v];
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
    if (!fn.nblocks())
      continue;
    AliasInfo AI = alias_analyze(fn);
    const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
    auto sgl = [&](uint32_t v) -> bool {
      return v != NONE && v < nv && AI.single[v] != NONE;
    };
    for (uint32_t b = 0; b < fn.nblocks(); ++b)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        if (I.k == K::Load || I.k == K::Store) {
          if (!sgl(I.a))
            lift_ok = false; // could hit ANY array's contents
        } else if (I.k == K::Len) {
          if (!sgl(I.a))
            stkarr_ok = false; // length table would shift
        }
        if (!stkarr_ok)
          continue;
        // observable uses of a handle VALUE (numbers that shift when an
        // earlier allocation leaves the arena)
        switch (I.k) {
        case K::Bin:   // arithmetic/comparison on a handle
        case K::Cbr:   // branching on a handle value
        case K::Print: // printing a handle
        case K::Ret:   // returning a handle to the caller
        case K::Alloc: // a handle as an allocation size
          if (sgl(I.a) || sgl(I.b2))
            stkarr_ok = false;
          break;
        case K::Load: // handle used as an INDEX
          if (sgl(I.b2))
            stkarr_ok = false;
          break;
        case K::Store: // handle used as an index or stored as a value
          if (sgl(I.b2) || sgl(I.c2))
            stkarr_ok = false;
          break;
        case K::Call: // handle passed to a function
          for (uint32_t v : I.args)
            if (sgl(v))
              stkarr_ok = false;
          break;
        default:
          break; // Copy/Phi: plumbing; Len handle position: fine
        }
      }
  }
}

// The array arena: identical bump-allocator semantics in EVERY backend
// (interpreters, bytecode VM, emitted C, emitted x86-64). A handle is the
// index of the array's first element; allocation is monotonic (no free), so
// handle values, aliasing, and allocation order are bit-identical everywhere.
// CAP must match MJZ_ARENA_CAP emitted into the C/asm backends.

// ============================================================================
