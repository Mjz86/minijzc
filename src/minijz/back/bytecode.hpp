#pragma once
// === bytecode.hpp ========================================
// bytecode emission + listing/disassembly.

#include "minijz/ir/instr.hpp"
#include "minijz/back/vm.hpp"

// ---- 13.5c. bytecode listing (disassembly) -----------------------------------
static std::string emit_bc_listing(const std::vector<uint8_t> &c) {
  std::string s{};
  char buf[160];
  if (c.size() < 4) return s;
  const uint32_t nfns = bc_rd_u32(c, 0);
  std::snprintf(buf, sizeof buf, "; %zu bytes, %u fn(s)\n",
                (size_t)c.size(), (unsigned)nfns);
  s += buf;
  for (uint32_t f = 0; f < nfns && c.size() >= 4 + (size_t)8 * (f + 1); ++f) {
    uint32_t fo = bc_rd_u32(c, 4 + (size_t)8 * f);
    uint32_t sl = bc_rd_u32(c, 4 + (size_t)8 * f + 4);
    uint32_t eo = (f + 1 < nfns) ? bc_rd_u32(c, 4 + (size_t)8 * (f + 1))
                                 : (uint32_t)c.size();
    std::snprintf(buf, sizeof buf, "; fn %u: off=%u slots=%u\n", f, fo, sl);
    s += buf;
    size_t pc = fo;
    while (pc < eo && pc < c.size()) {
      Bc op = (Bc)c[pc];
      std::snprintf(buf, sizeof buf, "%6zu  %-6s", pc, bc_name(op));
      s += buf;
      ++pc;
      switch (op) {
      case Bc::Push:
        std::snprintf(buf, sizeof buf, " %lld\n",
                      (long long)bc_rd_i64(c, pc));
        s += buf;
        pc += 8;
        break;
      case Bc::Load:
      case Bc::Store:
        std::snprintf(buf, sizeof buf, " v%u\n",
                      (unsigned)bc_rd_u32(c, pc));
        s += buf;
        pc += 4;
        break;
      case Bc::Jmp:
      case Bc::Jz:
        std::snprintf(buf, sizeof buf, " %u\n",
                      (unsigned)bc_rd_u32(c, pc));
        s += buf;
        pc += 4;
        break;
      case Bc::Call:
        std::snprintf(buf, sizeof buf, " f%u (%u args)\n",
                      (unsigned)bc_rd_u32(c, pc),
                      (unsigned)bc_rd_u32(c, pc + 4));
        s += buf;
        pc += 8;
        break;
      default: s += "\n"; break;
      }
    }
  }
  return s;
}

// ---- 13.5d. native build & run harness ----------------------------------------
