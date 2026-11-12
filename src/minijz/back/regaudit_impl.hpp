#pragma once
// === regaudit_impl.hpp ========================================
// the register-allocation audit body (regaudit.hpp's impl half; split
// out the emit_x64_impl way so the light declaration can travel).
// Included by regaudit.hpp in the header-only build and by
// tu/backends.cpp in the multi-TU build — include it nowhere else.
//
// Soundness of the traffic accounting: every number is derived from
// the SAME RaPlan + EmitPlan the codegen reads, with the codegen's own
// discipline — a textual use of a spilled vreg is exactly one slot
// read (the `ld` helper and the in-place operand text each emit one
// movq from the slot), a live def of a spilled vreg is exactly one slot
// write (st_rax / the direct stores), and a spilled incoming param is
// one prologue write. Nothing is estimated.

#include "regaudit.hpp" // the struct half (pragma once breaks the cycle)
#include "regalloc.hpp"
#include "emit_plan.hpp"

#include <bit> // std::popcount (the C++20 spelling; MSVC has no builtins)
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

// one row per fn; the plan + the coloring are rebuilt exactly as the
// codegen does (make_emit_plan -> ra_plan), so the audit and the
// emitted asm cannot disagree.
RegAuditOut reg_audit(const std::vector<Fn> &fns,
                      const std::vector<std::string_view> &names) {
  RegAuditOut A{};
  for (uint32_t fi = 0; fi < (uint32_t)fns.size(); ++fi) {
    const Fn &fn = fns[fi];
    EmitPlan P = make_emit_plan(fn);
    RaPlan R = ra_plan(fn, P);
    const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
    RegAuditRow row{};
    row.name = fi < names.size() ? names[fi] : "fn";
    row.blocks = (uint32_t)P.order.size();
    // the use walker — the allocator's each_use discipline (fused cbrs
    // read the comparison's operands; inlined consts are not uses),
    // plus the codegen's own dead skip: P.uses[] alone would count uses
    // made by dead instructions, which the codegen never emits. Only
    // blocks in P.order are walked (the emitted layout).
    auto each_use = [&](uint32_t i, auto &&f) {
      if (P.dead[i])
        return;
      const Instr &I = fn.code[i];
      if (I.k == K::Const)
        return; // inlined at every use; its def is a store, not a read
      auto one = [&](uint32_t v) {
        if (v != NONE && v < nv && !P.kc[v])
          f(v);
      };
      if (P.fuse[i] != NONE) { // the branch reads the comparison operands
        const Instr &D = fn.code[P.fuse[i]];
        one(D.a);
        one(D.b2);
        return;
      }
      one(I.a);
      one(I.b2);
      one(I.c2);
      for (uint32_t v : I.args)
        one(v);
    };
    uint32_t reloads = 0, stores = 0;
    // spilled incoming params: the prologue's `movq %argreg, slot`
    for (uint32_t p = 0; p < fn.nparams && p < nv; ++p)
      if (R.color[p] == 5)
        stores++;
    for (uint32_t v = 0; v < nv; ++v)
      if (R.color[v] == 5)
        row.spill_slots++;
      else if (R.color[v] >= 0)
        row.housed++;
    row.vregs = row.housed + row.spill_slots;
    for (uint32_t b : P.order)
      for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
        const Instr &I = fn.code[i];
        // live defs of spilled vregs: one store each — the is_def
        // discipline of the allocator (dead skipped, inlined consts
        // are not defs), and a Const def is real code when its slot is
        // not single-def inlinable: `movq $c, slot` is a store
        if (!P.dead[i] && I.dst != NONE && I.dst < nv && R.color[I.dst] == 5)
          if (I.k != K::Const || !P.kc[I.dst])
            stores++;
        each_use(i, [&](uint32_t v) {
          if (R.color[v] == 5)
            reloads++;
        });
      }
    row.reloads = reloads;
    row.stores = stores;
    row.regs = (uint32_t)std::popcount(R.usedmask);
    A.tot_vregs += row.vregs;
    A.tot_housed += row.housed;
    A.tot_spill_slots += row.spill_slots;
    A.tot_reloads += row.reloads;
    A.tot_stores += row.stores;
    A.tot_regs += row.regs;
    A.tot_blocks += row.blocks;
    if (row.spill_slots == 0)
      A.zero_spill_fns++;
    const uint32_t traffic = row.reloads + row.stores;
    if (traffic > A.worst_traffic) {
      A.worst_traffic = traffic;
      A.worst = (uint32_t)A.rows.size();
    }
    A.rows.push_back(row);
  }
  return A;
}

std::string reg_audit_text(const RegAuditOut &A,
                           const std::vector<std::string_view> &names) {
  (void)names; // row names are already borrowed
  std::string s{};
  char buf[160];
  s += "reg-audit: " + std::to_string(A.rows.size()) +
       " fns | x86-64 pool "
       "rbx,r12-r15 (callee-saved) | destroyed IR\n";
  s += "  fn                    blocks  vregs  housed  spills  "
       "reloads  stores  regs\n";
  for (const RegAuditRow &r : A.rows) {
    // NUL-safe: a string_view has no terminator (the codegen converts to
    // std::string for the same reason) — print with the explicit length
    std::snprintf(buf, sizeof buf, "  %-22.*s %6u %6u %7u %7u %8u %7u %5u\n",
                  (int)r.name.size(), r.name.data(), (unsigned)r.blocks,
                  (unsigned)r.vregs, (unsigned)r.housed,
                  (unsigned)r.spill_slots, (unsigned)r.reloads,
                  (unsigned)r.stores, (unsigned)r.regs);
    s += buf;
  }
  std::snprintf(buf, sizeof buf,
                "  totals: %u blocks | %u vregs | %u housed | %u spill slots"
                " | traffic %u (%u rd + %u wr) | %u/%u fns spill-free\n",
                (unsigned)A.tot_blocks, (unsigned)A.tot_vregs,
                (unsigned)A.tot_housed, (unsigned)A.tot_spill_slots,
                (unsigned)(A.tot_reloads + A.tot_stores),
                (unsigned)A.tot_reloads, (unsigned)A.tot_stores,
                (unsigned)A.zero_spill_fns, (unsigned)A.rows.size());
  s += buf;
  if (A.worst < A.rows.size() && A.worst_traffic > 0) {
    const RegAuditRow &r = A.rows[A.worst];
    std::snprintf(buf, sizeof buf,
                  "  worst: %.*s — %u spills, %u traffic (%u rd + %u wr)\n",
                  (int)r.name.size(), r.name.data(), (unsigned)r.spill_slots,
                  (unsigned)A.worst_traffic, (unsigned)r.reloads,
                  (unsigned)r.stores);
    s += buf;
  }
  return s;
}
