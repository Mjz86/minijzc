// main.cpp (multi-cpp mode): thin entry. The pipeline is compiled in the
// sibling TUs (front_ir/analysis/scalar/loop_linear/interproc/back_prog/
// apps) and linked here.
#include "minijz/apps/cli.hpp"
#include "minijz/apps/shrink.hpp"

int main(int argc, char **argv) {
  Cli C{};
  if (!parse_cli(argc, argv, C)) return 2;
  if (C.help) {
    usage();
    return 0;
  }
  bool have_action = C.demo || C.selftest || C.fuzz > 0 ||
                     !C.shrink_file.empty();
  std::string src{};
  std::string stem = "stdin";
  bool src_from_stdin = false; // stdin already consumed as the PROGRAM
  if (!C.file.empty()) {
    src = slurp_file(C.file);
    if (src.empty()) {
      std::printf("cannot read '%s'\n", C.file.c_str());
      return 1;
    }
    size_t slash = C.file.find_last_of('/');
    std::string base =
        slash == std::string::npos ? C.file : C.file.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    stem = dot == std::string::npos ? base : base.substr(0, dot);
    if (C.out_dir == ".") {
      C.out_dir = slash == std::string::npos ? "." : C.file.substr(0, slash);
    }
  } else if (!C.inline_src.empty()) {
    src = C.inline_src;
    stem = "inline";
  } else if (!have_action) {
#ifdef MJZ_HAVE_ISATTY
    if (isatty(0)) {
      std::printf("=== MiniJZ on mjz_lib (constexpr pipeline passed: see "
                  "static_assert) ===\n");
      repl();
      return 0;
    }
#endif
    src = slurp_file("/dev/stdin");
    src_from_stdin = true;
  }

  if (C.demo || C.selftest) {
    std::printf("=== MiniJZ: a basic compiler on mjz_lib graph algorithms "
                "===\n");
#ifdef MJZ_CX_ASSERT
    std::printf("(the constexpr pipeline already passed: see "
                "static_assert)\n\n");
#else
    CHECK(cx_pipeline(),
          "the full pipeline (incl. destruction + VM) must work");
    std::printf("(pipeline self-check passed)\n\n");
#endif
    run_demos();
    std::printf("\n");
  }
  if (!C.shrink_file.empty()) {
    int rc = run_shrink(C, C.shrink_file);
    if (rc != 0) ++g_fails;
    ++g_checks;
  }
  if (C.fuzz > 0 || C.selftest) {
    if (C.selftest && C.fuzz_native <= 0) C.fuzz_native = 6; // native gate
    long jobs = C.fuzz_jobs > 0 ? C.fuzz_jobs : default_fuzz_jobs(C.selftest);
    run_fuzz(C.selftest ? 200 : C.fuzz, C.cc, C.fuzz_native, C.out_dir,
             C.fuzz_from, C.fuzz_random, C.mjz_fuzz_dumps, jobs);
  }
  if (have_action) {
    std::printf("\ntotal failures: %d (of %d checks)\n", g_fails, g_checks);
    if (!src.empty()) std::printf("\n");
  }

  if (!src.empty()) {
    // ---- the input() tape ------------------------------------------------
    // --stdin=FILE wins; otherwise, if the program reads input() and stdin
    // still has data (program came from a file/-e, stdin is a pipe), the
    // piped bytes become the tape. Native builds get the tape verbatim.
    if (!C.stdin_path.empty()) {
      std::ifstream f(C.stdin_path, std::ios::binary);
      if (!f) {
        std::printf("cannot read --stdin file '%s'\n", C.stdin_path.c_str());
        return 1;
      }
      C.stdin_text =
          std::string((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
      C.inputs = parse_inputs(C.stdin_text);
    } else if (!C.no_exec || C.do_run) {
      // probe only when something will actually execute in-process
      PassCfg pc_probe{};
      pc_probe.do_opt = false;
      pc_probe.regalloc = false;
      Program P0 = compile(src, pc_probe); // cheap probe: uses input()?
      if (P0.ok && P0.uses_input) {
#ifdef MJZ_HAVE_ISATTY
        if (!src_from_stdin && !isatty(0)) {
          C.stdin_text = slurp_file("/dev/stdin");
          C.inputs = parse_inputs(C.stdin_text);
        } else {
          std::printf("(note: input() reads return 0 — pass --stdin=FILE or "
                      "pipe data on stdin)\n");
        }
#else
        if (!src_from_stdin) {
          C.stdin_text = slurp_file("/dev/stdin");
          C.inputs = parse_inputs(C.stdin_text);
        }
#endif
      }
    }
    Program P{};
    RunOut R = compile_report(P, C, src, false);
    if (!P.ok) return 1;
    if (C.debug_plan) { // dump the emission plan decisions per instr
      for (size_t fi = 0; fi < P.fns.size(); ++fi) {
        const Fn &F = P.fns[fi].destroyed;
        EmitPlan PL = make_emit_plan(F);
        std::printf("== fn %zu [%s] ==\norder:", fi,
                    std::string(P.fn_names[fi]).c_str());
        for (uint32_t b2 : PL.order)
          std::printf(" b%u(nxt=b%u)", b2,
                      PL.next[b2] == ~uint32_t(0) ? 999u : PL.next[b2]);
        std::printf("\n");
        for (uint32_t b = 0; b < F.nblocks(); ++b)
          for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
            const Instr &I = F.code[i];
            std::printf("i%u b%u k=%d dst=%u dead=%d kc=%d uses=%u\n", i, b,
                        (int)I.k, I.dst, PL.dead[i] ? 1 : 0,
                        (I.dst < PL.kc.size()) ? PL.kc[I.dst] : -1,
                        (I.dst < PL.uses.size()) ? PL.uses[I.dst] : 0u);
          }
      }
    }
    if (C.check_all) { // all five runners, like the fuzzer (debug aid)
      RunOut oracle = run_ast(P.P.ast, P.P.root, C.steps, C.inputs);
      RunOut plain = run_prog(fns_of(P, 0), C.steps, C.inputs);
      RunOut lowered = run_prog(fns_of(P, 2), C.steps, C.inputs);
      std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
      RunOut vm = vm_run(bc, C.steps, C.inputs);
      const char *names[] = {"oracle", "ssa", "lower", "vm"};
      RunOut vals[4] = {oracle, plain, lowered, vm};
      for (int k = 0; k < 4; ++k) {
        std::printf("%-8s: ", names[k]);
        print_run(vals[k]);
      }
      CHECK(same_run(oracle, plain) && same_run(oracle, lowered) &&
                same_run(oracle, vm),
            "--check-all: runners must agree");
    }
    if (C.emit_bc || C.do_run) {
      std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
      std::string bc_path = C.out_dir + "/" + stem + ".mbc";
      write_file_bytes(bc_path, bc);
      std::string lst = emit_bc_listing(bc);
      write_file(bc_path + ".lst", lst);
      if (C.print_bc) std::printf("%s", lst.c_str());
      if (!C.quiet && C.emit_bc)
        std::printf("wrote %s (+ .lst)\n", bc_path.c_str());
    }
    if (C.do_run) {
      run_matrix(P, C, stem);
    } else {
      if (C.emit_c || C.emit_exe) { // exe builds from the emitted C
        std::string p = C.out_dir + "/" + stem + ".c";
        write_file(p, emit_c(fns_of(P, 2), P.fn_names));
        if (!C.quiet && C.emit_c) std::printf("wrote %s\n", p.c_str());
      }
      if (C.emit_asm) {
        std::string p = C.out_dir + "/" + stem + ".s";
        write_file(p, emit_x64(fns_of(P, 2), P.fn_names));
        if (!C.quiet) std::printf("wrote %s\n", p.c_str());
      }
      if (C.emit_exe) {
        std::string cc = C.cc.empty() ? find_cc() : C.cc;
        if (cc.empty()) {
          std::printf("no native compiler found (tried clang++, g++, cc); "
                      "exe not built\n");
        } else {
          std::string exe = C.exe_name.empty() ? C.out_dir + "/" + stem + ".exe"
                                               : C.exe_name;
          std::string lvl = C.native_O.empty() ? "0" : C.native_O.front();
          std::string c_path = C.out_dir + "/" + stem + ".c";
          std::string cmd =
              cc + " -w -O" + lvl + " " + c_path + " -o " + exe;
          if (std::system(cmd.c_str()) == 0) {
            if (!C.quiet)
              std::printf("built %s (via: %s)\n", exe.c_str(), cmd.c_str());
          } else {
            std::printf("BUILD FAILED: %s\n", cmd.c_str());
            return 1;
          }
        }
      }
    }
    (void)R;
    return 0;
  }
  if (!have_action) {
    usage();
    return 0;
  }
  return g_fails ? 1 : 0;
}
// cache probe 1793991399152465055

