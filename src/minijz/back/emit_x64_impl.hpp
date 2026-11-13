#pragma once
// === emit_x64_impl.hpp ========================================
// the x86-64 backend body: per-fn codegen + the whole-program entry.
// The register allocator lives in regalloc.hpp (split off, checkpoint
// 21 — same include position, so the definition order and behavior are
// unchanged). Included by emit_x64.hpp in the header-only build
// (constexpr) and by tu/backends.cpp in the multi-TU build — include it
// nowhere else.
//
// PHASES (emit_x64_fn per Fn; emit_x64 assembles the whole program):
//   * plan + allocator: make_emit_plan, then ra_plan -> RaPlan{color,
//     spill, nspill, usedmask}; RREGS maps colors 0..4 to
//     rbx/r12/r13/r14/r15; loc(v) is the register or the spill home
//     -8*(npush+k+1)(%rbp), BELOW the pushed callee-saved regs.
//   * prologue: pushq %rbp; movq %rsp,%rbp; pushq the used subset;
//     subq $frame (nspill*8 rounded up to 16, +8 when npush is odd --
//     calls stay 16-aligned); non-main fns incq mjz_depth with the 512
//     guard; params move in from rdi/rsi/rdx/rcx/r8/r9 (never-used
//     params get NO mov: their homes are free for live values); no
//     slot zero-init (SSA dominance + Const-0 phi materialization).
//   * body over P.order (labels .Lb%u in main, .LF%u_%u in helpers,
//     referenced blocks only): Bin resolves the two-address hazard (y
//     coalesced into dst => accumulate onto y; Sub stashes y in %rcx);
//     shifts use %cl (x86's 6-bit mask == the &63 semantics); Div/Mod
//     guard 0 and INT64_MIN/-1 unless proven (I.nt) or const; Cbr
//     fuses its compare; helpers serve array ops/printf/input; st_rax
//     homes each result (register, or spill slot).
//   * epilogue per Ret: %rax, decq mjz_depth, addq $frame, popq the
//     saved regs in reverse, popq %rbp, ret (no Exit still => exit).
//   * emit_x64: feature scan, callees first then main, the
//     self-aligning mjz_trap, arena/input helpers, .rodata + .bss.
//
// THEORY: the prologue/epilogue are exact inverses (rbp, the counted
//     callee-saved subset, the rounded frame, undone in reverse), and
//     the RaPlan is the SAME chordal plan regaudit re-derives.
//
// CONSUMERS: emit_x64.hpp (both build modes); cli_run.hpp writes the
//     .s artifact for the native -O matrix.

#include "../graph_lib.hpp"
#include "../base/pass_cfg.hpp"
#include "../base/srcmap.hpp"
#include "../ir/instr.hpp"
#include "emit_plan.hpp"
#include "regalloc.hpp"

#include <string>
#include <string_view>
#include <vector>

#define MJZ_EMIT_FN // runtime-only (static locals): plain in both modes

MJZ_EMIT_FN std::string emit_x64_fn(const Fn &fn, uint32_t fidx, bool is_main,
                                    bool multi, const SrcMap &SM,
                                    std::string_view fname) {
  EmitPlan P = make_emit_plan(fn);
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  char buf[192];
  std::string s{};
  auto fits32 = [](val_t c) {
    return c >= INT64_C(-2147483648) && c <= INT64_C(2147483647);
  };
  RaPlan RP = ra_plan(fn, P);
  static const char *const RREGS[5] = {"rbx", "r12", "r13", "r14", "r15"};
  auto hascolor = [&](uint32_t v) -> bool {
    return v != NONE && v < nv && RP.color[v] >= 0 && RP.color[v] < 5;
  };
  auto rname = [&](uint32_t v) -> const char * { return RREGS[RP.color[v]]; };
  // number of callee-saved registers this function pushes (computed here so
  // the spill offsets below can skip them) — std::popcount is the C++20
  // spelling of __builtin_popcount (MSVC has no builtins)
  const uint32_t npush = (uint32_t)std::popcount(RP.usedmask);
  auto slot = [&](uint32_t v) -> std::string { // spill home of v
    // MUST live BELOW the pushed callee-saved registers: the pushes occupy
    // -8..-8*npush(%rbp), so spill k goes to -8*(npush+k+1)(%rbp). (The old
    // -8*(k+1)(%rbp) addressing overlapped the saved rbx/r12 and a spilled
    // value silently corrupted the caller's registers.)
    std::snprintf(buf, sizeof buf, "-%u(%%rbp)",
                  (unsigned)(8 * (npush + RP.spill[v] + 1)));
    return std::string(buf);
  };
  auto loc = [&](uint32_t v) -> std::string { // operand text: %reg or slot
    if (hascolor(v))
      return std::string("%") + rname(v);
    return slot(v);
  };
  auto is_c = [&](uint32_t v) -> bool {
    return v != NONE && v < nv && P.kc[v];
  };
  auto cv = [&](uint32_t v) -> val_t { return P.kv[v]; };
  // load an operand into a SCRATCH register (rax/rcx/rdx/rsi/rdi/r8/r9)
  auto ld = [&](const char *reg, uint32_t v) {
    if (is_c(v)) {
      if (fits32(cv(v)))
        std::snprintf(buf, sizeof buf, "  movq $%lld, %%%s\n", (long long)cv(v),
                      reg);
      else
        std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%%s\n",
                      (long long)cv(v), reg);
    } else if (hascolor(v)) {
      std::snprintf(buf, sizeof buf, "  movq %%%s, %%%s\n", rname(v), reg);
    } else {
      std::snprintf(buf, sizeof buf, "  movq %s, %%%s\n", slot(v).c_str(), reg);
    }
    s += buf;
  };
  // store %rax into a destination (register in place, or spill slot)
  auto st_rax = [&](uint32_t v) {
    if (v == NONE || v >= nv)
      return;
    if (hascolor(v)) {
      std::snprintf(buf, sizeof buf, "  movq %%rax, %%%s\n", rname(v));
      s += buf;
    } else {
      s += "  movq %rax, " + slot(v) + "\n";
    }
  };
  // move operand x into dst's register (no-op when already there)
  auto src_into = [&](const char *dr, uint32_t x) {
    if (is_c(x)) {
      if (fits32(cv(x))) {
        std::snprintf(buf, sizeof buf, "  movq $%lld, %%%s\n", (long long)cv(x),
                      dr);
        s += buf;
      } else {
        std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%rax\n",
                      (long long)cv(x));
        s += buf;
        std::snprintf(buf, sizeof buf, "  movq %%rax, %%%s\n", dr);
        s += buf;
      }
    } else if (hascolor(x) && !std::strcmp(rname(x), dr)) {
      // already in place: no code
    } else {
      std::snprintf(buf, sizeof buf, "  movq %s, %%%s\n", loc(x).c_str(), dr);
      s += buf;
    }
  };
  auto jcc_of = [](B b) -> const char * {
    switch (b) {
    case B::Lt:
      return "jl";
    case B::Le:
      return "jle";
    case B::Gt:
      return "jg";
    case B::Ge:
      return "jge";
    case B::Eq:
      return "je";
    default:
      return "jne";
    }
  };
  auto jcc_inv = [](const char *j) -> const char * {
    if (!std::strcmp(j, "jl"))
      return "jge";
    if (!std::strcmp(j, "jle"))
      return "jg";
    if (!std::strcmp(j, "jg"))
      return "jle";
    if (!std::strcmp(j, "jge"))
      return "jl";
    if (!std::strcmp(j, "je"))
      return "jne";
    return "je";
  };
  // frame: spill bytes, 16-aligned, padded so calls stay aligned given the
  // (possibly odd) number of pushed callee-saved registers
  uint64_t frame = ((uint64_t)RP.nspill * 8 + 15) / 16 * 16;
  if (npush & 1)
    frame += 8;
  if (frame < (uint64_t)RP.nspill * 8)
    frame = RP.nspill * 8;
  // block labels are file-scope in asm: prefix per function
  auto lbl = [&](uint32_t b) -> std::string {
    char l[48];
    if (is_main)
      std::snprintf(l, sizeof l, ".Lb%u", (unsigned)b);
    else
      std::snprintf(l, sizeof l, ".LF%u_%u", (unsigned)fidx, (unsigned)b);
    return std::string(l);
  };
  if (is_main) {
    s += "  .globl main\n  .type main, @function\nmain:\n";
  } else {
    s += "mjz_f" + std::to_string(fidx) + ":\n";
  }
  // ---- source-location banner (mirrors the C backend's) ------------------
  {
    uint32_t lo = NONE, hi = 0;
    for (const Instr &I : fn.code)
      if (I.src != NONE) {
        if (lo == NONE || I.src < lo)
          lo = I.src;
        if (I.src > hi)
          hi = I.src;
      }
    if (lo != NONE && SM.ok() && lo <= SM.src.size()) {
      char b2[96];
      if (is_main)
        std::snprintf(b2, sizeof b2, "  # main: source lines %u..%u\n",
                      (unsigned)SM.line(lo), (unsigned)SM.line(hi));
      else
        std::snprintf(b2, sizeof b2, "  # fn %s: source lines %u..%u\n",
                      std::string(fname).c_str(), (unsigned)SM.line(lo),
                      (unsigned)SM.line(hi));
      s += b2;
    }
  }
  s += "  pushq %rbp\n  movq %rsp, %rbp\n";
  for (uint32_t c = 0; c < 5; ++c)
    if (RP.usedmask & ((uint32_t)1 << c))
      s += std::string("  pushq %") + RREGS[c] + "\n";
  if (frame) {
    std::snprintf(buf, sizeof buf, "  subq $%llu, %%rsp\n",
                  (unsigned long long)frame);
    s += buf;
  }
  if (!is_main) { // recursion depth guard (matches the VM/interpreters)
    s += "  incq mjz_depth(%rip)\n";
    s += "  cmpq $512, mjz_depth(%rip)\n  jg mjz_trap\n";
    static const char *argregs[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    { // a NEVER-USED parameter gets NO incoming mov: the RA is free to
      // give its home to a live value (dead params have no interference
      // edges), and the mov would clobber that live value on entry
      std::vector<uint32_t> pu(nv, 0);
      for (const Instr &I : fn.code) {
        auto bump = [&](uint32_t v) {
          if (v != NONE && v < nv)
            ++pu[v];
        };
        bump(I.a);
        bump(I.b2);
        bump(I.c2);
        for (uint32_t v : I.args)
          bump(v);
      }
      for (uint32_t p = 0; p < fn.nparams && p < 6; ++p)
        if (pu[p] != 0)
          s += std::string("  movq %") + argregs[p] + ", " + loc(p) + "\n";
    }
  }
  // NOTE: no slot zero-init — SSA dominance guarantees every read of a slot
  // is preceded (on its path) by the write of its def; undef phi args were
  // materialized as explicit `Const 0` copies during SSA destruction.
  // referenced labels (fallthrough-only blocks need none)
  std::vector<char> refd(fn.nblocks(), 0);
  for (uint32_t b : P.order)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (P.dead[i])
        continue;
      if (I.k == K::Br && I.t != P.next[b])
        refd[I.t] = 1;
      if (I.k == K::Cbr) {
        if (P.fuse[i] == NONE && I.a < nv && P.kc[I.a]) {
          uint32_t tgt = (P.kv[I.a] != 0) ? I.t : I.f; // constant condition
          if (tgt != P.next[b])
            refd[tgt] = 1;
        } else if (I.f == P.next[b]) {
          refd[I.t] = 1; // `jcc .Lb_t` false arm falls through
        } else if (I.t == P.next[b]) {
          refd[I.f] = 1; // inverted `jcc .Lb_f` true arm falls through
        } else {
          refd[I.t] = 1;
          refd[I.f] = 1;
        }
      }
    }
  bool saw_exit = false;
  // A `# line:col` comment appears whenever the source location of the
  // emitted instructions CHANGES (one comment per source statement group;
  // pass-synthesized instrs carry no src and emit none)
  uint32_t last_src = NONE;
  for (uint32_t b : P.order) {
    if (refd[b])
      s += lbl(b) + ":\n";
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (P.dead[i])
        continue;
      if (SM.ok() && I.src != NONE && I.src != last_src) {
        char lb[48];
        if (SM.at(I.src, lb, sizeof lb)) {
          s += "  # ";
          s += lb;
          s += "\n";
        }
        last_src = I.src;
      }
      switch (I.k) {
      case K::Phi:
        break;         // destroyed input has none
      case K::Const: { // multi-def slot: a real write of THIS constant
        if (hascolor(I.dst)) {
          if (fits32(I.c)) {
            std::snprintf(buf, sizeof buf, "  movq $%lld, %%%s\n",
                          (long long)I.c, rname(I.dst));
            s += buf;
          } else {
            std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%rax\n",
                          (long long)I.c);
            s += buf;
            std::snprintf(buf, sizeof buf, "  movq %%rax, %%%s\n",
                          rname(I.dst));
            s += buf;
          }
        } else {
          if (fits32(I.c))
            s += "  movq $" + std::to_string((long long)I.c) + ", " +
                 slot(I.dst) + "\n";
          else {
            std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%rax\n",
                          (long long)I.c);
            s += buf;
            s += "  movq %rax, " + slot(I.dst) + "\n";
          }
        }
        break;
      }
      case K::Copy:
        if (hascolor(I.dst)) {
          src_into(rname(I.dst), I.a);
        } else if (is_c(I.a)) {
          if (fits32(cv(I.a)))
            s += "  movq $" + std::to_string((long long)cv(I.a)) + ", " +
                 slot(I.dst) + "\n";
          else {
            std::snprintf(buf, sizeof buf, "  movabsq $%lld, %%rax\n",
                          (long long)cv(I.a));
            s += buf;
            s += "  movq %rax, " + slot(I.dst) + "\n";
          }
        } else {
          s += "  movq " + loc(I.a) + ", %rax\n";
          s += "  movq %rax, " + slot(I.dst) + "\n";
        }
        break;
      case K::Bin: {
        // normalize: commutative ops put the immediate (if any) on the right
        uint32_t x = I.a, y = I.b2;
        if (is_c(x) && !is_c(y) &&
            (I.b == B::Add || I.b == B::Mul || I.b == B::Eq || I.b == B::Ne))
          std::swap(x, y);
        bool yimm = is_c(y);
        const bool inreg_dst = hascolor(I.dst);
        const char *dr = inreg_dst ? rname(I.dst) : "rax";
        switch (I.b) {
        case B::Add:
        case B::Sub:
        case B::Mul:
        case B::And:
        case B::Or:
        case B::Xor: {
          const char *op = I.b == B::Add   ? "addq"
                           : I.b == B::Sub ? "subq"
                           : I.b == B::Mul ? "imulq"
                           : I.b == B::And ? "andq"
                           : I.b == B::Or  ? "orq"
                                           : "xorq";
          const bool comm = (I.b != B::Sub);
          if (!yimm && hascolor(y) && !std::strcmp(rname(y), dr)) {
            // TWO-ADDRESS HAZARD: y is coalesced into the destination
            // register (legal: y dies here). Moving x into dr first would
            // destroy y before the op reads it.
            if (comm) { // dst = x OP y == y OP x: accumulate onto y in place
              std::snprintf(buf, sizeof buf, "  %s %s, %%%s\n", op,
                            loc(x).c_str(), dr);
              s += buf;
            } else { // Sub: stash y, move x in, subtract the stash
              std::snprintf(buf, sizeof buf, "  movq %%%s, %%rcx\n", dr);
              s += buf;
              src_into(dr, x);
              s += "  subq %rcx, %" + std::string(dr) + "\n";
            }
            break;
          }
          src_into(dr, x); // dst register holds x (mov elided when equal)
          if (yimm && fits32(cv(y))) {
            std::snprintf(buf, sizeof buf, "  %s $%lld, %%%s\n", op,
                          (long long)cv(y), dr);
            s += buf;
          } else if (yimm) {
            ld("rcx", y);
            std::snprintf(buf, sizeof buf, "  %s %%rcx, %%%s\n", op, dr);
            s += buf;
          } else {
            std::snprintf(buf, sizeof buf, "  %s %s, %%%s\n", op,
                          loc(y).c_str(), dr);
            s += buf;
          }
          break;
        }
        case B::Shl:
        case B::LShr:
        case B::AShr: {
          // x86 masks shift counts to 6 bits itself -- same semantics as the
          // &63 the other runners apply, so const and variable counts agree
          const char *op = I.b == B::Shl    ? "salq"
                           : I.b == B::LShr ? "shrq"
                                            : "sarq";
          src_into(dr, x);
          if (yimm) {
            std::snprintf(buf, sizeof buf, "  %s $%llu, %%%s\n", op,
                          (unsigned long long)((uint64_t)cv(y) & 63), dr);
            s += buf;
          } else {
            ld("rcx", y);
            std::snprintf(buf, sizeof buf, "  %s %%cl, %%%s\n", op, dr);
            s += buf;
          }
          break;
        }
        case B::MulHi: { // unsigned high half: rdx:rax = rax * rcx
          ld("rax", x);
          ld("rcx", y);
          s += "  mulq %rcx\n";
          if (inreg_dst) {
            std::snprintf(buf, sizeof buf, "  movq %%rdx, %%%s\n", dr);
            s += buf;
          } else {
            s += "  movq %rdx, %rax\n";
          }
          break;
        }
        case B::Div:
        case B::Mod: {
          bool bc = yimm; // divisor known?
          val_t bv = bc ? cv(y) : 0;
          bool ac = is_c(x); // dividend known?
          val_t av = ac ? cv(x) : 0;
          bool always = bc && (bv == 0 || (bv == -1 && ac && av == INT64_MIN));
          if (always) {
            s += "  jmp mjz_trap\n";
            break;
          }
          if (I.nt) {
            // PROVEN trap-free (range analysis): no guard, plain divide
            src_into("rax", x);
            ld("rcx", y);
            s += "  cqto\n  idivq %rcx\n";
            if (I.b == B::Mod)
              s += "  movq %rdx, %rax\n";
            st_rax(I.dst);
            break;
          }
          if (bc && bv == -1) { // x / -1 == -x (trap only on INT64_MIN)
            src_into("rax", x);
            s += "  movabsq $0x8000000000000000, %rdx\n";
            s += "  cmpq %rdx, %rax\n  je mjz_trap\n";
            s += I.b == B::Div ? "  negq %rax\n" : "  xorl %eax, %eax\n";
            break;
          }
          src_into("rax", x);
          if (bc) { // known safe divisor: no guard at all
            ld("rcx", y);
          } else { // unknown divisor: full guard, then divide
            ld("rcx", y);
            s += "  testq %rcx, %rcx\n  jz mjz_trap\n";
            s += "  movabsq $0x8000000000000000, %rdx\n";
            s += "  cmpq %rdx, %rax\n  jne 1f\n";
            s += "  cmpq $-1, %rcx\n  je mjz_trap\n";
            s += "1:\n";
          }
          s += "  cqto\n  idivq %rcx\n";
          if (I.b == B::Mod)
            s += "  movq %rdx, %rax\n";
          st_rax(I.dst); // register in place, spill slot otherwise
          break;
        }
        default: { // comparisons
          src_into("rax", x);
          if (yimm && fits32(cv(y))) {
            std::snprintf(buf, sizeof buf, "  cmpq $%lld, %%rax\n",
                          (long long)cv(y));
            s += buf;
          } else {
            ld("rcx", y);
            s += "  cmpq %rcx, %rax\n";
          }
          const char *set = I.b == B::Lt   ? "setl"
                            : I.b == B::Le ? "setle"
                            : I.b == B::Gt ? "setg"
                            : I.b == B::Ge ? "setge"
                            : I.b == B::Eq ? "sete"
                                           : "setne";
          if (inreg_dst) {
            std::snprintf(buf, sizeof buf, "  %s %%dl\n  movzbq %%dl, %%%s\n",
                          set, dr);
            s += buf;
          } else {
            std::snprintf(buf, sizeof buf, "  %s %%dl\n  movzbq %%dl, %%rax\n",
                          set);
            s += buf;
          }
          break;
        }
        }
        if (!inreg_dst && I.b != B::Div && I.b != B::Mod)
          s += "  movq %rax, " + slot(I.dst) + "\n";
        break;
      }
      case K::Br:
        if (I.t != P.next[b]) // else: fall through
          s += "  jmp " + lbl(I.t) + "\n";
        break;
      case K::Cbr: {
        // emit a compare + conditional jump (fused when possible)
        auto emit_cmp = [&]() -> B {
          uint32_t x = I.a, y = I.b2;
          B op = B::Ne; // plain truth test: jne = jump when cond != 0
          if (P.fuse[i] != NONE) {
            const Instr &D = fn.code[P.fuse[i]];
            x = D.a;
            y = D.b2;
            op = D.b;
            if (is_c(x) && !is_c(y) && (op == B::Eq || op == B::Ne))
              std::swap(x, y); // commutative: immediate on the right
          }
          if (P.fuse[i] == NONE) { // truth test on the condition value
            if (hascolor(x)) {
              std::snprintf(buf, sizeof buf, "  testq %%%s, %%%s\n", rname(x),
                            rname(x));
              s += buf;
            } else {
              ld("rax", x);
              s += "  testq %rax, %rax\n";
            }
            return op;
          }
          if (hascolor(x)) { // compare in the operand's own register
            if (is_c(y) && fits32(cv(y))) {
              std::snprintf(buf, sizeof buf, "  cmpq $%lld, %%%s\n",
                            (long long)cv(y), rname(x));
              s += buf;
            } else if (is_c(y)) {
              ld("rcx", y);
              std::snprintf(buf, sizeof buf, "  cmpq %%rcx, %%%s\n", rname(x));
              s += buf;
            } else {
              std::snprintf(buf, sizeof buf, "  cmpq %s, %%%s\n",
                            loc(y).c_str(), rname(x));
              s += buf;
            }
            return op;
          }
          ld("rax", x);
          if (is_c(y) && fits32(cv(y))) {
            std::snprintf(buf, sizeof buf, "  cmpq $%lld, %%rax\n",
                          (long long)cv(y));
            s += buf;
          } else {
            ld("rcx", y);
            s += "  cmpq %rcx, %rax\n";
          }
          return op;
        };
        if (P.fuse[i] == NONE && is_c(I.a)) { // constant condition
          uint32_t tgt = (cv(I.a) != 0) ? I.t : I.f;
          if (tgt != P.next[b])
            s += "  jmp " + lbl(tgt) + "\n";
          break;
        }
        B op = emit_cmp();
        const char *jcc = jcc_of(op);
        if (I.f == P.next[b]) // false arm falls through
          s += std::string("  ") + jcc + " " + lbl(I.t) + "\n";
        else if (I.t == P.next[b]) // true arm falls through: invert
          s += std::string("  ") + jcc_inv(jcc) + " " + lbl(I.f) + "\n";
        else {
          s += std::string("  ") + jcc + " " + lbl(I.t) + "\n";
          s += "  jmp " + lbl(I.f) + "\n";
        }
        break;
      }
      case K::Print:
        ld("rsi", I.a);
        s += "  leaq .Lfmt(%rip), %rdi\n";
        s += "  xorl %eax, %eax\n  call printf\n";
        break;
      case K::Bkpt: // observation point: no code (the # line:col comment
        s += "  # breakpoint\n"; // above already locates it)
        break;
      case K::Alloc:
        ld("rdi", I.a);
        s += "  call mjz_alloc\n";
        st_rax(I.dst);
        break;
      case K::Load:
        ld("rdi", I.a);
        ld("rsi", I.b2);
        s += "  call mjz_load\n";
        st_rax(I.dst);
        break;
      case K::Store:
        ld("rdi", I.a);
        ld("rsi", I.b2);
        ld("rdx", I.c2);
        s += "  call mjz_store\n";
        break;
      case K::Len:
        ld("rdi", I.a);
        s += "  call mjz_len\n";
        st_rax(I.dst);
        break;
      case K::Input:
        s += "  call mjz_input\n";
        st_rax(I.dst);
        break;
      case K::Depth: // frames incl. main = mjz_depth + 1 (1 in main)
        if (is_main) {
          s += "  movq $1, %rax\n";
        } else {
          s += "  movq mjz_depth(%rip), %rax\n  incq %rax\n";
        }
        st_rax(I.dst);
        break;
      case K::Call: { // SysV: up to six integer registers
        static const char *argregs[6] = {"rdi", "rsi", "rdx",
                                         "rcx", "r8",  "r9"};
        for (size_t k = 0; k < I.args.size() && k < 6; ++k)
          ld(argregs[k], I.args[k]);
        s += "  call mjz_f" + std::to_string((unsigned)I.c) + "\n";
        st_rax(I.dst);
        break;
      }
      case K::Ret:
        if (I.a != NONE)
          ld("rax", I.a);
        else
          s += "  xorl %eax, %eax\n";
        if (multi)
          s += "  decq mjz_depth(%rip)\n";
        if (frame) {
          std::snprintf(buf, sizeof buf, "  addq $%llu, %%rsp\n",
                        (unsigned long long)frame);
          s += buf;
        }
        for (int32_t c = 4; c >= 0; --c)
          if (RP.usedmask & ((uint32_t)1 << c))
            s += std::string("  popq %") + RREGS[c] + "\n";
        s += "  popq %rbp\n  ret\n";
        break;
      case K::Exit:
        saw_exit = true;
        s += "  xorl %edi, %edi\n  call exit\n";
        break;
      }
    }
  }
  if (!saw_exit)
    s += "  xorl %edi, %edi\n  call exit\n";
  return s;
}

// whole-program asm: callee bodies, main, shared trap path + helpers + data.
MJZ_EMIT_FN std::string emit_x64(const std::vector<Fn> &fns,
                                 const std::vector<std::string_view> &names,
                                 std::string_view src) {
  (void)names;
  std::string s{};
  char buf[160];
  (void)buf;
  SrcMap SM{src};
  // helpers/arena are emitted only when the program uses the feature
  bool use_arr = false, use_in = false;
  for (const Fn &fn : fns)
    for (const Instr &I : fn.code) {
      if (I.k == K::Alloc || I.k == K::Load || I.k == K::Store || I.k == K::Len)
        use_arr = true;
      else if (I.k == K::Input)
        use_in = true;
    }
  const bool multi = fns.size() > 1;
  s += "# generated by minijz\n  .text\n";
  for (uint32_t f = 1; f < fns.size(); ++f) // callees first, main last
    s += emit_x64_fn(fns[f], f, false, multi, SM,
                     f < names.size() ? names[f] : std::string_view{});
  s += emit_x64_fn(fns[0], 0, true, multi, SM,
                   names.empty() ? std::string_view{} : names[0]);
  // shared trap path — self-aligning, so it is correct whether reached via
  // `call` (helpers) or via `jz`/`jmp` (main's div/mod and OOB checks)
  s += "mjz_trap:\n  pushq %rbp\n  movq %rsp, %rbp\n  andq $-16, %rsp\n";
  s += "  leaq .Ltraps(%rip), %rdi\n  call puts\n";
  s += "  movl $1, %edi\n  call exit\n";
  if (use_arr) {
    // arena helpers; the bump state must mirror the C/interp backends
    // exactly (CAP = 2^21 slots; handles are element indices)
    s += "# int64 mjz_alloc(int64 n): bump-allocate n slots, return handle\n";
    s += "mjz_alloc:\n";
    s += "  testq %rdi, %rdi\n  js mjz_trap\n"; // n < 0
    s += "  movabsq $2097152, %rdx\n  cmpq %rdx, %rdi\n  jg mjz_trap\n";
    s += "  subq %rdi, %rdx\n";                // CAP - n (no ovf)
    s += "  movq mjz_atop(%rip), %rax\n";      // h = top
    s += "  cmpq %rdx, %rax\n  jg mjz_trap\n"; // top > CAP - n
    s += "  leaq mjz_alen(%rip), %rdx\n";
    s += "  movq %rdi, (%rdx,%rax,8)\n";                     // len[h] = n
    s += "  addq %rax, %rdi\n  movq %rdi, mjz_atop(%rip)\n"; // top = h+n
    s += "  ret\n";
    s += "# int64 mjz_len(int64 h): 0 for any non-handle\n";
    s += "mjz_len:\n";
    s += "  testq %rdi, %rdi\n  js .Llen0\n";
    s += "  movabsq $2097152, %rdx\n  cmpq %rdx, %rdi\n  jg .Llen0\n";
    s += "  leaq mjz_alen(%rip), %rdx\n  movq (%rdx,%rdi,8), %rax\n  ret\n";
    s += ".Llen0:\n  xorl %eax, %eax\n  ret\n";
    s += "# int64 mjz_load(int64 h, int64 i): TRAPs unless 0 <= i < len[h]\n";
    s += "mjz_load:\n";
    s += "  testq %rdi, %rdi\n  js mjz_trap\n";
    s += "  movabsq $2097152, %rdx\n  cmpq %rdx, %rdi\n  jg mjz_trap\n";
    s += "  testq %rsi, %rsi\n  js mjz_trap\n";
    s += "  leaq mjz_alen(%rip), %rdx\n  movq (%rdx,%rdi,8), %rcx\n";
    s += "  cmpq %rcx, %rsi\n  jge mjz_trap\n";
    s += "  leaq mjz_arena(%rip), %rdx\n  addq %rdi, %rsi\n";
    s += "  movq (%rdx,%rsi,8), %rax\n  ret\n";
    s += "# void mjz_store(int64 h, int64 i, int64 v)\n";
    s += "mjz_store:\n";
    s += "  testq %rdi, %rdi\n  js mjz_trap\n";
    s += "  movabsq $2097152, %r8\n  cmpq %r8, %rdi\n  jg mjz_trap\n";
    s += "  testq %rsi, %rsi\n  js mjz_trap\n";
    s += "  leaq mjz_alen(%rip), %r9\n  movq (%r9,%rdi,8), %rcx\n";
    s += "  cmpq %rcx, %rsi\n  jge mjz_trap\n";
    s += "  leaq mjz_arena(%rip), %r9\n  addq %rdi, %rsi\n";
    s += "  movq %rdx, (%r9,%rsi,8)\n  ret\n";
  }
  if (use_in) {
    s +=
        "# int64 mjz_input(void): scanf into stack scratch; 0 on EOF/failure\n";
    s += "mjz_input:\n";
    s += "  pushq %rbx\n  subq $16, %rsp\n";
    s += "  leaq .Lscan(%rip), %rdi\n";
    s += "  movq $0, (%rsp)\n  movq %rsp, %rsi\n";
    s += "  xorl %eax, %eax\n  call scanf\n";
    s += "  movq (%rsp), %rax\n";
    s += "  addq $16, %rsp\n  popq %rbx\n  ret\n";
  }
  s += "  .section .rodata\n";
  s += ".Lfmt:\n  .string \"%lld\\n\"\n";
  s += ".Ltraps:\n  .string \"TRAP\"\n";
  if (use_in)
    s += ".Lscan:\n  .string \"%lld\"\n";
  if (use_arr) { // (CAP + 1) * 8 bytes each; handles may equal CAP
    s += "  .section .bss\n  .balign 32\n";
    s += "mjz_arena:\n  .zero 16777224\n";
    s += "mjz_alen:\n  .zero 16777224\n";
    s += "mjz_atop:\n  .zero 8\n";
  }
  if (multi) { // recursion depth counter (shared by every non-main fn)
    s += "  .section .bss\n  .balign 8\nmjz_depth:\n  .zero 8\n";
  }
  return s;
}

// ---- 13.5c. bytecode listing (disassembly)
// -----------------------------------

#undef MJZ_EMIT_FN
