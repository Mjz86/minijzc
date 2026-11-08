#pragma once
// === debugger.hpp ========================================
// --debugger: an interactive, always-answering session over the compiled
// program. No timeouts, no waiting: every command dumps state instantly.
//
//   IR side      ir / cfg / stats / plan / opt   (pipeline inspection)
//   program side vm step/run/until/break/state   (the live machine)
//
// The VM half is built on the steppable VmCtx (see back/vm.hpp): every
// slot vector, the operand stack, the arena, the input position and the
// output tape are member state, so `vm state` shows EVERYTHING and
// `vm step` moves exactly one bytecode op.

#include "minijz/apps/cli_parse.hpp"
#include "minijz/prog/program.hpp"
#include "minijz/prog/printers.hpp"
#include "minijz/back/vm.hpp"
#include "minijz/back/bytecode.hpp"
#include "minijz/back/emit_plan.hpp"
#include "minijz/runtime/astrun.hpp"
#include "minijz/opt/optimize_api.hpp"

#include <cstdio>
#include <cstring>

// ---- bytecode disassembly helpers (debugger-only pretty printing) ---------
// one op at pc: textual form + the pc just past its operands
static std::string bc_disasm_one(const std::vector<uint8_t> &c, size_t pc,
                                 size_t *next = nullptr) {
  if (pc >= c.size()) {
    if (next) *next = pc;
    return "<eof>";
  }
  Bc op = (Bc)c[pc];
  size_t p = pc + 1;
  char buf[160];
  switch (op) {
  case Bc::Push:
    std::snprintf(buf, sizeof buf, "push %lld",
                  (long long)bc_rd_i64(c, p));
    p += 8;
    break;
  case Bc::Load:
    std::snprintf(buf, sizeof buf, "load s%u", bc_rd_u32(c, p));
    p += 4;
    break;
  case Bc::Store:
    std::snprintf(buf, sizeof buf, "store s%u", bc_rd_u32(c, p));
    p += 4;
    break;
  case Bc::Jmp:
  case Bc::Jz:
    std::snprintf(buf, sizeof buf, "%s %u", op == Bc::Jmp ? "jmp" : "jz",
                  bc_rd_u32(c, p));
    p += 4;
    break;
  case Bc::Call:
    std::snprintf(buf, sizeof buf, "call fn%u n%u", bc_rd_u32(c, p),
                  bc_rd_u32(c, p + 4));
    p += 8;
    break;
  default:
    std::snprintf(buf, sizeof buf, "%s", bc_name(op));
    break;
  }
  if (next) *next = p;
  return std::string(buf);
}

struct DbgVm {
  std::vector<uint8_t> bc{};
  VmCtx S{};
  std::vector<uint32_t> bps{}; // breakpoint pcs
  bool valid{false};
};

static void dbg_vm_reset(DbgVm &D, uint64_t limit,
                         const std::vector<val_t> &ins) {
  D.S = VmCtx{};
  (void)vm_init(D.S, D.bc, limit, ins);
}

// the full-state dump: everything the machine knows, on demand
static void dbg_vm_state(DbgVm &D, const Program &P) {
  const VmCtx &S = D.S;
  if (!D.valid) {
    std::printf("(no bytecode — run `vm reset` after a successful compile)\n");
    return;
  }
  std::printf("== VM state ==\n");
  if (!S.started) {
    std::printf("not started\n");
    return;
  }
  std::string fn_name = S.fn_at(S.pc) < P.fn_names.size()
                            ? std::string(P.fn_names[S.fn_at(S.pc)])
                            : "?";
  std::printf("pc=%zu (fn %u '%s') next: %s\n", S.pc, S.fn_at(S.pc),
              fn_name.c_str(), bc_disasm_one(D.bc, S.pc).c_str());
  std::printf("steps=%llu/%llu halted=%d trapped=%d timeout=%d\n",
              (unsigned long long)S.steps, (unsigned long long)S.step_limit,
              (int)S.halted, (int)S.trapped, (int)S.timeout);
  std::printf("frames=%zu:\n", S.frames.size());
  for (size_t f = 0; f < S.frames.size(); ++f) {
    uint32_t fid = f < S.fr_fn.size() ? S.fr_fn[f] : 0;
    const char *nm = fid < P.fn_names.size()
                         ? std::string(P.fn_names[fid]).c_str() : "?";
    std::printf("  #%-2zu fn%-2u (%s) slots(%zu):", f, fid, nm,
                S.frames[f].size());
    for (size_t k = 0; k < S.frames[f].size() && k < 24; ++k)
      std::printf(" [%zu]=%lld", k, (long long)S.frames[f][k]);
    if (S.frames[f].size() > 24)
      std::printf(" ...(+%zu)", S.frames[f].size() - 24);
    std::printf("\n");
  }
  std::printf("operand stack (%zu, top last):", S.st.size());
  for (size_t k = S.st.size() > 16 ? S.st.size() - 16 : 0; k < S.st.size(); ++k)
    std::printf(" %lld", (long long)S.st[k]);
  std::printf("\n");
  // arena: contiguous allocations h=0,4,8... per a_lenv entries
  size_t allocs = 0;
  for (val_t L : S.R.a_lenv)
    if (L > 0) ++allocs;
  std::printf("arena: %zu allocs, %zu slots:", allocs, S.R.a_data.size());
  val_t h = 0;
  size_t shown = 0;
  for (val_t L : S.R.a_lenv) {
    if (L > 0) {
      std::printf(" h%lld(len %lld)=[", (long long)h, (long long)L);
      for (val_t i = 0; i < L && i < 8; ++i)
        std::printf("%s%lld", i ? " " : "",
                    (long long)S.R.a_data[(size_t)(h + i)]);
      if (L > 8) std::printf(" ...+%lld", (long long)(L - 8));
      std::printf("]");
      if (++shown >= 6) {
        std::printf(" ...(+%zu more)", allocs - shown);
        break;
      }
    }
    h += L > 0 ? L : 0;
  }
  std::printf("\n");
  std::printf("input: pos %zu/%zu", S.R.in_pos, S.R.ins.size());
  if (S.R.in_pos < S.R.ins.size())
    std::printf(" (next: %lld)", (long long)S.R.ins[S.R.in_pos]);
  std::printf("\noutput (%zu):", S.R.out.size());
  for (size_t k = 0; k < S.R.out.size() && k < 16; ++k)
    std::printf(" %lld", (long long)S.R.out[k]);
  if (S.R.out.size() > 16) std::printf(" ...(+%zu)", S.R.out.size() - 16);
  std::printf("\n");
  if (!D.bps.empty()) {
    std::printf("breakpoints:");
    for (uint32_t b : D.bps) std::printf(" %u", b);
    std::printf("\n");
  }
}

static void dbg_vm_dis(DbgVm &D, long before, long after) {
  if (!D.valid) {
    std::printf("(no bytecode)\n");
    return;
  }
  // disassemble from the START of the fn containing pc (op starts stay
  // aligned that way), then show a window of +-before/after bytes around pc
  uint32_t f = D.S.fn_at(D.S.pc);
  size_t p = D.S.fn_off(f);
  size_t hi = D.S.pc + (size_t)after;
  std::vector<std::pair<size_t, std::string>> lines{};
  while (p < D.bc.size() && p <= hi) {
    size_t at = p;
    std::string d = bc_disasm_one(D.bc, p, &p); // p advances past operands
    lines.push_back({at, d});
  }
  size_t lo_pc = D.S.pc > (size_t)before ? D.S.pc - (size_t)before : 0;
  for (const auto &[at, d] : lines)
    if (at >= lo_pc && at <= hi)
      std::printf("%s%6zu: %-18s\n", at == D.S.pc ? "->" : "  ", at,
                  d.c_str());
  std::printf("   (pc=%zu, fn %u)\n", D.S.pc, f);
}

static void dbg_help() {
  std::printf(
      "debugger commands (arguments in [] are optional):\n"
      "  help                 this text\n"
      "  src                  print the program source\n"
      "  fns                  function table (stages: ssa/opt/dead instrs)\n"
      "  ir [FN] [ssa|opt|dst|all]   dump IR (default: all fns, stage opt)\n"
      "  cfg [FN]             CFG: blocks, preds/succs (main: + dominators)\n"
      "  stats                optimization statistics summary\n"
      "  plan [FN]            emission plan (order, next, dead, kconsts)\n"
      "  bc [FN]              full bytecode listing (or one fn's range)\n"
      "  opt [FN]             re-run the optimizer live, pass by pass,\n"
      "                       IR dumped after every changing pass (--debug=2)\n"
      "  check                run all five runners and compare\n"
      "  vm reset             re-create the machine (uses --steps, tape)\n"
      "  vm step [N]          execute N bytecode ops (default 1), traced\n"
      "  vm run               run to completion (bounded by --steps)\n"
      "  vm continue [N]      run until a breakpoint / halt (max N ops)\n"
      "  vm until PC          run until pc == PC (a one-shot breakpoint)\n"
      "  vm break PC          set a breakpoint at bytecode pc\n"
      "  vm unbreak PC        remove it (vm unbreak * clears all)\n"
      "  vm breaks            list breakpoints\n"
      "  vm state             FULL machine state: pc, frames, slots,\n"
      "                       operand stack, arena, input pos, output\n"
      "  vm dis [N]           disassemble around pc (+/- N bytes)\n"
      "  quit                 leave the debugger\n");
}

int run_debugger(const Cli &C, const std::string &src) {
  Program P = compile(src, pass_cfg_of(C));
  if (!P.ok) {
    std::printf("compile error: %s (at byte %u)\n",
                P.error ? P.error : "?", (unsigned)P.errpos);
    return 1;
  }
  DbgVm D{};
  D.bc = emit_bytecode(fns_of(P, 2));
  D.valid = true;
  dbg_vm_reset(D, C.steps, C.inputs);
  std::printf(
      "=== minijz debugger — %zu fn(s), %zu bytes of bytecode, %llu step "
      "budget ===\n",
      P.fns.size(), D.bc.size(), (unsigned long long)C.steps);
  std::printf(
      "program uses input(): %s (%s); type `help` for commands\n",
      P.uses_input ? "yes" : "no",
      C.stdin_path.empty() ? "no --stdin tape, input() reads 0" :
                             ("tape: " + C.stdin_path).c_str());
  char line[4096];
  for (;;) {
    std::printf("dbg> ");
    std::fflush(stdout);
    if (!std::fgets(line, sizeof line, stdin)) break;
    std::string t(line);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' ||
                          t.back() == ' ' || t.back() == '\t'))
      t.pop_back();
    // tokenize
    std::vector<std::string> tok{};
    {
      std::string cur{};
      for (char ch : t) {
        if (ch == ' ' || ch == '\t') {
          if (!cur.empty()) tok.push_back(cur);
          cur.clear();
        } else {
          cur += ch;
        }
      }
      if (!cur.empty()) tok.push_back(cur);
    }
    if (tok.empty()) continue;
    const std::string &cmd = tok[0];
    auto num = [&](size_t i, long dflt) -> long {
      return i < tok.size() ? atol(tok[i].c_str()) : dflt;
    };
    if (cmd == "quit" || cmd == "q" || cmd == ":q") break;
    if (cmd == "help" || cmd == "h" || cmd == "?") {
      dbg_help();
    } else if (cmd == "src") {
      std::printf("%s", src.c_str());
      if (!src.empty() && src.back() != '\n') std::printf("\n");
    } else if (cmd == "fns") {
      std::printf("%zu fn(s):\n", P.fns.size());
      for (size_t f = 0; f < P.fns.size(); ++f) {
        const FnUnit &U = P.fns[f];
        std::printf("  [%zu] %-12s ssa=%4u opt=%4u dst=%4u phis=%u\n", f,
                    std::string(P.fn_names[f]).c_str(),
                    (unsigned)U.ssa.code.size(), (unsigned)U.opt.code.size(),
                    (unsigned)U.destroyed.code.size(), U.sstats.phis);
      }
    } else if (cmd == "ir") {
      long f = num(1, -1);
      std::string stage = tok.size() > 2 ? tok[2] : "opt";
      bool all_stages = stage == "all";
      auto dump = [&](size_t fi) {
        const FnUnit &U = P.fns[fi];
        char t1[96], t2[96], t3[96];
        std::snprintf(t1, sizeof t1, "SSA (before opt) [%s]",
                      std::string(P.fn_names[fi]).c_str());
        std::snprintf(t2, sizeof t2, "SSA (after opt) [%s]",
                      std::string(P.fn_names[fi]).c_str());
        std::snprintf(t3, sizeof t3, "destroyed [%s]",
                      std::string(P.fn_names[fi]).c_str());
        if (stage == "ssa" || all_stages) print_fn(U.ssa, t1);
        if (stage == "opt" || all_stages) print_fn(U.opt, t2);
        if (stage == "dst" || all_stages) print_fn(U.destroyed, t3);
      };
      if (f < 0 || f >= (long)P.fns.size())
        for (size_t fi = 0; fi < P.fns.size(); ++fi) dump(fi);
      else
        dump((size_t)f);
    } else if (cmd == "cfg") {
      long f = num(1, 0);
      if (f < 0 || f >= (long)P.fns.size()) f = 0;
      const Fn &F = P.fns[f].ssa;
      if (f == 0)
        print_pre_main_analysis(P); // dominators/loops of pre-opt main
      for (uint32_t b = 0; b < F.nblocks(); ++b) {
        std::printf("  b%u: [i%u..i%u) succ:", b, F.blocks[b].begin,
                    F.blocks[b].end);
        if (F.succ0(b) != NONE) std::printf(" b%u", F.succ0(b));
        if (F.succ1(b) != NONE) std::printf(" b%u", F.succ1(b));
        if (!F.preds.empty() && !F.preds[b].empty()) {
          std::printf(" ; preds:");
          for (uint32_t p : F.preds[b]) std::printf(" b%u", p);
        }
        std::printf("\n");
      }
    } else if (cmd == "stats") {
      OptStats os = sum_ostats(P);
      std::printf("  fns: %zu | inlined %u | unrolled %u | dce %u | "
                  "hoist %u | mem2reg %u | constcalls %u | tces %u | "
                  "scctco %u | cdfolds %u | ifconv %u | traps killed %u\n",
                  P.fns.size(), (unsigned)os.inlines, (unsigned)os.unrolls,
                  (unsigned)os.dce, (unsigned)os.hoisted,
                  (unsigned)os.mem2reg_lifts, (unsigned)os.constcalls,
                  (unsigned)os.tces, (unsigned)os.scctco,
                  (unsigned)os.cdfolds, (unsigned)os.ifconvs,
                  (unsigned)os.traps_killed);
    } else if (cmd == "plan") {
      long f = num(1, 0);
      if (f < 0 || f >= (long)P.fns.size()) f = 0;
      const Fn &F = P.fns[f].destroyed;
      EmitPlan PL = make_emit_plan(F);
      std::printf("== fn %ld [%s] ==\norder:", f,
                  std::string(P.fn_names[f]).c_str());
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
    } else if (cmd == "bc") {
      long f = num(1, -1);
      if (f < 0) {
        std::printf("%s", emit_bc_listing(D.bc).c_str());
      } else {
        // list the ops of one fn (from its offset to the next fn's)
        uint32_t nf = D.S.nfns();
        if ((long)nf <= f) {
          std::printf("no fn %ld\n", f);
        } else {
          uint32_t lo = D.S.fn_off((uint32_t)f);
          uint32_t hi = (f + 1 < (long)nf) ? D.S.fn_off((uint32_t)f + 1)
                                           : (uint32_t)D.bc.size();
          size_t p = lo;
          while (p < hi) {
            std::printf("%s%6zu: %-18s\n", "  ", p,
                        bc_disasm_one(D.bc, p, &p).c_str());
          }
        }
      }
    } else if (cmd == "opt") {
      long f = num(1, 0);
      if (f < 0 || f >= (long)P.fns.size()) f = 0;
      Fn live = P.fns[f].ssa; // fresh copy: the pipeline runs on the copy
      OptStats os{};
      PassCfg cfg = pass_cfg_of(C);
      cfg.debug = 2; // one line per pass + IR after each changing pass
      cfg.dbg_name = P.fn_names[f];
      std::printf("== live optimize: fn %ld [%s] (from pre-opt SSA) ==\n", f,
                  std::string(P.fn_names[f]).c_str());
      (void)optimize_light(live, os, cfg);
      std::printf("== live optimize done (%u folds, %u dce, %u inlined into "
                  "this view) ==\n",
                  (unsigned)os.folds, (unsigned)os.dce, 0u);
    } else if (cmd == "check") {
      RunOut oracle = run_ast(P.P.ast, P.P.root, C.steps, C.inputs);
      RunOut plain = run_prog(fns_of(P, 0), C.steps, C.inputs);
      RunOut optimized = run_prog(fns_of(P, 1), C.steps, C.inputs);
      RunOut lowered = run_prog(fns_of(P, 2), C.steps, C.inputs);
      std::vector<uint8_t> bc = emit_bytecode(fns_of(P, 2));
      RunOut vm = vm_run(bc, C.steps, C.inputs);
      const char *names[] = {"oracle", "ssa", "opt", "lower", "vm"};
      RunOut vals[5] = {oracle, plain, optimized, lowered, vm};
      for (int k = 0; k < 5; ++k) {
        std::printf("%-8s: ", names[k]);
        print_run(vals[k]);
      }
      bool ok = same_run(oracle, plain) && same_run(oracle, optimized) &&
                same_run(oracle, lowered) && same_run(oracle, vm);
      std::printf("%s\n", ok ? "ALL AGREE" : "*** MISMATCH ***");
    } else if (cmd == "vm") {
      const std::string &sub = tok.size() > 1 ? tok[1] : "";
      if (sub == "reset") {
        dbg_vm_reset(D, C.steps, C.inputs);
        std::printf("machine reset: pc=%zu, %u fn(s), %zu slots in main\n",
                    D.S.pc, D.S.nfns(), D.S.frames.empty() ? 0 : D.S.frames[0].size());
      } else if (sub == "step") {
        long n = num(2, 1);
        if (n < 1) n = 1;
        for (long k = 0; k < n; ++k) {
          if (D.S.halted || D.S.trapped || D.S.timeout) break;
          size_t at = D.S.pc;
          std::string d = bc_disasm_one(D.bc, at);
          bool alive = vm_step_op(D.S);
          std::printf("  %6zu: %-18s ; stack(%zu)", at, d.c_str(),
                      D.S.st.size());
          if (!D.S.st.empty())
            std::printf(" top=%lld", (long long)D.S.st.back());
          std::printf("\n");
          if (!alive) break;
        }
        if (D.S.halted)
          std::printf("  HALTED after %llu steps\n",
                      (unsigned long long)D.S.steps);
        if (D.S.trapped) std::printf("  TRAPPED at pc=%zu\n", D.S.pc);
        if (D.S.timeout) std::printf("  TIMEOUT (step limit)\n");
      } else if (sub == "run" || sub == "continue") {
        long maxops = num(2, 1000000000L);
        uint64_t ops = 0;
        bool bp_hit = false;
        while (ops < (uint64_t)maxops && vm_step_op(D.S)) {
          ++ops;
          for (uint32_t bp : D.bps)
            if (D.S.pc == bp) {
              std::printf("breakpoint %u hit after %llu ops (pc=%zu)\n", bp,
                          (unsigned long long)ops, D.S.pc);
              bp_hit = true;
              break;
            }
          if (bp_hit) break;
        }
        std::printf(
            "%llu ops executed: pc=%zu steps=%llu%s%s%s\n",
            (unsigned long long)ops, D.S.pc, (unsigned long long)D.S.steps,
            D.S.halted ? " HALTED" : "", D.S.trapped ? " TRAPPED" : "",
            D.S.timeout ? " TIMEOUT" : "");
        if (sub == "run") {
          std::printf("output (%zu):", D.S.R.out.size());
          for (val_t v : D.S.R.out) std::printf(" %lld", (long long)v);
          std::printf("\n");
        }
      } else if (sub == "until") {
        long target = num(2, -1);
        if (target < 0) {
          std::printf("usage: vm until PC\n");
        } else {
          uint64_t ops = 0;
          while (vm_step_op(D.S)) {
            ++ops;
            if (D.S.pc == (size_t)target) break;
            if (D.S.halted || D.S.trapped || D.S.timeout) break;
          }
          std::printf("stopped at pc=%zu (target %ld) after %llu ops%s%s%s\n",
                      D.S.pc, target, (unsigned long long)ops,
                      D.S.halted ? " HALTED" : "",
                      D.S.trapped ? " TRAPPED" : "",
                      D.S.timeout ? " TIMEOUT" : "");
        }
      } else if (sub == "break") {
        long target = num(2, -1);
        if (target >= 0) {
          D.bps.push_back((uint32_t)target);
          std::printf("breakpoint set at pc %ld\n", target);
        } else {
          std::printf("usage: vm break PC\n");
        }
      } else if (sub == "unbreak") {
        if (tok.size() > 2 && tok[2] == "*") {
          D.bps.clear();
          std::printf("all breakpoints cleared\n");
        } else {
          long target = num(2, -1);
          for (size_t i = 0; i < D.bps.size(); ++i)
            if (D.bps[i] == (uint32_t)target) {
              D.bps.erase(D.bps.begin() + (long)i);
              std::printf("breakpoint %ld removed\n", target);
              break;
            }
        }
      } else if (sub == "breaks") {
        if (D.bps.empty()) std::printf("no breakpoints\n");
        for (uint32_t bp : D.bps)
          std::printf("  %u: %s\n", bp, bc_disasm_one(D.bc, bp).c_str());
      } else if (sub == "state") {
        dbg_vm_state(D, P);
      } else if (sub == "dis") {
        dbg_vm_dis(D, num(2, 16), num(2, 32));
      } else {
        std::printf("vm subcommand? try: reset step run continue until "
                    "break unbreak breaks state dis\n");
      }
    } else {
      std::printf("unknown command '%s' (try `help`)\n", cmd.c_str());
    }
  }
  std::printf("(debugger session ended)\n");
  return 0;
}
