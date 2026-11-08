#pragma once
// === printers.hpp ========================================
// the pretty-printers and the runner contract (same_run/text_of). LIGHT:
// everything here needs only the IR (instr.hpp) — no mjz_lib, no backends —
// so every TU can print IR and compare runner results cheaply.
// print_analysis() reads the An internals; its DECLARATION lives here and
// its definition is compiled once with the analysis (prog/compile_impl.hpp).

#include "minijz/ir/instr.hpp"

#include <cstdio>
#include <string>
#include <vector>

struct An; // opt/analysis/an.hpp (defined with the pipeline TU)

inline std::string vname(const Fn &fn, uint32_t v) {
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

void print_analysis(const An &A, const Fn &fn, const char *tag);

inline void print_fn(const Fn &fn, const char *title) {
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

inline void print_run(const RunOut &R) {
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

inline std::string text_of(const RunOut &R) {
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
