#pragma once
// === bytecode.hpp ========================================
// bytecode emission + listing/disassembly.
//
// WHAT (emit_bc_listing): a text disassembly of an emit_bytecode image
// (the emission half lives in vm.hpp; this file is also the umbrella
// that pulls the vm tripart in for the CLI/debugger/fuzz/shrink).
//   * header line "; %zu bytes, %u fn(s)"; per fn "off=%u slots=%u"
//     re-read from the [u32 nfns][nfns x (u32 off, u32 nslots)]
//     directory; a fn's extent runs to the next fn's offset, or the
//     image end for the last fn.
//   * per op: "%6zu %-6s" (byte offset + bc_name) then the immediates
//     -- Push prints its i64; Load/Store print the v%u slot id (the
//     same home the machine's frames index); Jmp/Jz the absolute
//     target offset; Call "f%u (%u args)"; Bkpt its SLOT ids (at most
//     8 rendered, "..." beyond -- the operand bytes are ALWAYS walked,
//     so the listing stays in lockstep with the encoder).
//
// DATA: reads only the image through the bc_defs.hpp accessors
// (bc_rd_u32/bc_rd_i64, bc_name) -- no IR, no state; a static free
// function, safe in every include mode.
//
// THEORY: the listing is the human-facing view of the very bytes
// vm_step_op executes: the printed offsets are the pc values a
// debugger session stops at, and Bkpt's slot list is what it shows.
//
// CONSUMERS: app_main.hpp (--list) and dbg_cmds.hpp (the debugger's
// disasm command).

#include "../ir/instr.hpp"
#include "vm.hpp"

// ---- 13.5c. bytecode listing (disassembly)
// -----------------------------------
static std::string emit_bc_listing(const std::vector<uint8_t> &c) {
  std::string s{};
  char buf[160];
  if (c.size() < 4)
    return s;
  const uint32_t nfns = bc_rd_u32(c, 0);
  std::snprintf(buf, sizeof buf, "; %zu bytes, %u fn(s)\n", (size_t)c.size(),
                (unsigned)nfns);
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
        std::snprintf(buf, sizeof buf, " %lld\n", (long long)bc_rd_i64(c, pc));
        s += buf;
        pc += 8;
        break;
      case Bc::Load:
      case Bc::Store:
        std::snprintf(buf, sizeof buf, " v%u\n", (unsigned)bc_rd_u32(c, pc));
        s += buf;
        pc += 4;
        break;
      case Bc::Jmp:
      case Bc::Jz:
        std::snprintf(buf, sizeof buf, " %u\n", (unsigned)bc_rd_u32(c, pc));
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
      case Bc::Bkpt: {
        uint32_t n = bc_rd_u32(c, pc);
        pc += 4;
        std::snprintf(buf, sizeof buf, " %u slot%s", (unsigned)n,
                      n == 1 ? "" : "s");
        s += buf;
        for (uint32_t k = 0; k < n; ++k, pc += 4) {
          if (k >= 8)
            continue; // render at most 8; ALWAYS advance past the operand
          std::snprintf(buf, sizeof buf, " s%u", (unsigned)bc_rd_u32(c, pc));
          s += buf;
        }
        if (n > 8)
          s += " ...";
        s += "\n";
        break;
      }
      default:
        s += "\n";
        break;
      }
    }
  }
  return s;
}

// ---- 13.5d. native build & run harness
// ----------------------------------------
