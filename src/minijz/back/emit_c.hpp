#pragma once
// === emit_c.hpp ========================================
// C backend entry. The plan (EmitPlan/make_emit_plan) lives in
// back/emit_plan.hpp; the codegen lives in emit_c_impl.hpp and is
// compiled ONCE (tu/backends.cpp in the multi-TU build) behind this
// light declaration.

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
