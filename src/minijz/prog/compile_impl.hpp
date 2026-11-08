#pragma once
// === compile_impl.hpp ========================================
// the composite pipeline body: parse -> build -> SSA-construct -> whole-
// program optimization -> destruction -> slot assignment, plus the
// An-dependent pretty-printer. HEAVY (mjz_lib graphs + every pass); it is
// included by prog/program.hpp in the header-only build (constexpr) and
// by tu/prog.cpp in the multi-TU build — include it nowhere else.

#include "minijz/front/parser.hpp"
#include "minijz/ir/builder.hpp"
#include "minijz/ir/destruct.hpp"
#include "minijz/ir/stats.hpp"
#include "minijz/opt/analysis/an.hpp"
#include "minijz/opt/analysis/alias.hpp" // prog_handle_gates
#include "minijz/opt/driver.hpp"         // optimize_prog (callgraph.hpp)
#include "minijz/opt/interprocedural/tailrec.hpp" // opt_tailrec body
#include "minijz/opt/interprocedural/scctco.hpp"  // opt_scc_tailcall body
#include "minijz/back/stack_alloc.hpp"
#include "minijz/runtime/astrun.hpp" // assign_slots
#include "minijz/prog/printers.hpp"
#include "minijz/prog/program.hpp"

#ifndef MJZ_PIPELINE_FN
#ifdef MJZ_MULTI_CPP
#define MJZ_PIPELINE_FN
#else
#define MJZ_PIPELINE_FN constexpr
#endif
#endif

// ============================================================================
// 12. full pipeline driver
// ============================================================================
MJZ_PIPELINE_FN Program compile(std::string_view src, PassCfg cfg) {
  Program P{};
  P.P = parse(src);
  if (!P.P.ok) {
    P.error = P.P.err.what;
    P.errpos = P.P.err.pos;
    return P;
  }
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
    if (ssa0[0].nblocks() == 0) {
      P.error = "empty program";
      return P;
    }
    An A = analyze(ssa0[0]);
    if (!ssa_construct(ssa0[0], A, sst[0])) {
      P.error = "SSA construction failed (use before def?)";
      return P;
    }
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
  for (uint32_t k = 0; k < a.fns.size(); ++k)
    names[k + 1] = a.fns[k].name;
  std::vector<OptStats> os(nf);
  std::vector<uint32_t> src_index(nf);
  for (uint32_t k = 0; k < nf; ++k)
    src_index[k] = k;
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

// ---- the An-dependent pretty-printers (compiled once, with the analysis) --
void print_analysis(const An &A, const Fn &fn, const char *tag) {
  uint32_t nloops = 0;
  std::printf("  [%s] blocks=%u", tag, (unsigned)A.n);
  std::printf(" | loops:");
  bool first = true;
  for (uint32_t h = 0; h < A.n; ++h) {
    if (A.inner[h] != h)
      continue;
    auto row = A.loops.range()[h];
    ++nloops;
    std::printf("%s{hdr=b%u,depth=%u,size=%u}", first ? "" : ",", (unsigned)h,
                (unsigned)(A.nest[h] + 1),
                (unsigned)(row.size() ? (uint32_t)row.size() : 1));
    first = false;
  }
  if (first)
    std::printf(" none");
  std::printf("\n");
  (void)fn;
  (void)nloops;
}

// main's pre-optimization dominator/loop analysis, on demand (the old
// Program::an_pre_main member, recomputed instead of carried around, so
// Program itself stays light)
void print_pre_main_analysis(const Program &P) {
  An A = analyze(P.ssa_all[0]);
  print_analysis(A, P.ssa_all[0], "analysis (mjz graph)");
}

// (see optimize_api.hpp) — the An stays here
bool optimize_light(Fn &fn, OptStats &os, PassCfg cfg) {
  An an{};
  return optimize(fn, os, an, cfg);
}
