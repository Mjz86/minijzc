#pragma once
// === program.hpp ========================================
// Program + FnUnit: the pipeline's composite data types. LIGHT by design —
// only the IR, the stats records and the parsed AST are needed to NAME
// these; every TU can hold a Program without parsing mjz_lib or the
// optimizer.
//
// compile() is the heavy side (parser -> builder -> SSA -> optimizer ->
// destruction): its DECLARATION is below, its body lives in
// prog/compile_impl.hpp, compiled once (tu/prog.cpp in the multi-TU
// build; constexpr-inline in the header-only build).

#include "../base/pass_cfg.hpp" // PassCfg / OptStats
#include "../front/ast.hpp"     // Parsed (the parser's output type)
#include "../ir/instr.hpp"      // Fn (the IR container)
#include "../ir/stats.hpp"      // SsaStats / DestrStats

#include <string_view>
#include <vector>

struct FnUnit {   // one function's journey through the pipeline
  Fn ssa{};       // SSA before optimization
  Fn opt{};       // SSA after optimization
  Fn destroyed{}; // after SSA destruction (no phis; slots only)
  SsaStats sstats{};
  OptStats ostats{};
  DestrStats dstats{};
};
struct Program {
  Parsed P{};
  std::string_view src{};    // the full source text (byte offsets in
                             // Instr::src index into this); the buffer must
                             // outlive the Program, exactly like P.ast.names
                             // (string_views into it) already do
  std::vector<Fn> ssa_all{}; // pre-opt SSA of EVERY fn (original indices;
                             // the unoptimized runners need the full set even
                             // after the optimizer dropped dead functions)
  std::vector<FnUnit> fns{}; // [0] = main body, then fns
  std::vector<std::string_view> fn_names{}; // parallel source names
  bool uses_input{};                        // the program contains input()
  bool ok{};
  const char *error{};
  uint32_t errpos{};
};

// the composite pipeline entry (defined in compile_impl.hpp; see above)
#ifdef MJZ_MULTI_CPP
Program compile(std::string_view src, PassCfg cfg = {}); // tu/prog.cpp
#else
constexpr Program compile(std::string_view src, PassCfg cfg = {});
#define MJZ_PIPELINE_FN constexpr
#include "compile_impl.hpp"
#undef MJZ_PIPELINE_FN
#endif
// main's pre-optimization CFG analysis (dominators/loops), pretty-printed;
// defined with compile() — the An internals never cross this boundary
void print_pre_main_analysis(const Program &P);

// assemble one IR view (0 = ssa, 1 = opt, 2 = destroyed) of a program
constexpr std::vector<Fn> fns_of(const Program &P, int which) {
  if (which == 0)
    return P.ssa_all; // unoptimized: ALL functions, orig indices
  std::vector<Fn> out{};
  out.reserve(P.fns.size());
  for (const FnUnit &U : P.fns)
    out.push_back(which == 1 ? U.opt : U.destroyed);
  return out;
}
// summed optimization statistics across all functions (iters: max)
#ifdef MJZ_MULTI_CPP
OptStats sum_ostats(const Program &P); // tu/runners.cpp (prog/printers_impl.hpp)
#else
constexpr OptStats sum_ostats(const Program &P); // body: prog/printers_impl.hpp
#endif
