#pragma once
// === cli_parse.hpp ========================================
// the CLI front: option table, parsing, usage text, and the -O/--enable/
// --disable mapping onto PassCfg. LIGHT (no IR, no pipeline): tu/main.cpp
// parses and validates the command line without paying for the compiler;
// the heavy interpretation lives in cli_run.hpp / app_main.hpp.

#include "../base/pass_cfg.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Cli {
  std::string file{};
  std::string inline_src{};
  std::string cc{}; // "" = auto-detect
  std::string out_dir{"."};
  std::string stdin_path{};    // --stdin=FILE (input() tape)
  std::string stdin_text{};    // raw text of the tape (fed to native builds)
  std::vector<val_t> inputs{}; // parsed tape (fed to in-process runners)
  std::vector<std::string> native_O{"0"};
  std::string exe_name{}; // -o NAME: name of the --emit=exe executable
  bool emit_c{false}, emit_asm{false}, emit_bc{false}, emit_exe{false};
  bool do_run{false}, no_opt{false}, print_ir{false}, print_bc{false};
  bool no_inline{false};
  bool no_scctco{false};
  bool no_reassoc{false}, no_lcf{false}, no_divmagic{false};
  int opt_level{-1};     // -O0..-O3 preset (-1: default, i.e. 3)
  unsigned debug{0};     // --debug=N: pass-manager dump volume (0..3)
  unsigned fp_limit{24}; // --fp-limit=N: fixpoint round cap (0 = default)
  bool regalloc{true};   // --no-regalloc: identity slots ("SSA-like" C)
  std::vector<std::string> enable_passes{}, disable_passes{};
  unsigned unroll{16}; // max constant trip count to fully unroll (0 = off)
  bool no_exec{false}, quiet{false}, check_all{false}, debug_plan{false};
  unsigned repeats{3};
  uint64_t steps{200000000ull};
  long fuzz{0}, fuzz_native{0}, fuzz_from{1};
  long fuzz_jobs{0};         // --fuzz-jobs=N: parallel fuzz workers
                             // (0 = auto: selftest serial, else cores)
  bool fuzz_random{false};   // --fuzz-cfg=random: random pass configs
  std::string shrink_file{}; // --shrink=FILE: delta-debug a failing program
  bool demo{false}, selftest{false}, help{false};
  bool mjz_fuzz_dumps{false};
  bool debugger{false}; // --debugger: interactive IR/VM session
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
      "  --fuzz-jobs=N    fuzz with N parallel workers (default: auto — the\n"
      "                    selftest stays serial, plain --fuzz=N uses the\n"
      "                    cores, capped at 4). Progress goes to stderr; a\n"
      "                    watchdog names the in-flight seeds if a chunk\n"
      "                    stalls, so a pathological seed is identifiable\n"
      "                    instead of looking like a hang\n"
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
      "                    parsed for the in-process runners AND piped "
      "verbatim\n"
      "                    into the native binaries (default: real stdin if "
      "it\n"
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
      "  --fuzz_dump       dumps fuzz info (MJZ_FUZZ_DUMP env var is no longer "
      "used) \n"
      "  --debugger        interactive debugger on the compiled program: dump\n"
      "                    IR at every stage, walk the pass pipeline live,\n"
      "                    single-step the bytecode VM with full state\n"
      "                    (frames/slots/stack/arena), breakpoints included\n"
      "  --help            this text\n");
}

static void split_commas(const std::string &s, std::vector<std::string> &out) {
  std::string cur{};
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty())
        out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty())
    out.push_back(cur);
}

static bool parse_cli(int argc, char **argv, Cli &C) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char *pre) -> const char * {
      size_t n = std::strlen(pre);
      if (a.compare(0, n, pre) != 0)
        return nullptr;
      const char *v = a.c_str() + n;
      if (!*v)
        return nullptr;
      return v;
    };
    if (a == "--help" || a == "-h")
      C.help = true;
    else if (a == "--demo")
      C.demo = true;
    else if (a == "--selftest")
      C.selftest = true;
    else if (a == "--fuzz")
      C.fuzz = 1000;
    else if (const char *v = val("--fuzz="))
      C.fuzz = atol(v);
    else if (const char *v = val("--fuzz-from=")) {
      C.fuzz_from = atol(v);
      if (C.fuzz_from < 1)
        C.fuzz_from = 1;
    } else if (a == "--fuzz-native")
      C.fuzz_native = 20;
    else if (const char *v = val("--fuzz-native="))
      C.fuzz_native = atol(v);
    else if (a == "--fuzz-cfg=random" || a == "--fuzz-random")
      C.fuzz_random = true;
    else if (const char *v = val("--shrink="))
      C.shrink_file = v;
    else if (a == "--run")
      C.do_run = true;
    else if (a == "--no-exec" || a == "--compile-only")
      C.no_exec = true;
    else if (a == "--check-all")
      C.check_all = true;
    else if (a == "--debug-plan")
      C.debug_plan = true;
    else if (a == "-O0")
      C.opt_level = 0;
    else if (a == "-O1")
      C.opt_level = 1;
    else if (a == "-O2")
      C.opt_level = 2;
    else if (a == "-O3")
      C.opt_level = 3;
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
    } else if (const char *v = val("--debug="))
      C.debug = (unsigned)atol(v);
    else if (const char *v = val("--fp-limit="))
      C.fp_limit = (unsigned)atol(v);
    else if (a == "--no-regalloc")
      C.regalloc = false;
    else if (a == "--no-opt")
      C.no_opt = true;
    else if (a == "--no-inline")
      C.no_inline = true;
    else if (a == "--no-scctco")
      C.no_scctco = true;
    else if (a == "--no-reassoc")
      C.no_reassoc = true;
    else if (a == "--fuzz_dump")
      C.mjz_fuzz_dumps = true;
    else if (const char *v = val("--fuzz-jobs="))
      C.fuzz_jobs = atol(v);
    else if (a == "--debugger")
      C.debugger = true;
    else if (a == "--no-lcf")
      C.no_lcf = true;
    else if (a == "--no-divmagic")
      C.no_divmagic = true;
    else if (const char *v = val("--unroll="))
      C.unroll = (unsigned)atol(v);
    else if (a == "--print-ir")
      C.print_ir = true;
    else if (a == "--print-bc")
      C.print_bc = true;
    else if (a == "--quiet")
      C.quiet = true;
    else if (const char *v = val("--repeats="))
      C.repeats = (unsigned)atol(v);
    else if (const char *v = val("--steps="))
      C.steps = strtoull(v, nullptr, 10);
    else if (const char *v = val("--cc="))
      C.cc = v;
    else if (const char *v = val("--stdin="))
      C.stdin_path = v;
    else if (const char *v = val("--out-dir="))
      C.out_dir = v;
    else if (const char *v = val("--native-O=")) {
      C.native_O.clear();
      split_commas(v, C.native_O);
    } else if (const char *v = val("--emit=")) {
      std::vector<std::string> parts{};
      split_commas(v, parts);
      for (auto &p : parts) {
        if (p == "c")
          C.emit_c = true;
        else if (p == "asm")
          C.emit_asm = true;
        else if (p == "bc")
          C.emit_bc = true;
        else if (p == "exe")
          C.emit_exe = true;
        else {
          std::printf("unknown --emit target '%s' (c, asm, bc, exe)\n",
                      p.c_str());
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
static const char *const PASS_NAMES[] = {
    "fold",   "copyprop", "canon",    "cse",   "unreach",  "splice",
    "dce",    "mem2reg",  "trapelim", "licm",  "divmagic", "reassoc",
    "unroll", "lcf",      "inline",   "scctco"};
static bool set_named_pass(PassCfg &p, const std::string &n, bool on,
                           std::string &err) {
  auto eq = [&](const char *s) { return n == s; };
  if (eq("fold"))
    p.p_fold = on;
  else if (eq("copyprop"))
    p.p_copyprop = on;
  else if (eq("canon"))
    p.p_canon = on;
  else if (eq("cse"))
    p.p_cse = on;
  else if (eq("unreach"))
    p.p_unreach = on;
  else if (eq("splice"))
    p.p_splice = on;
  else if (eq("dce"))
    p.p_dce = on;
  else if (eq("mem2reg"))
    p.p_mem2reg = on;
  else if (eq("trapelim"))
    p.p_trapelim = on;
  else if (eq("licm"))
    p.p_licm = on;
  else if (eq("divmagic"))
    p.p_divmagic = on;
  else if (eq("reassoc"))
    p.p_reassoc = on;
  else if (eq("unroll"))
    p.p_unroll = on;
  else if (eq("lcf"))
    p.p_lcf = on;
  else if (eq("constcall"))
    p.p_constcall = on;
  else if (eq("scctco"))
    p.p_scctco = on;
  else if (eq("tailrec"))
    p.p_tailrec = on;
  else if (eq("cdfold"))
    p.p_cdfold = on;
  else if (eq("ifconv"))
    p.p_ifconv = on;
  else if (eq("sccp"))
    p.p_sccp = on;
  else if (eq("ldelim"))
    p.p_ldelim = on;
  else if (eq("callcse"))
    p.p_callcse = on;
  else if (eq("inline"))
    p.do_inline = on;
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
  if (C.no_opt)
    p.do_opt = false;
  if (C.no_inline)
    p.do_inline = false;
  if (C.no_scctco)
    p.p_scctco = false;
  if (C.no_reassoc)
    p.p_reassoc = false; // legacy granular flags
  if (C.no_lcf)
    p.p_lcf = false;
  if (C.no_divmagic)
    p.p_divmagic = false;
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
