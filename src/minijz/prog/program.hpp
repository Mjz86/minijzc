#pragma once
// === program.hpp ========================================
// Program + compile(): the composite front-end/optimizer entry.

#include "minijz/front/parser.hpp"
#include "minijz/ir/builder.hpp"
#include "minijz/opt/driver.hpp"
#include "minijz/ir/destruct.hpp"

// ============================================================================
// 12. full pipeline driver
// ============================================================================
struct FnUnit { // one function's journey through the pipeline
  Fn ssa{};        // SSA before optimization
  Fn opt{};        // SSA after optimization
  Fn destroyed{};  // after SSA destruction (no phis; slots only)
  SsaStats sstats{};
  OptStats ostats{};
  DestrStats dstats{};
};
struct Program {
  Parsed P{};
  std::vector<Fn> ssa_all{}; // pre-opt SSA of EVERY fn (original indices;
                             // the unoptimized runners need the full set even
                             // after the optimizer dropped dead functions)
  std::vector<FnUnit> fns{};                // [0] = main body, then fns
  std::vector<std::string_view> fn_names{}; // parallel source names
  An an_pre_main{};                         // main's unoptimized SSA CFG
  bool uses_input{}; // the program contains input()
  bool ok{};
  const char *error{};
  uint32_t errpos{};
};

constexpr Program compile(std::string_view src, PassCfg cfg = {}) {
  Program P{};
  P.P = parse(src);
  if (!P.P.ok) { P.error = P.P.err.what; P.errpos = P.P.err.pos; return P; }
  const Ast &a = P.P.ast;
  // call arity must match the definitions (checked post-parse: definitions
  // may appear anywhere in the file)
  for (const Expr &e : a.es)
    if (e.k == Ek::Call) {
      const FnDef &fd = a.fns[e.var];
      if (fd.params.size() != a.seq_kids[e.l].size()) {
        P.error = "wrong number of arguments in call";
        P.errpos = 0;
        return P;
      }
    }
  const uint32_t nf = 1 + (uint32_t)a.fns.size();
  std::vector<Fn> ssa0(nf);
  std::vector<SsaStats> sst(nf);
  {
    Builder B(ssa0[0]);
    B.build_main(a, P.P.root);
    if (ssa0[0].nblocks() == 0) { P.error = "empty program"; return P; }
    An A = analyze(ssa0[0]);
    if (!ssa_construct(ssa0[0], A, sst[0])) {
      P.error = "SSA construction failed (use before def?)";
      return P;
    }
    P.an_pre_main = A;
  }
  for (uint32_t k = 0; k < a.fns.size(); ++k) {
    Builder B(ssa0[k + 1]);
    B.build_fn(a, a.fns[k]);
    An A = analyze(ssa0[k + 1]);
    if (!ssa_construct(ssa0[k + 1], A, sst[k + 1])) {
      P.error = "SSA construction failed (use before def?)";
      return P;
    }
  }
  // whole-program optimization: per-fn fixpoints, callgraph SCC inlining,
  // dead-function elimination, final pass (unrolling included).
  // The program-wide handle gates (array promotion / C stack arrays) are
  // computed on the PRE-optimization IR of every function.
  std::vector<Fn> work = ssa0;
  std::vector<std::string_view> names(nf);
  names[0] = "<main>";
  for (uint32_t k = 0; k < a.fns.size(); ++k) names[k + 1] = a.fns[k].name;
  std::vector<OptStats> os(nf);
  std::vector<uint32_t> src_index(nf);
  for (uint32_t k = 0; k < nf; ++k) src_index[k] = k;
  prog_handle_gates(ssa0, cfg.lift_ok, cfg.stkarr_ok);
  if (cfg.do_opt)
    optimize_prog(work, names, os, cfg, src_index);
  // assemble the per-function units (dropped fns realigned via src_index)
  P.ssa_all = ssa0;
  P.fns.resize(work.size());
  P.fn_names = names;
  for (uint32_t j = 0; j < work.size(); ++j) {
    FnUnit &U = P.fns[j];
    uint32_t src = src_index[j];
    U.ssa = ssa0[src];
    U.sstats = sst[src];
    if (cfg.stkarr_ok)
      analyze_stack_allocs(work[j]); // stash local arrays before both copies
    else
      work[j].stk_arrays.clear();
    U.opt = work[j];
    U.ostats = os[j];
    U.destroyed = work[j]; // inherits .stk_arrays (vreg ids are stable)
    destroy_ssa(U.destroyed, U.dstats);
    post_destruct_peephole(U.destroyed, U.dstats);
    if (cfg.regalloc) // C + bytecode: minimize the variable/slot count
      assign_slots(U.destroyed);
  }
  P.uses_input = P.P.uses_input;
  P.ok = true;
  return P;
}

// assemble one IR view (0 = ssa, 1 = opt, 2 = destroyed) of a program
constexpr std::vector<Fn> fns_of(const Program &P, int which) {
  if (which == 0) return P.ssa_all; // unoptimized: ALL functions, orig indices
  std::vector<Fn> out{};
  out.reserve(P.fns.size());
  for (const FnUnit &U : P.fns)
    out.push_back(which == 1 ? U.opt : U.destroyed);
  return out;
}
// summed optimization stats across all functions (iters: max)
static OptStats sum_ostats(const Program &P) {
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
    if (U.ostats.iters > t.iters) t.iters = U.ostats.iters;
  }
  return t;
}

// ============================================================================
