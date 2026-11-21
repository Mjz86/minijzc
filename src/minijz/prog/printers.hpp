#pragma once
// === printers.hpp ========================================
// the pretty-printers and the runner contract (same_run/text_of). LIGHT:
// everything here needs only the IR (instr.hpp) — no mjz_lib, no backends —
// so every TU can print IR and compare runner results cheaply.
// print_analysis() reads the An internals; its DECLARATION lives here and
// its definition is compiled once with the analysis (prog/compile_impl.hpp).

#include "../base/srcmap.hpp"
#include "../ir/instr.hpp"

#include <cstdio>
#include <string>
#include <vector>

struct An; // opt/analysis/an.hpp (defined with the pipeline TU)

inline std::string vname(const Fn &fn, uint32_t v) {
  if (v == NONE)
    return "_";
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

void print_analysis(const An &A, const Fn &fn, const char *tag);

// the IR text of one fn (title + body). One code path for --print-ir, the
// debugger's `ir` command and the pass-diff viewer (which diffs the text).
#ifdef MJZ_MULTI_CPP
// the five bodies compiled ONCE in tu/runners.cpp (prog/printers_impl.hpp)
std::string fn_ir_str(const Fn &fn, const char *title,
                      std::string_view src = {});
void print_fn(const Fn &fn, const char *title, std::string_view src = {});
std::string unified_diff(const std::string &a, const std::string &b,
                         int ctx = 2);
void print_run(const RunOut &R, std::string_view src = {});
std::string text_of(const RunOut &R);
#else
inline std::string fn_ir_str(const Fn &fn, const char *title,
                             std::string_view src = {});
inline void print_fn(const Fn &fn, const char *title,
                     std::string_view src = {});
inline std::string unified_diff(const std::string &a, const std::string &b,
                                int ctx = 2);
inline void print_run(const RunOut &R, std::string_view src = {});
inline std::string text_of(const RunOut &R);
#include "printers_impl.hpp"
#endif


// ---- unified line diff (LCS) ------------------------------------------------
// Small texts only (IR dumps of one fn): the O(n*m) table is fine at these
// sizes. Shared by the debugger's `opt diff` and the CLI's --opt-diff.
// Output: unified-diff style hunks with `ctx` lines of context, '-' old
// lines, '+' new lines; empty string when the texts are equal.


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

