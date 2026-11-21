#pragma once
// === emit_plan.hpp ========================================
// the emission plan (EmitPlan + make_emit_plan): everything the native
// backends need to emit non-redundant code, computed once from the
// destroyed IR — block layout/order, inlined constants, fused compares,
// dead flags. Standalone and LIGHT: the C and asm backends and the
// debugger all read it without pulling any backend's codegen.
//
// ALGORITHM (make_emit_plan -- six phases over one destroyed Fn):
//   1. reachability: DFS from block 0 over succ0/succ1.
//   2. reverse postorder, succ0 first => loop headers precede bodies.
//   3. trace layout: walk the RPO; chain each block to its preferred
//      unplaced successor (both open => fewer preds wins; succ1 alone
//      => the false arm falls through, shaping `if (c) goto T;`),
//      linearizing into P.order and P.next (the fallthrough neighbor).
//      [phases 1-3 are the library kernels now: opt_ns::
//      calculate_reachable_mask + opt/layout.hpp's
//      calculate_reverse_postorder + calculate_trace_layout, with the
//      pred-count score; this file keeps only the IR glue -- the NONE
//      filtering and the score tables]
//   4. constants: kc/kv mark a vreg literal-inlinable ONLY with exactly
//      one def that is a Const -- post-destruction phi slots hold one
//      def per edge; inlining one edge's constant everywhere would
//      freeze the loop test (post_destruct_peephole can fold an edge).
//   5. fusion: P.fuse[i] = the compare def a Cbr folds into its branch
//      text; legal only when that vreg has ONE use (this branch) and
//      ONE def (a phi slot would make "the" def arbitrary).
//   6. uses/dead: P.uses counts textual uses (consts inlined, fused
//      branches read the compare's operands); P.dead marks pure instrs
//      with zero textual uses (Const when kc, Copy/Len/safe Bin, and
//      every fused compare def).
//
// DATA STRUCTURES: EmitPlan{order, next, kc, kv, uses, fuse, dead}.
//
// THEORY: the plan is the single truth about what the EMITTED text
//      will read -- liveness and homes must be decided against the
//      same fusion/inlining the codegen performs, or another vreg
//      could lawfully reuse (clobber) a fused compare's live operands.
//
// CONSUMERS: emit_c_fn (emit_c_impl.hpp); emit_x64_fn via ra_plan
//      (regalloc.hpp); the register audit (regaudit_impl.hpp); the slot
//      allocator assign_slots (astrun.hpp) mirrors the fusion gate.

#include "../base/srcmap.hpp"
#include "../ir/instr.hpp"
#include "../opt/opt_generic.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

struct EmitPlan {
  std::vector<uint32_t> order{}; // reachable blocks, emission order
  std::vector<uint32_t> next{};  // per block: block emitted right after
  std::vector<char> kc{};        // per vreg: defined by Const
  std::vector<val_t> kv{};       // per vreg: that constant
  std::vector<uint32_t> uses{};  // per vreg: textual uses after inlining
  std::vector<uint32_t> fuse{};  // per instr: Cbr -> fused compare def idx
  std::vector<char> dead{};      // per instr: skip (pure & unused / inlined)
};

// ---- the branch-fusion gate, shared by make_emit_plan (phase 5) and the
// VM's slot allocator (runtime/astrun.hpp's assign_slots) ------------------
// BOTH consumers must agree on WHICH Cbr folds its comparison into the
// branch text: the C backend reads the comparison's OPERANDS at the branch
// (their live ranges extend past the dead compare), while the VM branches
// on the RESULT -- a disagreement let another vreg reuse (clobber) a fused
// operand's slot mid-range (fuzz seed 33). The algorithm lived inline in
// both files until the ckpt-31 dedup wave; it lives once now. Kept
// allocation-lean (three nv vectors + one code vector) so the constexpr
// slot allocator's evaluation budget is not disturbed.
struct FuseTable {
  std::vector<uint32_t> raw{};     // per vreg: textual use count
  std::vector<uint32_t> def_idx{}; // per vreg: LAST def index
  std::vector<uint32_t> ndefs{};   // per vreg: def count
  std::vector<uint32_t> fuse{};    // per instr: Cbr -> fused compare def
};
constexpr FuseTable make_fuse_table(const Fn &fn, uint32_t nv) {
  FuseTable T{};
  const uint32_t nb = fn.nblocks();
  T.raw.assign(nv, 0);
  T.def_idx.assign(nv, NONE);
  T.ndefs.assign(nv, 0);
  for (uint32_t i = 0; i < fn.code.size(); ++i)
    if (fn.code[i].dst != NONE && fn.code[i].dst < nv)
      ++T.ndefs[fn.code[i].dst];
  for (uint32_t i = 0; i < fn.code.size(); ++i) {
    bump_uses_of(fn.code[i], nv, T.raw); // the shared counter (instr.hpp)
    if (fn.code[i].dst != NONE && fn.code[i].dst < nv)
      T.def_idx[fn.code[i].dst] = i;
  }
  // branch fusion: cbr on a single-use comparison. Single USE is not
  // enough post-destruction: a phi slot has one use (this branch) but one
  // def per incoming edge; fusing "the" def would pick an arbitrary edge's
  // comparison. Require a single DEF too.
  T.fuse.assign(fn.code.size(), NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k != K::Cbr || I.a == NONE || I.a >= nv)
        continue;
      uint32_t d = T.def_idx[I.a];
      if (d == NONE || T.raw[I.a] != 1 || T.ndefs[I.a] != 1)
        continue;
      const Instr &D = fn.code[d];
      bool is_cmp =
          D.k == K::Bin && (D.b == B::Lt || D.b == B::Le || D.b == B::Gt ||
                            D.b == B::Ge || D.b == B::Eq || D.b == B::Ne);
      if (is_cmp)
        T.fuse[i] = d;
    }
  return T;
}

// ---- the label-reference mask, shared by both text backends ----------------
// Which blocks need a label in the emitted text: a branch target that is
// NOT the fallthrough neighbor (P.next). The constant-condition fold
// (unfused Cbr on a known const picks its arm statically) and the
// fallthrough elision are identical policy in the C and the asm emitter
// -- emit_c_impl and emit_x64_impl hand-rolled identical copies until the
// ckpt-31 dedup wave lifted them here.
#ifdef MJZ_MULTI_CPP
// the three bodies compiled ONCE in tu/runners.cpp (back/emit_plan_impl.hpp)
std::vector<char> mark_referenced_blocks(const Fn &fn, const EmitPlan &P);
void append_source_range_banner(std::string &s, const Fn &fn,
                                const SrcMap &SM, bool is_main,
                                std::string_view fname, const char *open,
                                const char *close);
EmitPlan make_emit_plan(const Fn &fn);
#else
inline std::vector<char> mark_referenced_blocks(const Fn &fn,
                                                const EmitPlan &P);
inline void append_source_range_banner(std::string &s, const Fn &fn,
                                       const SrcMap &SM, bool is_main,
                                       std::string_view fname, const char *open,
                                       const char *close);
inline EmitPlan make_emit_plan(const Fn &fn);
#include "emit_plan_impl.hpp"
#endif

// ---- the source-location banner, shared by both text backends --------------
// "which source lines this fn came from": the lo/hi scan over I.src and the
// one-line comment. The C and asm spellings differ ONLY in the comment
// syntax, passed as the (open, close) pair -- the identical scan was
// hand-rolled in both emitters until the ckpt-31 dedup wave.


// ---- 13.5a. C backend
// -------------------------------------------------------- Emits C valid as
// both C and C++. One function per FnUnit: helpers get `static int64_t
// mjz_fN(...)` signatures (params are vregs v0..vP-1), a depth guard (runaway
// recursion TRAPs exactly like the interpreters/VM), and MJZ_RET epilogues;
// main is `int main(void)`. Arithmetic goes through uint64_t casts: the wraps
// the interpreters produce are UB-free here; div/mod are guarded so the trap
// fires BEFORE the UB division would.
