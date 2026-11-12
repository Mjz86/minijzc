#pragma once
// === debugger.hpp ========================================
// --debugger: an interactive, always-answering session over the compiled
// program. No timeouts, no waiting: every command dumps state instantly.
//
//   IR side      ir / cfg / stats / plan / opt   (pipeline inspection)
//   program side vm step/run/until/break/state   (the live machine)
//
// Checkpoint 16 broke the former 1200-line monolith into layers; the
// code-quality continuation finishes the job (every function small,
// every file focused):
//   dbg_support.hpp  machines, breakpoint scan, naming, snapshots
//   dbg_sess.hpp     the SESSION state (DbgSess) + the watchpoint and
//                    stop engines, decomposed into small methods
//   dbg_cmds.hpp     src/fns/ir/cfg/stats/plan/bc/bkpts/vars/watch/check
//   dbg_opt.hpp      the pass explorer (`opt` and its subcommands)
//   dbg_vm.hpp       the machine half (`vm` subcommands)
//   this file        the help text, the banner, tokenize + dispatch,
//                    run_debugger()'s thin loop
//
// The VM half is built on the steppable VmCtx (see back/vm.hpp); the
// breakpoint semantics live with DbgSess::run_bounded (dbg_sess.hpp).

#include "dbg_cmds.hpp"
#include "dbg_opt.hpp"
#include "dbg_vm.hpp"

// ---- help -------------------------------------------------------------------
static void dbg_help() {
  std::printf(
      "debugger commands (arguments in [] are optional):\n"
      "  help                 this text\n"
      "  src [N]              print the program source (with line numbers;\n"
      "                       N: just that line)\n"
      "  fns                  function table (stages: ssa/opt/dead instrs)\n"
      "  ir [FN] [ssa|opt|dst|all]   dump IR (default: all fns, stage opt)\n"
      "  cfg [FN]             CFG: blocks, preds/succs (main: + dominators)\n"
      "  stats                optimization statistics summary\n"
      "  plan [FN]            emission plan (order, next, dead, kconsts)\n"
      "  bc [FN]              full bytecode listing (or one fn's range)\n"
      "  bkpts                language breakpoints (`breakpoint x;` stmts):\n"
      "                       pc, fn, source line, observed values now\n"
      "  vars [FN]            named slots of the current (or given) fn with\n"
      "                       live values — what survived optimization\n"
      "  watch NAME           add a variable to show at every stop\n"
      "  unwatch NAME|*       remove one watch / all\n"
      "  opt [FN]             pass explorer: re-run the pipeline on a copy,\n"
      "                       snapshot after every pass, print the timeline\n"
      "  opt show [N]         full IR of snapshot N (default: last)\n"
      "  opt diff [A [B]]     unified IR diff between snapshots (names or\n"
      "                       indices; default: first vs last)\n"
      "  opt break PASS       breakpoint BETWEEN passes: show PASS's snapshot\n"
      "                       and its diff against the previous one, and ARM\n"
      "                       it — the next `opt [FN]` PAUSES after that pass\n"
      "  opt continue         disarm the pass breakpoint + collect in full\n"
      "  opt list             the pass toggle table (on/off for this run)\n"
      "  check                run all five runners and compare\n"
      "  vm reset             re-create the machine (uses --steps, tape)\n"
      "  vm step [N]          execute N bytecode ops (default 1), traced\n"
      "                       with source line annotations\n"
      "  vm run               run to completion (bounded by --steps)\n"
      "  vm continue [N]      run until a breakpoint / halt (max N ops);\n"
      "                       language breakpoints stop here automatically\n"
      "  vm until PC          run until pc == PC (a one-shot breakpoint)\n"
      "  vm break PC          set a breakpoint at bytecode pc\n"
      "  vm line L            set breakpoints at every pc of source line L\n"
      "  vm unbreak PC|*      remove it (vm unbreak * clears all)\n"
      "  vm breaks            list breakpoints\n"
      "  vm state             FULL machine state: pc, frames, slots,\n"
      "                       operand stack, arena, input pos, output\n"
      "  vm dis [N]           disassemble around pc (+/- N bytes)\n"
      "  quit                 leave the debugger\n");
}

// ---- the session loop
// --------------------------------------------------------
static std::vector<std::string> dbg_tokenize(const std::string &t) {
  std::vector<std::string> tok{};
  std::string cur{};
  for (char ch : t) {
    if (ch == ' ' || ch == '\t') {
      if (!cur.empty())
        tok.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  if (!cur.empty())
    tok.push_back(cur);
  return tok;
}
static void dbg_banner(DbgSess &S) {
  std::printf(
      "=== minijz debugger — %zu fn(s), %zu bytes of bytecode, %llu step "
      "budget ===\n",
      S.P.fns.size(), S.D.bc.size(), (unsigned long long)S.C.steps);
  if (!S.bkpts.empty())
    std::printf("program has %zu language breakpoint(s): `bkpts` lists them; "
                "`vm continue` stops there\n",
                S.bkpts.size());
  std::printf("program uses input(): %s (%s); type `help` for commands\n",
              S.P.uses_input ? "yes" : "no",
              S.C.stdin_path.empty() ? "no --stdin tape, input() reads 0"
                                     : ("tape: " + S.C.stdin_path).c_str());
}
static void dbg_dispatch(DbgSess &S, const std::vector<std::string> &tok) {
  const std::string &cmd = tok[0];
  if (cmd == "help" || cmd == "h" || cmd == "?")
    dbg_help();
  else if (cmd == "src")
    dbg_cmd_src(S, tok);
  else if (cmd == "fns")
    dbg_cmd_fns(S);
  else if (cmd == "ir")
    dbg_cmd_ir(S, tok);
  else if (cmd == "cfg")
    dbg_cmd_cfg(S, tok);
  else if (cmd == "stats")
    dbg_cmd_stats(S);
  else if (cmd == "plan")
    dbg_cmd_plan(S, tok);
  else if (cmd == "bc")
    dbg_cmd_bc(S, tok);
  else if (cmd == "bkpts")
    dbg_cmd_bkpts(S);
  else if (cmd == "vars")
    dbg_cmd_vars(S, tok);
  else if (cmd == "watch" || cmd == "unwatch")
    dbg_cmd_watch(S, tok);
  else if (cmd == "opt")
    dbg_cmd_opt(S, tok);
  else if (cmd == "check")
    dbg_cmd_check(S);
  else if (cmd == "vm")
    dbg_cmd_vm(S, tok);
  else
    std::printf("unknown command '%s' (try `help`)\n", cmd.c_str());
}
int run_debugger(const Cli &C, const std::string &src) {
  Program P = compile(src, pass_cfg_of(C));
  if (!P.ok) {
    print_compile_error(src, P.error, P.errpos, C.file.c_str());
    return 1;
  }
  DbgSess S(C, src, std::move(P));
  S.D.bc = emit_bytecode(fns_of(S.P, 2), &S.pcsrc);
  S.D.valid = true;
  S.bkpts = dbg_scan_bkpts(S.D.bc, S.P, S.pcsrc);
  dbg_vm_reset(S.D, C.steps, C.inputs);
  dbg_banner(S);
  char line[4096];
  for (;;) {
    std::printf("dbg> ");
    std::fflush(stdout);
    if (!std::fgets(line, sizeof line, stdin))
      break;
    std::string t(line);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' ||
                          t.back() == ' ' || t.back() == '\t'))
      t.pop_back();
    std::vector<std::string> tok = dbg_tokenize(t);
    if (tok.empty())
      continue;
    if (tok[0] == "quit" || tok[0] == "q" || tok[0] == ":q")
      break;
    dbg_dispatch(S, tok);
  }
  std::printf("(debugger session ended)\n");
  return 0;
}
