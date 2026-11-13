#pragma once
// === emit_x64.hpp ========================================
// x86-64 asm backend entry. The register allocator (chordal coloring via
// mjz_lib) plus the codegen live in emit_x64_impl.hpp, compiled ONCE
// (tu/backends.cpp in the multi-TU build) behind this light declaration.
//
// WHAT: emit_x64(fns, names, src) is the whole-program x86-64 entry --
// the per-fn codegen (emit_x64_fn), the register allocator (RaPlan +
// ra_plan in regalloc.hpp) and the shared asm runtime (mjz_trap, the
// arena helpers mjz_alloc/mjz_len/mjz_load/mjz_store, mjz_input, the
// .rodata formats, the .bss arena) all live in emit_x64_impl.hpp.
//
// BUILD MODES: under MJZ_MULTI_CPP a declaration only (defined once in
// tu/backends.cpp); otherwise the impl is included here -- the
// header-only build still links exactly one definition.
//
// THEORY (homes after SSA destruction): unlike the C/VM slot view this
// backend has a FIXED pool -- the five callee-saved registers
// rbx/r12-r15, picked so anything live across a call needs no extra
// saving. ra_plan chordal-colors the SSA interference graph (greedy in
// maximum-cardinality-search order, OPTIMAL for the chordal graphs SSA
// shapes produce); values the 5 colors cannot house DO spill to
// -N(%rbp) frame slots. The register audit (regaudit.hpp) regenerates
// the SAME plan -- one allocator, two consumers -- so audit and
// codegen can never disagree.
//
// CONSUMERS: cli_run.hpp writes the .s artifact; the autodetected cc
// builds it in the native -O matrix.

#include "../ir/instr.hpp"

#include <string>
#include <string_view>
#include <vector>

#ifdef MJZ_MULTI_CPP
// defined in tu/backends.cpp (via emit_x64_impl.hpp)
std::string emit_x64(const std::vector<Fn> &fns,
                     const std::vector<std::string_view> &names,
                     std::string_view src = {});
#else
// runtime-only (static locals in the codegen); the header-only build still
// compiles it exactly once — one TU per binary includes the umbrella
std::string emit_x64(const std::vector<Fn> &fns,
                     const std::vector<std::string_view> &names,
                     std::string_view src = {});
#include "emit_x64_impl.hpp"
#endif
