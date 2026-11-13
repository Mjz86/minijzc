#pragma once
// === dbg_vm.hpp ========================================
// the --debugger MACHINE half (`vm` subcommands), split out of
// debugger.hpp: reset / step (traced, with breakpoint observation) /
// run / continue / until / break / line / unbreak / breaks / state /
// dis — each subcommand a small function over the session.

#include "dbg_sess.hpp"

// ---- vm step [N]: execute N ops, traced ------------------------------------
// one op: the trace line + what a language breakpoint observes there
static void dbg_vm_show_bkpt(DbgSess &S, size_t at) {
  const DbgBk *B = S.bkpt_at(at);
  if (!B || S.D.S.frames.empty())
    return;
  const std::vector<val_t> &fr = S.D.S.frames.back();
  std::printf(" ; ");
  for (size_t j = 0; j < B->slots.size(); ++j) {
    if (j)
      std::printf(", ");
    std::string nm = j < B->names.size() && !B->names[j].empty()
                         ? B->names[j]
                         : ("slot " + std::to_string(B->slots[j]));
    val_t v = B->slots[j] < fr.size() ? fr[B->slots[j]] : (val_t)0;
    std::printf("%s=%s", nm.c_str(),
                dbg_val(S.D.S, v, j < B->arry.size() && B->arry[j]).c_str());
  }
}
static bool dbg_vm_step_one(DbgSess &S) {
  size_t at = S.D.S.pc;
  std::string d = bc_disasm_one(S.D.bc, at);
  bool alive = vm_step_op(S.D.S);
  std::printf("  %6zu: %-18s ; %s stack(%zu)", at, d.c_str(),
              S.loc_str(at).c_str(), S.D.S.st.size());
  if (!S.D.S.st.empty())
    std::printf(" top=%lld", (long long)S.D.S.st.back());
  if (S.D.bc[at] == (uint8_t)Bc::Bkpt) // show what it observes
    dbg_vm_show_bkpt(S, at);
  std::printf("\n");
  return alive;
}
static void dbg_vm_stop_msgs(DbgSess &S) {
  if (S.D.S.halted)
    std::printf("  HALTED after %llu steps\n", (unsigned long long)S.D.S.steps);
  if (S.D.S.trapped)
    std::printf("  TRAPPED at pc=%zu (%s)\n", S.D.S.pc,
                S.loc_str(S.D.S.pc).c_str());
  if (S.D.S.timeout)
    std::printf("  TIMEOUT (step limit)\n");
}
static void dbg_vm_step(DbgSess &S, const std::vector<std::string> &tok) {
  long n = dbg_num(tok, 2, 1);
  if (n < 1)
    n = 1;
  for (long k = 0; k < n; ++k) {
    if (S.D.S.halted || S.D.S.trapped || S.D.S.timeout)
      break;
    if (!dbg_vm_step_one(S))
      break;
  }
  dbg_vm_stop_msgs(S);
}

// ---- vm run / vm continue [N] ----------------------------------------------
static void dbg_vm_run(DbgSess &S, const std::vector<std::string> &tok,
                       const std::string &sub) {
  long maxops = dbg_num(tok, 2, 1000000000L);
  S.run_bounded((uint64_t)maxops);
  if (sub == "run") {
    std::printf("output (%zu):", S.D.S.R.out.size());
    for (val_t v : S.D.S.R.out)
      std::printf(" %lld", (long long)v);
    std::printf("\n");
  }
}

// ---- vm until PC: a one-shot breakpoint ------------------------------------
static void dbg_vm_until(DbgSess &S, const std::vector<std::string> &tok) {
  long target = dbg_num(tok, 2, -1);
  if (target < 0) {
    std::printf("usage: vm until PC\n");
    return;
  }
  uint64_t ops = 0;
  while (vm_step_op(S.D.S)) {
    ++ops;
    if (S.D.S.pc == (size_t)target)
      break;
    if (S.D.S.halted || S.D.S.trapped || S.D.S.timeout)
      break;
  }
  std::printf("stopped at pc=%zu (target %ld) after %llu ops%s%s%s\n", S.D.S.pc,
              target, (unsigned long long)ops, S.D.S.halted ? " HALTED" : "",
              S.D.S.trapped ? " TRAPPED" : "", S.D.S.timeout ? " TIMEOUT" : "");
}

// ---- vm break PC / vm line L ------------------------------------------------
static void dbg_vm_break(DbgSess &S, const std::vector<std::string> &tok) {
  long target = dbg_num(tok, 2, -1);
  if (target >= 0) {
    S.D.bps.push_back((uint32_t)target);
    std::printf("breakpoint set at pc %ld (%s)\n", target,
                S.loc_str((size_t)target).c_str());
  } else {
    std::printf("usage: vm break PC (or vm line L for a source line)\n");
  }
}
// set a breakpoint at every op start of source line L; returns how many
static size_t dbg_line_bps(DbgSess &S, long L) {
  size_t added = 0;
  // walk op starts (operands share their op's source offset)
  if (S.D.bc.size() >= 4) {
    uint32_t nf = bc_rd_u32(S.D.bc, 0);
    size_t p = 4 + (size_t)8 * nf;
    while (p < S.D.bc.size()) {
      if ((long)S.line_at(p) == L) {
        S.D.bps.push_back((uint32_t)p);
        ++added;
      }
      (void)bc_disasm_one(S.D.bc, p, &p);
    }
  }
  return added;
}
static void dbg_vm_line(DbgSess &S, const std::vector<std::string> &tok) {
  long L = dbg_num(tok, 2, -1);
  if (L <= 0) {
    std::printf("usage: vm line L (source line — see `src`)\n");
    return;
  }
  size_t added = dbg_line_bps(S, L);
  if (added)
    std::printf("%zu breakpoint(s) set on line %ld (`vm breaks`, "
                "`vm continue`)\n",
                added, L);
  else
    std::printf("no bytecode on line %ld (see `src` for numbering)\n", L);
}

// ---- vm unbreak PC|* / vm breaks
// ---------------------------------------------
static void dbg_vm_unbreak(DbgSess &S, const std::vector<std::string> &tok) {
  if (tok.size() > 2 && tok[2] == "*") {
    S.D.bps.clear();
    std::printf("all breakpoints cleared\n");
    return;
  }
  long target = dbg_num(tok, 2, -1);
  for (size_t i = 0; i < S.D.bps.size(); ++i)
    if (S.D.bps[i] == (uint32_t)target) {
      S.D.bps.erase(S.D.bps.begin() + (long)i);
      std::printf("breakpoint %ld removed\n", target);
      break;
    }
}
static void dbg_vm_breaks(DbgSess &S) {
  if (S.D.bps.empty())
    std::printf("no manual breakpoints (%zu language breakpoint(s))\n",
                S.bkpts.size());
  for (uint32_t bp : S.D.bps)
    std::printf("  %u: %s (%s)\n", bp, bc_disasm_one(S.D.bc, bp).c_str(),
                S.loc_str(bp).c_str());
}

// vm reset/step/run/continue/until/break/line/unbreak/breaks/state/dis
static void dbg_cmd_vm(DbgSess &S, const std::vector<std::string> &tok) {
  const std::string &sub = tok.size() > 1 ? tok[1] : "";
  if (sub == "reset") {
    dbg_vm_reset(S.D, S.C.steps, S.C.inputs);
    S.wlast.clear(); // watch baselines die with the machine state
    S.wlast_fn = NONE;
    std::printf("machine reset: pc=%zu, %u fn(s), %zu slots in main\n",
                S.D.S.pc, S.D.S.nfns(),
                S.D.S.frames.empty() ? 0 : S.D.S.frames[0].size());
  } else if (sub == "step") {
    dbg_vm_step(S, tok);
  } else if (sub == "run" || sub == "continue") {
    dbg_vm_run(S, tok, sub);
  } else if (sub == "until") {
    dbg_vm_until(S, tok);
  } else if (sub == "break") {
    dbg_vm_break(S, tok);
  } else if (sub == "line") {
    dbg_vm_line(S, tok);
  } else if (sub == "unbreak") {
    dbg_vm_unbreak(S, tok);
  } else if (sub == "breaks") {
    dbg_vm_breaks(S);
  } else if (sub == "state") {
    dbg_vm_state(S.D, S.P);
  } else if (sub == "dis") {
    dbg_vm_dis(S.D, dbg_num(tok, 2, 16), dbg_num(tok, 2, 32));
  } else {
    std::printf("vm subcommand? try: reset step run continue until "
                "break line unbreak breaks state dis\n");
  }
}
