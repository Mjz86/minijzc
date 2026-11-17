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

#include "../ir/instr.hpp"
#include "../opt/opt_generic.hpp"

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

inline EmitPlan make_emit_plan(const Fn &fn) {
  EmitPlan P{};
  const uint32_t nb = fn.nblocks();
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  // the kernel rows: valid succs in SLOT order (the tiebreak vocabulary)
  std::vector<std::vector<ulen>> succ_rows(nb);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t s : {fn.succ0(b), fn.succ1(b)})
      if (s != NONE && s < nb)
        succ_rows[b].push_back(s);
  // reachability from entry + RPO (succ0 first => loop headers precede
  // their bodies) + the trace chains: all three in the library now
  const std::vector<char> reach =
      opt_ns::calculate_reachable_mask(succ_rows, 0);
  const std::vector<ulen> rpo =
      opt_ns::calculate_reverse_postorder(succ_rows, reach, 0);
  std::vector<ulen> score(nb, 0);
  for (uint32_t b = 0; b < nb; ++b)
    score[b] = ulen(fn.preds[b].size()); // fewer preds: the cheaper jump
  const opt_ns::trace_layout_t tr =
      opt_ns::calculate_trace_layout(succ_rows, reach, score, rpo);
  P.order.clear();
  P.order.reserve(tr.order.size());
  for (ulen b : tr.order)
    P.order.push_back(uint32_t(b));
  P.next.assign(nb, NONE);
  for (uint32_t b = 0; b < nb; ++b)
    P.next[b] = (tr.next[b] == opt_ns::none_v) ? NONE : uint32_t(tr.next[b]);
  // constants: a vreg is literal-inlinable ONLY when it has exactly ONE def
  // and that def is a Const. Post-destruction phi slots can hold several
  // defs, one of which post_destruct_peephole may have folded into a Const
  // (e.g. `c3 = 5` on the entry edge, `c3 = t` on the back edge): inlining
  // the one Const def at every use would freeze the slot's other-path
  // values to that constant and miscompile the loop test.
  P.kc.assign(nv, 0);
  P.kv.assign(nv, 0);
  {
    std::vector<uint32_t> ndefs(nv, 0);
    for (const Instr &I : fn.code)
      if (I.dst != NONE && I.dst < nv)
        ++ndefs[I.dst];
    for (const Instr &I : fn.code)
      if (I.k == K::Const && I.dst != NONE && I.dst < nv && ndefs[I.dst] == 1) {
        P.kc[I.dst] = 1;
        P.kv[I.dst] = I.c;
      }
  }
  // raw use counts + def index (for fusion decisions); ndefs also gates
  // fusion (see the comment at the fuse loop below)
  std::vector<uint32_t> raw(nv, 0);
  std::vector<uint32_t> def_idx(nv, NONE);
  std::vector<uint32_t> ndefs(nv, 0);
  for (uint32_t i = 0; i < fn.code.size(); ++i)
    if (fn.code[i].dst != NONE && fn.code[i].dst < nv)
      ++ndefs[fn.code[i].dst];
  for (uint32_t i = 0; i < fn.code.size(); ++i) {
    const Instr &I = fn.code[i];
    if (I.a != NONE && I.a < nv)
      raw[I.a]++;
    if (I.b2 != NONE && I.b2 < nv)
      raw[I.b2]++;
    if (I.c2 != NONE && I.c2 < nv)
      raw[I.c2]++;
    for (uint32_t v : I.args)
      if (v < nv)
        raw[v]++;
    if (I.dst != NONE && I.dst < nv)
      def_idx[I.dst] = i;
  }
  // branch fusion: cbr on a single-use comparison
  P.fuse.assign(fn.code.size(), NONE);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k != K::Cbr || I.a == NONE || I.a >= nv)
        continue;
      uint32_t d = def_idx[I.a];
      // single USE is not enough post-destruction: a phi slot has one use
      // (this branch) but one def per incoming edge; fusing "the" def would
      // pick an arbitrary edge's comparison. Require a single DEF too.
      if (d == NONE || raw[I.a] != 1 || ndefs[I.a] != 1)
        continue;
      const Instr &D = fn.code[d];
      bool is_cmp =
          D.k == K::Bin && (D.b == B::Lt || D.b == B::Le || D.b == B::Gt ||
                            D.b == B::Ge || D.b == B::Eq || D.b == B::Ne);
      if (is_cmp)
        P.fuse[i] = d;
    }
  // textual uses (consts inlined, fused cbrs read the comparison operands)
  P.uses.assign(nv, 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const)
        continue; // inlined at every use
      if (P.fuse[i] != NONE) {
        const Instr &D = fn.code[P.fuse[i]];
        if (D.a != NONE && D.a < nv && !P.kc[D.a])
          P.uses[D.a]++;
        if (D.b2 != NONE && D.b2 < nv && !P.kc[D.b2])
          P.uses[D.b2]++;
        continue;
      }
      for (uint32_t v : {I.a, I.b2, I.c2})
        if (v != NONE && v < nv && !P.kc[v])
          P.uses[v]++;
      for (uint32_t v : I.args)
        if (v != NONE && v < nv && !P.kc[v])
          P.uses[v]++;
    }
  // dead: pure instrs whose dst is never textually read
  P.dead.assign(fn.code.size(), 0);
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Const) {
        // dead (inlined at every use) only when the slot is const-inlinable,
        // i.e. single-def; a multi-def slot's Const edge write is real code
        P.dead[i] = (I.dst != NONE && I.dst < nv && P.kc[I.dst]) ? 1 : 0;
        continue;
      }
      if (I.dst == NONE || I.dst >= nv)
        continue;
      if (P.uses[I.dst] != 0)
        continue;
      bool pure = I.k == K::Copy || I.k == K::Len ||
                  (I.k == K::Bin && I.b != B::Div && I.b != B::Mod);
      if (pure)
        P.dead[i] = 1;
    }
  for (uint32_t d : P.fuse)
    if (d != NONE)
      P.dead[d] = 1; // consumed by the branch text
  return P;
}

// ---- 13.5a. C backend
// -------------------------------------------------------- Emits C valid as
// both C and C++. One function per FnUnit: helpers get `static int64_t
// mjz_fN(...)` signatures (params are vregs v0..vP-1), a depth guard (runaway
// recursion TRAPs exactly like the interpreters/VM), and MJZ_RET epilogues;
// main is `int main(void)`. Arithmetic goes through uint64_t casts: the wraps
// the interpreters produce are UB-free here; div/mod are guarded so the trap
// fires BEFORE the UB division would.
