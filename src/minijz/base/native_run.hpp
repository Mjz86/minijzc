#pragma once
// === native_run.hpp ========================================
// native build & run harness: find_cc, slurp_file, parse_inputs,
// run_capture, write_file -- shared by --run, the fuzzer and
// --shrink. Windows notes: system() goes through cmd.exe there, which
// has no `command -v`, no /dev/null (it is `nul`) and whose `timeout`
// is a SLEEP command, not coreutils' killer -- the helpers below hide
// those three differences behind one portable spelling each.

#include "../config.hpp"

// ---- 13.5d. native build & run harness
// ----------------------------------------
static bool write_file(const std::string &path, const std::string &data) {
  std::ofstream f(path, std::ios::binary);
  if (!f)
    return false;
  f << data;
  return bool(f);
}
static bool write_file_bytes(const std::string &path,
                             const std::vector<uint8_t> &data) {
  std::ofstream f(path, std::ios::binary);
  if (!f)
    return false;
  f.write((const char *)data.data(), (std::streamsize)data.size());
  return bool(f);
}
static std::string slurp_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return {};
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}
// cmd.exe's null device; POSIX shells spell it /dev/null
static inline const char *mjz_devnull() {
#ifdef _WIN32
  return "nul";
#else
  return "/dev/null";
#endif
}
// "timeout SECS cmd" on POSIX (coreutils); on Windows cmd's `timeout` is a
// SLEEP (it would wait SECS and never run cmd), so native invocations run
// bare there -- in-process runs are bounded by the step budget either way,
// and a hung native build is a compiler bug the parity gate will name.
static inline std::string mjz_timeout(int secs, const std::string &cmd) {
#ifdef _WIN32
  (void)secs;
  return cmd;
#else
  return "timeout " + std::to_string(secs) + " " + cmd;
#endif
}
static std::string find_cc() {
#ifdef _WIN32
  // no `command -v` in cmd.exe; `where` probes PATH (and PATHEXT, so "g++"
  // finds g++.exe). Only GNU-style drivers (clang++/g++/clang/gcc -- the
  // windows-llvm / windows-ninja presets install exactly those) accept the
  // -O0/-o spelling the harness builds; plain cl.exe does not.
  auto has = [](const char *cc) {
    std::string cmd = std::string("where ") + cc + " >nul 2>&1";
    return std::system(cmd.c_str()) == 0;
  };
  if (has("clang++"))
    return "clang++"; // preferred: user-requested driver
  if (has("g++"))
    return "g++";
  if (has("clang"))
    return "clang";
  if (has("gcc"))
    return "gcc";
  return "";
#else
  auto has = [](const char *cc) {
    std::string cmd = std::string("command -v ") + cc + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
  };
  if (has("clang++"))
    return "clang++"; // preferred: user-requested driver
  if (has("g++"))
    return "g++";
  if (has("cc"))
    return "cc";
  return "";
#endif
}
// parse whitespace-separated integers; stop at the first non-number (the
// same place scanf in the native builds gives up, so the tape stays in sync)
static std::vector<val_t> parse_inputs(const std::string &text) {
  std::vector<val_t> out{};
  const char *p = text.c_str();
  char *end{};
  for (;;) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
      ++p;
    if (!*p)
      break;
    long long v = std::strtoll(p, &end, 10);
    if (end == p)
      break;
    out.push_back((val_t)v);
    p = end;
  }
  return out;
}
// run `cmd`, capturing stdout into `out`; stdin redirected from `in_path`
// (empty = inherit); returns the raw system() status
static int run_capture(const std::string &cmd, std::string &out,
                       const std::string &cap_path,
                       const std::string &in_path = {}) {
  std::string full = cmd;
  if (!in_path.empty())
    full += " < " + in_path;
  full += std::string(" > ") + cap_path + " 2>" + mjz_devnull();
  int rc = std::system(full.c_str());
  out = slurp_file(cap_path);
  return rc;
}
static double now_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
