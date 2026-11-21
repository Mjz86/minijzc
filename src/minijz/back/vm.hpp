#pragma once
// === vm.hpp ========================================
// the bytecode VM: the builder (emit_bytecode) plus, via the children
// below, the steppable machine (vm_step.hpp) and the SSA interpreter
// (interp_ssa.hpp) — the checkpoint-21 vm tripart. This include order
// preserves the historical single-file definition order, so every
// includer keeps working unchanged and behavior is byte-identical.
//
// ALGORITHM (emit_bytecode, the builder -- two overloaded forms):
//   * pass 1 sizes the image: off[f][b] is each block's absolute
//     byte offset (cur starts at 4 + 8*nfns, the directory size, and
//     sums bc_instr_size over every instr); fn_off[f] = block 0's.
//   * the directory: [u32 nfns][nfns x (u32 code_off, u32 nslots)] --
//     nslots is slot_count(fn): the interference-coloring home count
//     when regalloc ran, one slot per vreg otherwise.
//   * pass 2 lowers each Instr to a micro-sequence of stack ops:
//     Const -> Push imm + Store slot; Copy -> Load + Store; Bin ->
//     Load a, Load b2, bc_of_bin(I.b), Store dst; Cbr -> Load a +
//     Jz false-target + Jmp true-target; Call pushes args in
//     parameter order then Call fnidx nargs; Ret loads (Push 0 when
//     void) then Ret; Phi is skipped (destroyed input); Bkpt keeps
//     its slot ids; jumps are absolute offsets (a NONE successor
//     targets the block's own offset).
//   * srcs (optional out): one source offset per emitted byte (I.src,
//     or NONE) -- the debugger's line breakpoints; runs never build it.
//
// DATA STRUCTURES: off (per-fn block offsets), fn_off (entry points),
// c (the image; bc_put_u32/bc_put_i64 append), srcs.
//
// THEORY: after SSA destruction every vreg has a home via slot_id --
// fn.slot_of, the coloring where vregs with disjoint live ranges share
// one slot (params pinned to 0..nparams-1 for the Call convention), or
// the identity map. The VM NEVER spills: colors are unbounded because
// frames have unlimited slots; slot_count IS the frame size.
//
// CONSUMERS: vm_step/vm_run (vm_step.hpp) run the image;
// emit_bc_listing disassembles it; cli_run.hpp + the tours build it.

#include "../ir/instr.hpp"

// (MJZ_MAX_CALL_DEPTH is defined with the const-call evaluator in 9e3)

#include "bc_defs.hpp"

// layout: [u32 nfns][nfns x (u32 code_off, u32 nslots)][code...]
// fns[0] is the main body; block jumps are absolute byte offsets.
// srcs (optional out): per-byte source offsets — every byte of an emitted
// op carries the IR instr's src (NONE when synthesized). The debugger uses
// it for line breakpoints and step traces; normal runs never build it.
#ifdef MJZ_MULTI_CPP
// both bodies compiled ONCE in tu/runners.cpp (back/vm_impl.hpp)
std::vector<uint8_t> emit_bytecode(const std::vector<Fn> &fns,
                                   std::vector<uint32_t> *srcs);
std::vector<uint8_t> emit_bytecode(const std::vector<Fn> &fns);
#else
constexpr std::vector<uint8_t> emit_bytecode(const std::vector<Fn> &fns,
                                             std::vector<uint32_t> *srcs);
constexpr std::vector<uint8_t> emit_bytecode(const std::vector<Fn> &fns);
#include "vm_impl.hpp"
#endif

// ---- checkpoint 21: the vm tripart ---------------------------------------
// vm.hpp keeps the bytecode builder's DECLARATIONS (bodies: vm_impl.hpp,
// compiled once in tu/runners.cpp under MJZ_MULTI_CPP, constexpr-inline
// otherwise); the opcode/accessor layer lives in bc_defs.hpp (included
// above), the steppable machine in vm_step.hpp (VmCtx + vm_init +
// vm_step_op + vm_run -- the debugger's live view) and the SSA interpreter
// in interp_ssa.hpp (run_prog).
#include "interp_ssa.hpp"
#include "vm_step.hpp"
