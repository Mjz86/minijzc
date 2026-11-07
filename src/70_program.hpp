#pragma once

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
// 13. pretty-printers (runtime only)
// ============================================================================
static std::string vname(const Fn &fn, uint32_t v) {
  if (v == NONE) return "_";
  char buf[64];
  uint32_t base = fn.base_of(v), ver = fn.vern_of(v);
  std::string n;
  if (base < fn.var_names.size() && !fn.var_names[base].empty()) {
    // named source variable: versions are per-base numbered -> unique
    n.assign(fn.var_names[base]);
    std::snprintf(buf, sizeof buf, "%s.%u", n.c_str(), (unsigned)ver);
  } else {
    // anonymous temp: raw vreg id (many temp bases share version numbers)
    std::snprintf(buf, sizeof buf, "t%u", (unsigned)v);
  }
  return std::string(buf);
}

static void print_fn(const Fn &fn, const char *title) {
  std::printf("  ---- %s ----\n", title);
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    if (fn.blocks[b].begin == fn.blocks[b].end &&
        fn.succ0(b) == NONE && fn.succ1(b) == NONE && b != 0)
      continue; // isolated dead block
    std::printf("  b%u:", b);
    if (!fn.preds.empty() && !fn.preds[b].empty()) {
      std::printf("              ; preds:");
      for (uint32_t p : fn.preds[b]) std::printf(" b%u", p);
    }
    std::printf("\n");
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      std::printf("    ");
      switch (I.k) {
      case K::Const:
        std::printf("%s = %lld", vname(fn, I.dst).c_str(),
                    (long long)I.c);
        break;
      case K::Copy:
        std::printf("%s = %s", vname(fn, I.dst).c_str(),
                    vname(fn, I.a).c_str());
        break;
      case K::Bin:
        std::printf("%s = %s %s %s", vname(fn, I.dst).c_str(),
                    vname(fn, I.a).c_str(), bname(I.b),
                    vname(fn, I.b2).c_str());
        break;
      case K::Br:
        std::printf("br b%u", I.t);
        break;
      case K::Cbr:
        std::printf("cbr %s ? b%u : b%u", vname(fn, I.a).c_str(), I.t, I.f);
        break;
      case K::Print:
        std::printf("print %s", vname(fn, I.a).c_str());
        break;
      case K::Phi: {
        std::printf("%s = phi ", vname(fn, I.dst).c_str());
        if (I.a != NONE)
          std::printf("[%s from b%u]", vname(fn, I.a).c_str(), I.t);
        if (I.b2 != NONE)
          std::printf("%s[%s from b%u]", I.a != NONE ? " " : "",
                      vname(fn, I.b2).c_str(), I.f);
        break;
      }
      case K::Exit:
        std::printf("exit");
        break;
      case K::Alloc:
        std::printf("%s = alloc %s", vname(fn, I.dst).c_str(),
                    vname(fn, I.a).c_str());
        break;
      case K::Load:
        std::printf("%s = %s[%s]", vname(fn, I.dst).c_str(),
                    vname(fn, I.a).c_str(), vname(fn, I.b2).c_str());
        break;
      case K::Store:
        std::printf("%s[%s] = %s", vname(fn, I.a).c_str(),
                    vname(fn, I.b2).c_str(), vname(fn, I.c2).c_str());
        break;
      case K::Len:
        std::printf("%s = len %s", vname(fn, I.dst).c_str(),
                    vname(fn, I.a).c_str());
        break;
      case K::Input:
        std::printf("%s = input()", vname(fn, I.dst).c_str());
        break;
      case K::Depth:
        std::printf("%s = depth()", vname(fn, I.dst).c_str());
        break;
      case K::Call: {
        std::printf("%s = f%u(", vname(fn, I.dst).c_str(), (unsigned)I.c);
        for (size_t k = 0; k < I.args.size(); ++k)
          std::printf("%s%s", k ? ", " : "", vname(fn, I.args[k]).c_str());
        std::printf(")");
        break;
      }
      case K::Ret:
        if (I.a == NONE) std::printf("ret 0");
        else std::printf("ret %s", vname(fn, I.a).c_str());
        break;
      }
      std::printf("\n");
    }
  }
}

static void print_analysis(const An &A, const Fn &fn, const char *tag) {
  uint32_t nloops = 0;
  std::printf("  [%s] blocks=%u", tag, (unsigned)A.n);
  std::printf(" | loops:");
  bool first = true;
  for (uint32_t h = 0; h < A.n; ++h) {
    if (A.inner[h] != h) continue;
    auto row = A.loops.range()[h];
    ++nloops;
    std::printf("%s{hdr=b%u,depth=%u,size=%u}", first ? "" : ",",
                (unsigned)h, (unsigned)(A.nest[h] + 1),
                (unsigned)(row.size() ? (uint32_t)row.size() : 1));
    first = false;
  }
  if (first) std::printf(" none");
  std::printf("\n");
  (void)fn;
  (void)nloops;
}

static void print_run(const RunOut &R) {
  if (R.trapped) { std::printf("TRAP\n"); return; }
  if (R.timeout) { std::printf("TIMEOUT\n"); return; }
  for (size_t i = 0; i < R.out.size(); ++i)
    std::printf("%s%lld", i ? " " : "", (long long)R.out[i]);
  if (R.out.empty()) std::printf("(no output)");
  std::printf("\n");
}

constexpr bool same_run(const RunOut &a, const RunOut &b) {
  return a.trapped == b.trapped && a.timeout == b.timeout && a.out == b.out;
}

// ============================================================================
// 13.5 BACKENDS (runtime only): C, x86-64 asm (GAS), bytecode listing,
//      and the native build&run harness used by --run.
//      All backends consume the DESTROYED IR (no phis).
//      Output text contract shared by every runner (incl. interpreters):
//        one "%lld\n" line per print; "TRAP\n" appended if a div/mod traps;
//        "TIMEOUT\n" if a step-limited runner hit its limit.
// ============================================================================

// canonical text of an in-process RunOut — comparable with native stdout
static std::string text_of(const RunOut &R) {
  std::string s{};
  char buf[32];
  for (val_t v : R.out) {
    std::snprintf(buf, sizeof buf, "%lld\n", (long long)v);
    s += buf;
  }
  if (R.trapped) s += "TRAP\n";
  else if (R.timeout) s += "TIMEOUT\n";
  return s;
}