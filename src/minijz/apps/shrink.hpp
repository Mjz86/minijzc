#pragma once
// === shrink.hpp ========================================
// --shrink: line-chunk delta-debugging of failing programs.

#include "cli_parse.hpp"
#include "app_api.hpp" // compile_report decl
#include "../prog/program.hpp"
#include "../prog/runners.hpp"
#include "../runtime/astrun.hpp"
#include "../back/bytecode.hpp"
#include "../base/native_run.hpp"

// ---- 17b. --shrink=FILE: delta-debug a failing program ---------------------
// (ported from the Branchedmain monolith; adapted to the split's helpers).
// Greedily removes line chunks while the program still fails the full
// differential: oracle vs SSA vs optimized vs lowered vs VM, PLUS native
// C/asm parity at -O0 -- backend-only bugs are exactly what shrinking
// exists for. A sibling .in tape (as the fuzzer saves) is picked up
// automatically; deletions must keep the program compiling.
int run_shrink(const Cli &C, const std::string &path) {
  std::string src = slurp_file(path);
  if (src.empty()) {
    std::printf("cannot read '%s'\n", path.c_str());
    return 1;
  }
  PassCfg cfg = pass_cfg_of(C);
  std::vector<val_t> ins{}; // an optional sibling tape (fuzzer saves .in)
  {
    size_t dot = path.find_last_of('.');
    std::string stem = dot == std::string::npos ? path : path.substr(0, dot);
    std::string tape = slurp_file(stem + ".in");
    if (!tape.empty())
      ins = parse_inputs(tape);
  }
  const std::string cc = find_cc();
  std::string tape_path{};
  if (!ins.empty()) {
    tape_path = "./" + path + ".shrink.in";
    std::string t{};
    for (val_t v : ins)
      t += std::to_string(v) + "\n";
    write_file(tape_path, t);
  }
  auto fails = [&](const std::string &s) -> bool {
    Program P = compile(s, cfg);
    if (!P.ok)
      return false; // deletions must keep the program compiling
    RunOut oracle = run_ast(P.P.ast, P.P.root, 200000, ins);
    if (oracle.timeout)
      return false;
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
    if (cc.empty())
      return false;
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
      if (std::system(("timeout 60 " + cc + " -w -O0 " + srcp + " -o " + exe +
                       " 2>/dev/null")
                          .c_str()) != 0)
        continue;
      std::string out_text{};
      int rc =
          run_capture("timeout 10 " + exe, out_text, stem + ".out", tape_path);
      if (rc != 0 && (((unsigned)rc >> 8) == 124 || ((unsigned)rc >> 8) == 137))
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
    if (!cur.empty())
      lines.push_back(cur);
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
        std::vector<std::string> trial(lines.begin(), lines.begin() + (long)i);
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
    if (chunk == 1)
      break;
  }
  std::string small = join(lines);
  std::string out = path + ".small.mjz";
  write_file(out, small);
  std::printf("shrink: %zu -> %zu lines; wrote %s\nreduced program:\n%s\n",
              original, lines.size(), out.c_str(), small.c_str());
  return 0;
}
