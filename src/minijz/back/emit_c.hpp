#pragma once
// === emit_c.hpp ========================================
// C backend entry. The plan (EmitPlan/make_emit_plan) lives in
// back/emit_plan.hpp; the codegen lives in emit_c_impl.hpp and is
// compiled ONCE (tu/backends.cpp in the multi-TU build) behind this
// light declaration.
//
// WHAT: emit_c(fns, names, src) is the whole-program C entry -- the
// per-fn emitter emit_c_fn, the feature-gated runtime prelude
// (mjz_alloc/mjz_len/mjz_load/mjz_store, mjz_input, mjz_depth +
// MJZ_RET), the callee prototypes and the callees-then-main assembly
// all live in emit_c_impl.hpp. src only drives the source-location
// comments (empty src = none).
//
// BUILD MODES: under MJZ_MULTI_CPP this header declares only (the
// definition is compiled once in tu/backends.cpp); otherwise it
// includes emit_c_impl.hpp right here -- the header-only build still
// gets exactly one definition because one TU per binary includes the
// umbrella.
//
// THEORY (where SSA values live): every vreg is addressed through
// slot_id(fn, v) -- the interference-coloring home in fn.slot_of when
// regalloc ran (vregs with disjoint live ranges share one r%u local;
// params keep slots 0..nparams-1), or the identity v%u ("SSA-like")
// map with --no-regalloc. The C backend NEVER spills: colors are
// unbounded because C has unlimited locals -- fewer colors just means
// fewer declarations. (The x64 backend, with its fixed 5-register
// pool, does spill; see emit_x64.hpp.)
//
// CONSUMERS: cli_run.hpp writes the .c artifact; the output must stay
// valid as BOTH C and C++ (the autodetected driver prefers clang++).

#include "../ir/instr.hpp"

#include <string>
#include <string_view>
#include <vector>

// src (the source text the program was compiled from) drives the source
// location comments: every emitted statement group carries the line:col of
// the AST token that produced it ("a rough idea of what happens where").
// Empty src = no annotations.
#ifdef MJZ_MULTI_CPP
// defined in tu/backends.cpp (via emit_c_impl.hpp)
std::string emit_c(const std::vector<Fn> &fns,
                   const std::vector<std::string_view> &names,
                   std::string_view src = {});
#else
// runtime-only (the codegen builds strings); the header-only build still
// compiles it exactly once — one TU per binary includes the umbrella
std::string emit_c(const std::vector<Fn> &fns,
                   const std::vector<std::string_view> &names,
                   std::string_view src = {});
#include "emit_c_impl.hpp"
#endif
