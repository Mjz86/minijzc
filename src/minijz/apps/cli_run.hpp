#pragma once
// === cli_run.hpp ========================================
// the CLI's heavy half: compile_report (compile + full report), the --run
// backend matrix, and the interactive statement repl. Compiled in exactly
// ONE TU per binary (tu/cli_app.cpp in the multi-TU build; root main.cpp
// in the header-only build) — the entry functions here are externals.

#include "cli_parse.hpp"
#include "app_api.hpp"
#include "../prog/program.hpp"
#include "../prog/runners.hpp"
#include "../runtime/astrun.hpp"
#include "../back/bytecode.hpp"
#include "../back/emit_plan.hpp"
#include "../base/native_run.hpp"
#include "../opt/optimize_api.hpp" // optimize_light (--opt-diff snapshots)
#include "../front/diag.hpp"       // located diagnostics

// ---- 17b. --shrink=FILE: delta-debug a failing program ---------------------

// the per-file compile report + in-process runs; returns the optimized RunOut
RunOut compile_report(Program &P, const Cli &C, const std::string &src,
                      bool brief) {
  P = compile(src, pass_cfg_of(C));
  if (!P.ok) {
    print_compile_error(src, P.error, P.errpos, C.file.c_str());
    return {};
  }
  if (!brief && !C.quiet) {
    OptStats os = sum_ostats(P);
    size_t ssa_i = 0, opt_i = 0, dst_i = 0;
    uint32_t phis = 0, de_c = 0, dz_c = 0, dse = 0, dcy = 0, dfu = 0;
    for (const FnUnit &U : P.fns) {
      ssa_i += U.ssa.code.size();
      opt_i += U.opt.code.size();
      dst_i += U.destroyed.code.size();
      phis += U.sstats.phis;
      de_c += U.dstats.edges_split;
      dz_c += U.dstats.copies;
      dse += U.dstats.self_copies;
      dcy += U.dstats.cycles_broken;
      dfu += U.dstats.fused;
    }
    print_pre_main_analysis(P);
    std::printf(
        "  fns: %zu | ssa: %zu instrs, %u phis | opt: %zu | destroyed: %zu\n"
        "  opt: iters %u | folds %u | copies %u | copyprop %u | cse %u | "
        "canon %u+%u | brs %u | cbr== %u | dead-blk %u | splice %u | "
        "phi-degen %u | dce %u | hoist %u | INLINED %u | UNROLLED %u | "
        "REASSOC %u(-%u) | LCF %u+%u+g%u|rt%u | DIVMAGIC %u | NOTCOMB %u | "
        "TRAP-KILL %u | MEM2REG %u | DSE %u | SCC-TCO %u(+%u) | "
        "SCCP %u(+%ubr) | LD-FWD %u | LD-CSE %u | RANGE-CMP %u | "
        "CMP-FACT %u | CALL-CSE %u | DEAD-ARGS %u | FNSPEC %u | ICF %u | "
        "JT %u | ARG-PROMO %u | GVN %u\n"
        "  destroy: split-edges %u | copies %u | self %u | cycles %u | "
        "copy-fused %u\n",
        P.fns.size(), ssa_i, (unsigned)phis, opt_i, dst_i, (unsigned)os.iters,
        (unsigned)os.folds, (unsigned)os.copies, (unsigned)os.copyprops,
        (unsigned)os.cses, (unsigned)os.canon_swaps, (unsigned)os.canon_cmps,
        (unsigned)os.brs_folded, (unsigned)os.cbr_same,
        (unsigned)os.blocks_gone, (unsigned)os.splices, (unsigned)os.phi_degens,
        (unsigned)os.dce, (unsigned)os.hoisted, (unsigned)os.inlines,
        (unsigned)os.unrolls, (unsigned)os.reassocs, (unsigned)os.reassoc_gone,
        (unsigned)os.lcf_loops, (unsigned)os.lcf_sims, (unsigned)os.lcf_gensims,
        (unsigned)os.lcf_rt, (unsigned)os.divmagics, (unsigned)os.notcombs,
        (unsigned)os.traps_killed, (unsigned)os.mem2reg_lifts,
        (unsigned)os.mem2reg_dses, (unsigned)os.scctco,
        (unsigned)os.scctco_folds, (unsigned)os.sccp_consts,
        (unsigned)os.sccp_brs, (unsigned)os.ld_fwds, (unsigned)os.ld_cses,
        (unsigned)os.rangecmps, (unsigned)os.cmpfacts, (unsigned)os.callcses,
        (unsigned)os.deadargs, (unsigned)os.fnspecs, (unsigned)os.icf,
        (unsigned)os.jthreads, (unsigned)os.argpromos, (unsigned)os.gvns,
        (unsigned)de_c, (unsigned)dz_c, (unsigned)dse, (unsigned)dcy,
        (unsigned)dfu);
  }
  if (C.print_ir) {
    for (size_t f = 0; f < P.fns.size(); ++f) {
      char t1[96], t2[96], t3[96];
      std::snprintf(t1, sizeof t1, "SSA (before opt) [%s]",
                    std::string(P.fn_names[f]).c_str());
      std::snprintf(t2, sizeof t2, "SSA (after opt) [%s]",
                    std::string(P.fn_names[f]).c_str());
      std::snprintf(t3, sizeof t3, "destroyed [%s]",
                    std::string(P.fn_names[f]).c_str());
      print_fn(P.fns[f].ssa, t1, P.src);
      print_fn(P.fns[f].opt, t2, P.src);
      print_fn(P.fns[f].destroyed, t3, P.src);
    }
  }
  if (C.opt_diff_fn >= 0) {
    // the batch pass-diff viewer (same snapshot machinery as the
    // debugger's `opt` explorer, minus the session)
    long f = C.opt_diff_fn;
    if (f >= (long)P.fns.size())
      f = 0;
    struct CliSnap {
      std::string name;
      bool changed;
      std::string ir;
      uint32_t instrs, blocks;
    };
    std::vector<CliSnap> snaps{};
    Fn live = P.fns[(size_t)f].ssa;
    struct Coll {
      std::vector<CliSnap> *out;
      std::string_view src;
    } coll{&snaps, P.src};
    {
      CliSnap base{};
      base.name = "baseline";
      base.changed = false;
      base.ir = fn_ir_str(live, "baseline (pre-opt SSA)", P.src);
      base.instrs = (uint32_t)live.code.size();
      base.blocks = live.nblocks();
      snaps.push_back(std::move(base));
    }
    PassCfg cfg = pass_cfg_of(C);
    cfg.debug = 0;
    cfg.dbg_name = P.fn_names[(size_t)f];
    cfg.snap_hook = [](const char *pass, bool changed, const Fn &fn,
                       void *ctx) {
      Coll *c = (Coll *)ctx;
      CliSnap s{};
      s.name = pass;
      s.changed = changed;
      char title[64];
      std::snprintf(title, sizeof title, "after %s", pass);
      s.ir = fn_ir_str(fn, title, c->src);
      s.instrs = (uint32_t)fn.code.size();
      s.blocks = fn.nblocks();
      c->out->push_back(std::move(s));
    };
    cfg.snap_ctx = &coll;
    OptStats os{};
    (void)optimize_light(live, os, cfg);
    std::printf("== --opt-diff: fn %ld [%s] ==\n", f,
                std::string(P.fn_names[(size_t)f]).c_str());
    for (size_t k = 0; k < snaps.size(); ++k) {
      const CliSnap &s = snaps[k];
      std::printf("  #%-3zu %-10s %-9s %u instrs, %u blocks\n", k,
                  s.name.c_str(), s.changed ? "changed" : "fixed",
                  (unsigned)s.instrs, (unsigned)s.blocks);
      if (k > 0 && s.changed) {
        std::string d = unified_diff(snaps[k - 1].ir, s.ir);
        if (!d.empty())
          std::printf("%s", d.c_str());
      }
    }
    std::printf("(interprocedural passes run program-wide; not snapshotted)\n");
  }
  if (C.no_exec && !C.do_run)
    return {}; // compile/emit only
  RunOut R = run_prog(fns_of(P, 1), C.steps, C.inputs);
  RunOut Ro = run_ast(P.P.ast, P.P.root, C.steps, C.inputs);
  if (!C.quiet || brief) {
    std::printf("oracle   : ");
    print_run(Ro, P.src);
    std::printf("compiled : ");
    print_run(R, P.src);
    if (!same_run(Ro, R))
      std::printf("*** MISMATCH between oracle and compiled output ***\n");
  }
  return R;
}

// the --run matrix: in-process runners + native builds, match check, timings
static void run_matrix(Program &P, const Cli &C, const std::string &stem) {
  std::string cap = C.out_dir + "/" + stem + ".out.tmp";
  // --- in-process runners (reference results & timings)
  double t0 = now_ms();
  RunOut ref = run_prog(fns_of(P, 1), C.steps, C.inputs);
  double t_interp = now_ms() - t0;
  std::string ref_text = text_of(ref);
  t0 = now_ms();
  std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
  RunOut vm = vm_run(bc, C.steps, C.inputs);
  double t_vm = now_ms() - t0;
  bool vm_ok = (text_of(vm) == ref_text);
  if (ref.timeout) {
    std::printf("interpreter hit the step limit; native runs skipped "
                "(an infinite loop would hang)\n");
    return;
  }
  // --- write the native sources (always, since we are about to build them)
  std::string c_path = C.out_dir + "/" + stem + ".c";
  std::string s_path = C.out_dir + "/" + stem + ".s";
  if (!write_file(c_path, emit_c(fns_of(P, 2), P.fn_names, P.src)) ||
      !write_file(s_path, emit_x64(fns_of(P, 2), P.fn_names, P.src))) {
    std::printf("error: cannot write %s / %s\n", c_path.c_str(),
                s_path.c_str());
    return;
  }
  std::string cc = C.cc.empty() ? find_cc() : C.cc;
  if (cc.empty()) {
    std::printf("no native compiler found (tried clang++, g++, cc); "
                "in-process results only\n");
    std::printf("  %-14s %10.3f ms  %s\n", "interp(ssa)", t_interp,
                "reference");
    std::printf("  %-14s %10.3f ms  %s\n", "vm(bytecode)", t_vm,
                vm_ok ? "match" : "MISMATCH");
    return;
  }
  if (!C.quiet)
    std::printf("native driver: %s (autodetected: clang++ preferred)\n",
                cc.c_str());
  // the native binaries read the SAME input() tape the in-process runners
  // saw (verbatim bytes); with no tape they read the null device (EOF -> 0)
  std::string in_path = mjz_devnull();
  if (!C.stdin_text.empty()) {
    std::string p = C.out_dir + "/" + stem + ".in.tmp";
    if (write_file(p, C.stdin_text))
      in_path = p;
  }
  std::printf("  %-18s %10s  %s\n", "backend", "time(ms)", "output");
  std::printf("  %-18s %10.3f  %s\n", "interp (SSA opt)", t_interp,
              "reference");
  std::printf("  %-18s %10.3f  %s\n", "vm (bytecode)", t_vm,
              vm_ok ? "match" : "MISMATCH");
  for (const std::string &lvl : C.native_O) {
    struct Job {
      const char *tag;
      std::string src_path;
      bool is_asm;
    };
    Job jobs[2] = {{"c", c_path, false}, {"asm", s_path, true}};
    for (Job &J : jobs) {
      std::string exe =
          C.out_dir + "/" + stem + "." + J.tag + ".O" + lvl + ".exe";
      std::string build = mjz_timeout(60, cc + " -w -O" + lvl + " " +
                                              J.src_path + " -o " + exe);
      if (std::system(build.c_str()) != 0) {
        std::printf("  %-18s %10s  BUILD FAILED (%s -O%s)\n", J.tag, "-",
                    cc.c_str(), lvl.c_str());
        continue;
      }
      std::string out_text{};
      double best = 1e300;
      int rc = 0;
      for (unsigned r = 0; r < (C.repeats ? C.repeats : 1); ++r) {
        t0 = now_ms();
        rc = run_capture(mjz_timeout(20, exe), out_text, cap, in_path);
        double dt = now_ms() - t0;
        if (dt < best)
          best = dt;
      }
      bool timed_out = (rc != 0 && (((unsigned)rc >> 8) == 124 ||
                                    ((unsigned)rc >> 8) == 137));
      if (timed_out)
        out_text += "TIMEOUT\n";
      bool match = (out_text == ref_text);
      std::string name = std::string(J.tag) + " -O" + lvl;
      std::printf("  %-18s %10.3f  %s\n", name.c_str(), best,
                  match ? "match" : "MISMATCH");
    }
  }
}

static void repl() {
  char line[4096];
  std::string buf{};
  std::printf("MiniJZ interactive — statements end with ';'; a blank line "
              "compiles+runs the buffer as an INDEPENDENT program "
              "(variables do not carry over); :r resets; :q quits\n");
  for (;;) {
    std::printf("minijz> ");
    std::fflush(stdout);
    if (!std::fgets(line, sizeof line, stdin))
      break;
    std::string t(line);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' ||
                          t.back() == ' ' || t.back() == '\t'))
      t.pop_back();
    if (t == ":q")
      break;
    if (t == ":r") {
      buf.clear();
      std::printf("(buffer reset)\n");
      continue;
    }
    if (t.empty()) {
      if (buf.empty())
        continue;
      Cli C{};
      C.steps = 10000000;
      Program P{};
      (void)compile_report(P, C, buf, true);
      buf.clear();
      continue;
    }
    buf += t;
    buf += "\n";
  }
  std::printf("\n(bye)\n");
}
