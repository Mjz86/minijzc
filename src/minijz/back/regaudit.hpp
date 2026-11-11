#pragma once
// === regaudit.hpp ========================================
// the register-allocation audit (checkpoint 21): a read-only second
// consumer of regalloc.hpp — the SAME RaPlan the x86-64 codegen gets,
// reported as a per-function table instead of asm. One allocator,
// two consumers: the audit cannot drift from what is emitted.
//
// The metrics (all on the DESTROYED code, after all optimization):
//   vregs        values with a home (params + live defs) — the located set
//   housed       of those, the ones the coloring kept in the 5-register
//                callee-saved pool (rbx, r12-r15)
//   spill-slots  the rest — one -N(%rbp) slot each
//   reloads      spill-slot READS: one per textual use of a spilled vreg
//                (the codegen's `ld`/in-place operand read: exactly the
//                `movq -N(%rbp), %r..` count the asm would carry)
//   stores       spill-slot WRITES: one per live def of a spilled vreg
//                plus one per spilled incoming param (the prologue's
//                `movq %rdi, -N(%rbp)`)
//   regs         the callee-saved subset actually used (pushed in the
//                prologue) — popcount of the used mask
//
// LIGHT declaration header (the emit_x64.hpp pattern): the struct
// definitions are safe everywhere; the functions are declared, and the
// impl is compiled once — regaudit_impl.hpp via this header in the
// header-only build, via tu/backends.cpp in the multi-TU build.

#include "../ir/instr.hpp"

#include <string>
#include <string_view>
#include <vector>

struct RegAuditRow {
  std::string_view name{}; // borrowed from the fn table
  uint32_t blocks{};       // reachable blocks (the emitted layout)
  uint32_t vregs{};        // located values (params + live defs)
  uint32_t housed{};       // kept in the register pool
  uint32_t spill_slots{};  // spilled values
  uint32_t reloads{};      // spill-slot reads
  uint32_t stores{};       // spill-slot writes
  uint32_t regs{};         // callee-saved registers used (0..5)
};

struct RegAuditOut {
  std::vector<RegAuditRow> rows{};
  uint32_t tot_vregs{}, tot_housed{}, tot_spill_slots{}, tot_reloads{},
      tot_stores{}, tot_regs{}, tot_blocks{};
  uint32_t zero_spill_fns{}; // fns with spill_slots == 0
  uint32_t worst{};          // row index with the max reloads+stores
  uint32_t worst_traffic{};
};

#ifdef MJZ_MULTI_CPP
// defined in tu/backends.cpp (via regaudit_impl.hpp)
RegAuditOut reg_audit(const std::vector<Fn> &fns,
                      const std::vector<std::string_view> &names);
std::string reg_audit_text(const RegAuditOut &A,
                           const std::vector<std::string_view> &names);
#else
// runtime-only (plain functions, like the allocator they read)
RegAuditOut reg_audit(const std::vector<Fn> &fns,
                      const std::vector<std::string_view> &names);
std::string reg_audit_text(const RegAuditOut &A,
                           const std::vector<std::string_view> &names);
#include "regaudit_impl.hpp"
#endif
