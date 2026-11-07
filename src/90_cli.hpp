#pragma once

// ============================================================================
// 17. CLI: input commands, emission, native build&run, comparison matrix
// ============================================================================
struct Cli {
  std::string file{};
  std::string inline_src{};
  std::string cc{};          // "" = auto-detect
  std::string out_dir{"."};
  std::string stdin_path{};  // --stdin=FILE (input() tape)
  std::string stdin_text{};  // raw text of the tape (fed to native builds)
  std::vector<val_t> inputs{}; // parsed tape (fed to in-process runners)
  std::vector<std::string> native_O{"0"};
  std::string exe_name{};    // -o NAME: name of the --emit=exe executable
  bool emit_c{false}, emit_asm{false}, emit_bc{false}, emit_exe{false};
  bool do_run{false}, no_opt{false}, print_ir{false}, print_bc{false};
  bool no_inline{false};
  bool no_scctco{false};
  bool no_reassoc{false}, no_lcf{false}, no_divmagic{false};
  int opt_level{-1};  // -O0..-O3 preset (-1: default, i.e. 3)
  unsigned debug{0};  // --debug=N: pass-manager dump volume (0..3)
  unsigned fp_limit{24}; // --fp-limit=N: fixpoint round cap (0 = default)
  bool regalloc{true};   // --no-regalloc: identity slots ("SSA-like" C)
  std::vector<std::string> enable_passes{}, disable_passes{};
  unsigned unroll{16}; // max constant trip count to fully unroll (0 = off)
  bool no_exec{false}, quiet{false}, check_all{false}, debug_plan{false};
  unsigned repeats{3};
  uint64_t steps{200000000ull};
  long fuzz{0}, fuzz_native{0}, fuzz_from{1};
  bool fuzz_random{false};      // --fuzz-cfg=random: random pass configs
  std::string shrink_file{};    // --shrink=FILE: delta-debug a failing program
  bool demo{false}, selftest{false}, help{false};
};

static void usage() {
  std::printf(
      "minijz — a small compiler on mjz_lib graph algorithms\n"
      "\n"
      "usage: minijz [options] [file.mjz]\n"
      "  (no file, tty)   interactive: type statements, blank line runs, :q "
      "quits\n"
      "  (no file, pipe)  the program is read from stdin\n"
      "  -e \"<source>\"    compile the inline source instead of a file\n"
      "\n"
      "actions:\n"
      "  --demo            curated demo suite\n"
      "  --fuzz[=N]        differential fuzz of N random programs (default "
      "1000)\n"
      "  --fuzz-from=S     first fuzz seed (default 1; lets long sweeps run "
      "in shards)\n"
      "  --fuzz-native[=K] additionally build & run C+asm at -O0/-O2 for the\n"
      "                    first K fuzz programs (slow; proves the emitted\n"
      "                    code byte-for-byte against the oracle)\n"
      "  --fuzz-cfg=random fuzz with an extra random pass configuration per\n"
      "                    seed (1..4 passes disabled, random tier): exposes\n"
      "                    pass-interaction bugs the fixed rotation misses;\n"
      "                    failures are auto-saved as fuzz_fail_<seed>.mjz/\n"
      "                    .flags/.in\n"
      "  --shrink=FILE    delta-debug a failing program: greedily remove\n"
      "                    lines while the differential (all runners + native\n"
      "                    parity) still fails; writes FILE.small.mjz. Pair\n"
      "                    with the .flags/.in the fuzzer saved\n"
      "  --selftest        demos + fuzz(200)\n"
      "\n"
      "compile & run:\n"
      "  --run             emit, build and run ALL backends, verify identical "
      "output,\n"
      "                    print a timing table (uses the native compiler "
      "below)\n"
      "  --emit=c,asm,bc   write backend files (.c / .s / .mbc + .mbc.lst)\n"
      "  --emit=exe         also build an executable with the native\n"
      "                    compiler (uses --cc and the first --native-O\n"
      "                    level; -o NAME renames it)\n"
      "  -o NAME           output executable name for --emit=exe\n"
      "  --no-exec         compile/emit only: do not run the program\n"
      "                    (no oracle/compiled lines, stdin untouched)\n"
      "  --stdin=FILE      whitespace-separated integers fed to input():\n"
      "                    parsed for the in-process runners AND piped verbatim\n"
      "                    into the native binaries (default: real stdin if it\n"
      "                    is not a terminal; reads return 0 at EOF)\n"
      "  --native-O=L      optimization level(s) for the native compiler,\n"
      "                    comma list (e.g. 0,2); default 0 — measure OUR "
      "codegen,\n"
      "                    not the native compiler's improvements\n"
      "  --cc=CMD          native compiler driver (default: clang++ if "
      "present, else g++)\n"
      "  --repeats=N       timing repeats for --run (default 3, min taken)\n"
      "  --steps=N         interpreter/VM step limit (default 200000000)\n"
      "\n"
      "optimizer (pass manager):\n"
      "  -O0 .. -O3        preset (default -O3). -O0: nothing (SSA +\n"
      "                    destruction only); -O1: cheap tier to fixpoint\n"
      "                    (fold/copyprop/canon/cse/unreach/splice/dce);\n"
      "                    -O2: + mem2reg, trapelim (range+alias), licm,\n"
      "                    divmagic, reassoc, ifconv; -O3: + unroll, lcf,\n"
      "                    constcall, tailrec, cdfold, inline (expensive CFG\n"
      "                    restructuring runs only after the cheaper tiers\n"
      "                    reach their fixpoint; constcall evaluates pure\n"
      "                    constant-argument calls — recursive ones included\n"
      "                    — at compile time; tailrec eliminates inner-SCC\n"
      "                    tail calls — mutual recursion like is_even/is_odd\n"
      "                    merges into one sel-dispatched function with an\n"
      "                    exact recursion-budget loop; cdfold then replaces\n"
      "                    such countdown loops by closed forms, so\n"
      "                    print(is_odd(input())) becomes\n"
      "                    [depth guard] + print(input() & 1))\n"
      "  --enable=P,P2      force passes on (folds, copyprop, canon, cse,\n"
      "  --disable=P,P2     force passes off  unreach, splice, dce, mem2reg,\n"
      "                    trapelim, licm, divmagic, reassoc, ifconv, unroll,\n"
      "                    lcf, constcall, scctco, tailrec, cdfold, inline) —\n"
      "                    applied on top of the -O preset, so e.g. `-O1\n"
      "                    --enable=lcf` is legal\n"
      "  --unroll=N         full-unroll loops with constant trip count <= N\n"
      "                    (default 16; 0 disables unrolling)\n"
      "  --fp-limit=N       fixpoint round cap per tier (default 24)\n"
      "  --debug=N          pass-manager dumps: 1 = one line per pass;\n"
      "                    2 = + the IR after each pass that changed;\n"
      "                    3 = + per-fixpoint-round headers\n"
      "  --no-regalloc      keep one named local per vreg (SSA-like C, one\n"
      "                    VM slot per vreg) instead of interference-colored\n"
      "                    shared slots (register-like C: r0..r{K-1})\n"
      "  --no-opt           skip the optimizer entirely (same as -O0)\n"
      "  --no-inline        skip interprocedural inlining (call-graph SCC\n"
      "                    analysis still runs; recursive fns never inline)\n"
      "  --no-scctco        skip SCC tail-call optimization (inner-SCC tail\n"
      "                    calls collapse to loops; mutual recursion like\n"
      "                    is_even/is_odd folds to `n & 1`)\n"
      "  --no-reassoc       skip linear-combination reassociation\n"
      "  --no-lcf           skip closed-form loop evaluation (compile-time\n"
      "                    arithmetic/geometric series + simulation, AND the\n"
      "                    guarded runtime-trip-count closed form)\n"
      "  --no-divmagic      skip div/mod-by-constant -> mulhi+shift lowering\n"
      "  --print-ir        dump SSA (pre/post opt) and destroyed IR\n"
      "  --print-bc        print the bytecode listing\n"
      "  --quiet           no reports, just results\n"
      "  --help            this text\n");
}

static void split_commas(const std::string &s, std::vector<std::string> &out) {
  std::string cur{};
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
}

static bool parse_cli(int argc, char **argv, Cli &C) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char *pre) -> const char * {
      size_t n = std::strlen(pre);
      if (a.compare(0, n, pre) != 0) return nullptr;
      const char *v = a.c_str() + n;
      if (!*v) return nullptr;
      return v;
    };
    if (a == "--help" || a == "-h") C.help = true;
    else if (a == "--demo") C.demo = true;
    else if (a == "--selftest") C.selftest = true;
    else if (a == "--fuzz") C.fuzz = 1000;
    else if (const char *v = val("--fuzz=")) C.fuzz = atol(v);
    else if (const char *v = val("--fuzz-from=")) {
      C.fuzz_from = atol(v);
      if (C.fuzz_from < 1) C.fuzz_from = 1;
    }
    else if (a == "--fuzz-native") C.fuzz_native = 20;
    else if (const char *v = val("--fuzz-native=")) C.fuzz_native = atol(v);
    else if (a == "--fuzz-cfg=random" || a == "--fuzz-random")
      C.fuzz_random = true;
    else if (const char *v = val("--shrink=")) C.shrink_file = v;
    else if (a == "--run") C.do_run = true;
    else if (a == "--no-exec" || a == "--compile-only") C.no_exec = true;
    else if (a == "--check-all") C.check_all = true;
    else if (a == "--debug-plan") C.debug_plan = true;
    else if (a == "-O0") C.opt_level = 0;
    else if (a == "-O1") C.opt_level = 1;
    else if (a == "-O2") C.opt_level = 2;
    else if (a == "-O3") C.opt_level = 3;
    else if (const char *v = val("--opt=")) {
      int lv = atoi(v);
      if (lv < 0 || lv > 3) {
        std::printf("--opt=%s: level must be 0..3\n", v);
        return false;
      }
      C.opt_level = lv;
    } else if (const char *v = val("--enable=")) {
      split_commas(v, C.enable_passes);
    } else if (const char *v = val("--disable=")) {
      split_commas(v, C.disable_passes);
    } else if (const char *v = val("--debug=")) C.debug = (unsigned)atol(v);
    else if (const char *v = val("--fp-limit="))
      C.fp_limit = (unsigned)atol(v);
    else if (a == "--no-regalloc") C.regalloc = false;
    else if (a == "--no-opt") C.no_opt = true;
    else if (a == "--no-inline") C.no_inline = true;
    else if (a == "--no-scctco") C.no_scctco = true;
    else if (a == "--no-reassoc") C.no_reassoc = true;
    else if (a == "--no-lcf") C.no_lcf = true;
    else if (a == "--no-divmagic") C.no_divmagic = true;
    else if (const char *v = val("--unroll=")) C.unroll = (unsigned)atol(v);
    else if (a == "--print-ir") C.print_ir = true;
    else if (a == "--print-bc") C.print_bc = true;
    else if (a == "--quiet") C.quiet = true;
    else if (const char *v = val("--repeats=")) C.repeats = (unsigned)atol(v);
    else if (const char *v = val("--steps=")) C.steps = strtoull(v, nullptr, 10);
    else if (const char *v = val("--cc=")) C.cc = v;
    else if (const char *v = val("--stdin=")) C.stdin_path = v;
    else if (const char *v = val("--out-dir=")) C.out_dir = v;
    else if (const char *v = val("--native-O=")) {
      C.native_O.clear();
      split_commas(v, C.native_O);
    } else if (const char *v = val("--emit=")) {
      std::vector<std::string> parts{};
      split_commas(v, parts);
      for (auto &p : parts) {
        if (p == "c") C.emit_c = true;
        else if (p == "asm") C.emit_asm = true;
        else if (p == "bc") C.emit_bc = true;
        else if (p == "exe") C.emit_exe = true;
        else {
          std::printf("unknown --emit target '%s' (c, asm, bc, exe)\n", p.c_str());
          return false;
        }
      }
    } else if (a == "-e") {
      if (i + 1 >= argc) {
        std::printf("-e needs an argument\n");
        return false;
      }
      C.inline_src = argv[++i];
    } else if (a == "-o") {
      if (i + 1 >= argc) {
        std::printf("-o needs an argument\n");
        return false;
      }
      C.exe_name = argv[++i];
    } else if (!a.empty() && a[0] == '-') {
      std::printf("unknown option '%s' (try --help)\n", a.c_str());
      return false;
    } else {
      if (!C.file.empty()) {
        std::printf("multiple input files given ('%s' and '%s')\n",
                    C.file.c_str(), a.c_str());
        return false;
      }
      C.file = a;
    }
  }
  return true;
}

// ---- CLI -> PassCfg ---------------------------------------------------------
// The -O preset selects the pass tiers; granular legacy flags and the
// --enable/--disable overrides are applied on top (in that order, so a
// later --disable wins over --enable of the same pass).
static const char *const PASS_NAMES[] = {"fold",  "copyprop", "canon",
    "cse",   "unreach",  "splice", "dce",     "mem2reg", "trapelim",
    "licm",  "divmagic", "reassoc", "unroll", "lcf",     "inline",
    "scctco"};
static bool set_named_pass(PassCfg &p, const std::string &n, bool on,
                           std::string &err) {
  auto eq = [&](const char *s) { return n == s; };
  if (eq("fold")) p.p_fold = on;
  else if (eq("copyprop")) p.p_copyprop = on;
  else if (eq("canon")) p.p_canon = on;
  else if (eq("cse")) p.p_cse = on;
  else if (eq("unreach")) p.p_unreach = on;
  else if (eq("splice")) p.p_splice = on;
  else if (eq("dce")) p.p_dce = on;
  else if (eq("mem2reg")) p.p_mem2reg = on;
  else if (eq("trapelim")) p.p_trapelim = on;
  else if (eq("licm")) p.p_licm = on;
  else if (eq("divmagic")) p.p_divmagic = on;
  else if (eq("reassoc")) p.p_reassoc = on;
  else if (eq("unroll")) p.p_unroll = on;
  else if (eq("lcf")) p.p_lcf = on;
  else if (eq("constcall")) p.p_constcall = on;
  else if (eq("scctco")) p.p_scctco = on;
  else if (eq("tailrec")) p.p_tailrec = on;
  else if (eq("cdfold")) p.p_cdfold = on;
  else if (eq("ifconv")) p.p_ifconv = on;
  else if (eq("inline")) p.do_inline = on;
  else {
    err = n;
    return false;
  }
  return true;
}
static PassCfg pass_cfg_of(const Cli &C) {
  PassCfg p{};
  p.set_level(C.opt_level < 0 ? 3u : (uint32_t)C.opt_level);
  p.unroll = C.unroll;
  p.fp_limit = C.fp_limit ? C.fp_limit : 24u;
  p.debug = (uint8_t)(C.debug > 3 ? 3 : C.debug);
  p.regalloc = C.regalloc;
  if (C.no_opt) p.do_opt = false;
  if (C.no_inline) p.do_inline = false;
  if (C.no_scctco) p.p_scctco = false;
  if (C.no_reassoc) p.p_reassoc = false; // legacy granular flags
  if (C.no_lcf) p.p_lcf = false;
  if (C.no_divmagic) p.p_divmagic = false;
  std::string err{};
  for (const std::string &s : C.enable_passes)
    if (!set_named_pass(p, s, true, err)) {
      std::printf("unknown pass '%s' (see --help)\n", s.c_str());
      break;
    }
  for (const std::string &s : C.disable_passes)
    if (!set_named_pass(p, s, false, err)) {
      std::printf("unknown pass '%s' (see --help)\n", s.c_str());
      break;
    }
  return p;
}

// ---- 17b. --shrink=FILE: delta-debug a failing program ---------------------
// (ported from the Branchedmain monolith; adapted to the split's helpers).
// Greedily removes line chunks while the program still fails the full
// differential: oracle vs SSA vs optimized vs lowered vs VM, PLUS native
// C/asm parity at -O0 -- backend-only bugs are exactly what shrinking
// exists for. A sibling .in tape (as the fuzzer saves) is picked up
// automatically; deletions must keep the program compiling.
static int run_shrink(const Cli &C, const std::string &path) {
  std::string src = slurp_file(path);
  if (src.empty()) {
    std::printf("cannot read '%s'\n", path.c_str());
    return 1;
  }
  PassCfg cfg = pass_cfg_of(C);
  std::vector<val_t> ins{}; // an optional sibling tape (fuzzer saves .in)
  {
    size_t dot = path.find_last_of('.');
    std::string stem =
        dot == std::string::npos ? path : path.substr(0, dot);
    std::string tape = slurp_file(stem + ".in");
    if (!tape.empty()) ins = parse_inputs(tape);
  }
  const std::string cc = find_cc();
  std::string tape_path{};
  if (!ins.empty()) {
    tape_path = "./" + path + ".shrink.in";
    std::string t{};
    for (val_t v : ins) t += std::to_string(v) + "\n";
    write_file(tape_path, t);
  }
  auto fails = [&](const std::string &s) -> bool {
    Program P = compile(s, cfg);
    if (!P.ok) return false; // deletions must keep the program compiling
    RunOut oracle = run_ast(P.P.ast, P.P.root, 200000, ins);
    if (oracle.timeout) return false;
    RunOut plain = run_prog(fns_of(P, 0), 200000, ins);
    RunOut optimized = run_prog(fns_of(P, 1), 200000, ins);
    RunOut lowered = run_prog(fns_of(P, 2), 200000, ins);
    std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
    RunOut vm = vm_run(bc, 2000000, ins);
    if (!same_run(oracle, plain) || !same_run(oracle, optimized) ||
        !same_run(oracle, lowered) || !same_run(oracle, vm))
      return true;
    // native parity too: backend-only bugs (asm codegen, emission plan)
    // are exactly what shrinking exists for
    if (cc.empty()) return false;
    // NOTE: exe paths must contain a '/', else execvp (via timeout) will
    // NOT find them in the current directory and the run silently "fails"
    std::string stem = "./" + path + ".shrink";
    std::string ref = text_of(oracle);
    write_file(stem + ".c", emit_c(fns_of(P, 2), P.fn_names));
    write_file(stem + ".s", emit_x64(fns_of(P, 2), P.fn_names));
    bool bad = false;
    for (int ti = 0; ti < 2 && !bad; ++ti) {
      std::string srcp = ti ? stem + ".s" : stem + ".c";
      std::string exe = stem + (ti ? ".a.exe" : ".c.exe");
      if (std::system(("timeout 60 " + cc + " -w -O0 " + srcp + " -o " +
                       exe + " 2>/dev/null")
                          .c_str()) != 0)
        continue;
      std::string out_text{};
      int rc = run_capture("timeout 10 " + exe, out_text, stem + ".out",
                           tape_path);
      if (rc != 0 &&
          (((unsigned)rc >> 8) == 124 || ((unsigned)rc >> 8) == 137))
        out_text += "TIMEOUT\n";
      bad |= (out_text != ref);
      std::remove(exe.c_str());
    }
    return bad;
  };
  if (!fails(src)) {
    std::printf("'%s' does not fail the differential with the current "
                "flags\n(try passing the flags saved in the matching .flags "
                "file)\n",
                path.c_str());
    return 1;
  }
  // split into lines, then chunked ddmin (largest chunks first)
  std::vector<std::string> lines{};
  {
    std::string cur{};
    for (char c : src) {
      if (c == '\n') {
        lines.push_back(cur);
        cur.clear();
      } else
        cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);
  }
  size_t original = lines.size();
  auto join = [&](const std::vector<std::string> &ls) {
    std::string r{};
    for (const auto &l : ls) {
      r += l;
      r += '\n';
    }
    return r;
  };
  for (size_t chunk = lines.size() / 2; chunk >= 1; chunk /= 2) {
    for (bool again = true; again;) {
      again = false;
      for (size_t i = 0; i + chunk <= lines.size();) {
        std::vector<std::string> trial(lines.begin(),
                                       lines.begin() + (long)i);
        trial.insert(trial.end(), lines.begin() + (long)(i + chunk),
                     lines.end());
        if (fails(join(trial))) {
          lines.erase(lines.begin() + (long)i,
                      lines.begin() + (long)(i + chunk));
          again = true;
        } else {
          ++i;
        }
      }
    }
    if (chunk == 1) break;
  }
  std::string small = join(lines);
  std::string out = path + ".small.mjz";
  write_file(out, small);
  std::printf("shrink: %zu -> %zu lines; wrote %s\nreduced program:\n%s\n",
              original, lines.size(), out.c_str(), small.c_str());
  return 0;
}

// the per-file compile report + in-process runs; returns the optimized RunOut
static RunOut compile_report(Program &P, const Cli &C, const std::string &src,
                             bool brief) {
  P = compile(src, pass_cfg_of(C));
  if (!P.ok) {
    std::printf("compile error: %s (at byte %u)\n",
                P.error ? P.error : "?", (unsigned)P.errpos);
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
    print_analysis(P.an_pre_main, P.fns[0].ssa, "analysis (mjz graph)");
    std::printf(
        "  fns: %zu | ssa: %zu instrs, %u phis | opt: %zu | destroyed: %zu\n"
        "  opt: iters %u | folds %u | copies %u | copyprop %u | cse %u | "
        "canon %u+%u | brs %u | cbr== %u | dead-blk %u | splice %u | "
        "phi-degen %u | dce %u | hoist %u | INLINED %u | UNROLLED %u | "
        "REASSOC %u(-%u) | LCF %u+%u+g%u|rt%u | DIVMAGIC %u | NOTCOMB %u | "
        "TRAP-KILL %u | MEM2REG %u | DSE %u | SCC-TCO %u(+%u)\n"
        "  destroy: split-edges %u | copies %u | self %u | cycles %u | "
        "copy-fused %u\n",
        P.fns.size(), ssa_i, (unsigned)phis, opt_i, dst_i,
        (unsigned)os.iters, (unsigned)os.folds, (unsigned)os.copies,
        (unsigned)os.copyprops, (unsigned)os.cses, (unsigned)os.canon_swaps,
        (unsigned)os.canon_cmps, (unsigned)os.brs_folded,
        (unsigned)os.cbr_same, (unsigned)os.blocks_gone, (unsigned)os.splices,
        (unsigned)os.phi_degens, (unsigned)os.dce, (unsigned)os.hoisted,
        (unsigned)os.inlines, (unsigned)os.unrolls, (unsigned)os.reassocs,
        (unsigned)os.reassoc_gone, (unsigned)os.lcf_loops,
        (unsigned)os.lcf_sims, (unsigned)os.lcf_gensims, (unsigned)os.lcf_rt,
        (unsigned)os.divmagics, (unsigned)os.notcombs,
        (unsigned)os.traps_killed, (unsigned)os.mem2reg_lifts,
        (unsigned)os.mem2reg_dses, (unsigned)os.scctco,
        (unsigned)os.scctco_folds, (unsigned)de_c,
        (unsigned)dz_c, (unsigned)dse, (unsigned)dcy, (unsigned)dfu);
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
      print_fn(P.fns[f].ssa, t1);
      print_fn(P.fns[f].opt, t2);
      print_fn(P.fns[f].destroyed, t3);
    }
  }
  if (C.no_exec && !C.do_run) return {}; // compile/emit only
  RunOut R = run_prog(fns_of(P, 1), C.steps, C.inputs);
  RunOut Ro = run_ast(P.P.ast, P.P.root, C.steps, C.inputs);
  if (!C.quiet || brief) {
    std::printf("oracle   : ");
    print_run(Ro);
    std::printf("compiled : ");
    print_run(R);
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
  if (!write_file(c_path, emit_c(fns_of(P, 2), P.fn_names)) ||
      !write_file(s_path, emit_x64(fns_of(P, 2), P.fn_names))) {
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
  // saw (verbatim bytes); with no tape they read /dev/null (EOF -> 0)
  std::string in_path = "/dev/null";
  if (!C.stdin_text.empty()) {
    std::string p = C.out_dir + "/" + stem + ".in.tmp";
    if (write_file(p, C.stdin_text)) in_path = p;
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
      std::string exe = C.out_dir + "/" + stem + "." + J.tag + ".O" + lvl +
                        ".exe";
      std::string build = "timeout 60 " + cc + " -w -O" + lvl + " " +
                          J.src_path + " -o " + exe;
      if (std::system(build.c_str()) != 0) {
        std::printf("  %-18s %10s  BUILD FAILED (%s -O%s)\n", J.tag, "-", cc.c_str(), lvl.c_str());
        continue;
      }
      std::string out_text{};
      double best = 1e300;
      int rc = 0;
      for (unsigned r = 0; r < (C.repeats ? C.repeats : 1); ++r) {
        t0 = now_ms();
        rc = run_capture("timeout 20 " + exe, out_text, cap, in_path);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
      }
      bool timed_out = (rc != 0 && (((unsigned)rc >> 8) == 124 ||
                                     ((unsigned)rc >> 8) == 137));
      if (timed_out) out_text += "TIMEOUT\n";
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
    if (!std::fgets(line, sizeof line, stdin)) break;
    std::string t(line);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' ||
                          t.back() == ' ' || t.back() == '\t'))
      t.pop_back();
    if (t == ":q") break;
    if (t == ":r") {
      buf.clear();
      std::printf("(buffer reset)\n");
      continue;
    }
    if (t.empty()) {
      if (buf.empty()) continue;
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