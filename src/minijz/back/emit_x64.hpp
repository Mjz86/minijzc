#pragma once
// === emit_x64.hpp ========================================
// x86-64 asm backend entry. The register allocator (chordal coloring via
// mjz_lib) plus the codegen live in emit_x64_impl.hpp, compiled ONCE
// (tu/backends.cpp in the multi-TU build) behind this light declaration.

#include "minijz/ir/instr.hpp"

#include <string>
#include <string_view>
#include <vector>

#ifdef MJZ_MULTI_CPP
// defined in tu/backends.cpp (via emit_x64_impl.hpp)
std::string emit_x64(const std::vector<Fn> &fns,
                     const std::vector<std::string_view> &names);
#else
// runtime-only (static locals in the codegen); the header-only build still
// compiles it exactly once — one TU per binary includes the umbrella
std::string emit_x64(const std::vector<Fn> &fns,
                     const std::vector<std::string_view> &names);
#include "minijz/back/emit_x64_impl.hpp"
#endif
