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

#include "../front/ast.hpp"     // Parsed (the parser's output type)
#include "../ir/instr.hpp"      // Fn (the IR container)
#include "../ir/stats.hpp"      // SsaStats / DestrStats
#include "../base/pass_cfg.hpp" // PassCfg / OptStats

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
constexpr OptStats sum_ostats(const Program &P) {
  OptStats t{};
  for (const FnUnit &U : P.fns) {
    t.folds += U.ostats.folds;
    t.copies += U.ostats.copies;
    t.brs_folded += U.ostats.brs_folded;
    t.blocks_gone += U.ostats.blocks_gone;
    t.dce += U.ostats.dce;
    t.hoisted += U.ostats.hoisted;
    t.copyprops += U.ostats.copyprops;
    t.cses += U.ostats.cses;
    t.splices += U.ostats.splices;
    t.cbr_same += U.ostats.cbr_same;
    t.phi_degens += U.ostats.phi_degens;
    t.canon_swaps += U.ostats.canon_swaps;
    t.canon_cmps += U.ostats.canon_cmps;
    t.inlines += U.ostats.inlines;
    t.unrolls += U.ostats.unrolls;
    t.reassocs += U.ostats.reassocs;
    t.reassoc_gone += U.ostats.reassoc_gone;
    t.lcf_loops += U.ostats.lcf_loops;
    t.lcf_sims += U.ostats.lcf_sims;
    t.lcf_gensims += U.ostats.lcf_gensims;
    t.constcalls += U.ostats.constcalls;
    t.deadallocs += U.ostats.deadallocs;
    t.tces += U.ostats.tces;
    t.tce_merges += U.ostats.tce_merges;
    t.cdfolds += U.ostats.cdfolds;
    t.ifconvs += U.ostats.ifconvs;
    t.lcf_rt += U.ostats.lcf_rt;
    t.divmagics += U.ostats.divmagics;
    t.notcombs += U.ostats.notcombs;
    t.traps_killed += U.ostats.traps_killed;
    t.mem2reg_lifts += U.ostats.mem2reg_lifts;
    t.mem2reg_dses += U.ostats.mem2reg_dses;
    t.scctco += U.ostats.scctco;
    t.scctco_folds += U.ostats.scctco_folds;
    t.sccp_consts += U.ostats.sccp_consts;
    t.sccp_brs += U.ostats.sccp_brs;
    t.ld_fwds += U.ostats.ld_fwds;
    t.ld_cses += U.ostats.ld_cses;
    t.rangecmps += U.ostats.rangecmps;
    t.cmpfacts += U.ostats.cmpfacts;
    t.callcses += U.ostats.callcses;
    if (U.ostats.iters > t.iters)
      t.iters = U.ostats.iters;
  }
  return t;
}
