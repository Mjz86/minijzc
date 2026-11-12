#pragma once
// === dbg_cmds.hpp ========================================
// the --debugger COMMAND handlers (one small function per family):
// src / fns / ir / cfg / stats / plan / bc / bkpts / vars / watch /
// check. The pass explorer (`opt`) lives in dbg_opt.hpp, the machine
// half (`vm`) in dbg_vm.hpp; the session state (DbgSess) in
// dbg_sess.hpp; dispatch in debugger.hpp.

#include "dbg_sess.hpp"

// ---- src [N]: numbered source (the line numbers feed `vm line`) -----------
static void dbg_src_row(DbgSess &S, uint32_t ln, const std::string &row,
                        long only) {
  std::printf("%s%4u | %.*s\n",
              (long)ln == S.line_at(S.D.S.pc) && !only ? "->" : "  ",
              (unsigned)ln, (int)row.size(), row.data());
}
static void dbg_cmd_src(DbgSess &S, const std::vector<std::string> &tok) {
  long only = dbg_num(tok, 1, 0);
  uint32_t ln = 1;
  size_t p0 = 0;
  while (p0 <= S.src.size()) {
    size_t pe = S.src.find('\n', p0);
    std::string row =
        S.src.substr(p0, pe == std::string::npos ? S.src.size() - p0 : pe - p0);
    if (!only || (long)ln == only) {
      dbg_src_row(S, ln, row, only);
      if (only)
        break;
    }
    if (pe == std::string::npos)
      break;
    p0 = pe + 1;
    ++ln;
  }
}

// ---- fns: function table ---------------------------------------------------
static void dbg_cmd_fns(DbgSess &S) {
  std::printf("%zu fn(s):\n", S.P.fns.size());
  for (size_t f = 0; f < S.P.fns.size(); ++f) {
    const FnUnit &U = S.P.fns[f];
    std::printf("  [%zu] %-12s ssa=%4u opt=%4u dst=%4u phis=%u\n", f,
                std::string(S.P.fn_names[f]).c_str(),
                (unsigned)U.ssa.code.size(), (unsigned)U.opt.code.size(),
                (unsigned)U.destroyed.code.size(), U.sstats.phis);
  }
}

// ---- ir [FN] [ssa|opt|dst|all] ---------------------------------------------
static void dbg_ir_dump(DbgSess &S, size_t fi, const std::string &stage,
                        bool all_stages) {
  const FnUnit &U = S.P.fns[fi];
  char t1[96], t2[96], t3[96];
  std::snprintf(t1, sizeof t1, "SSA (before opt) [%s]",
                std::string(S.P.fn_names[fi]).c_str());
  std::snprintf(t2, sizeof t2, "SSA (after opt) [%s]",
                std::string(S.P.fn_names[fi]).c_str());
  std::snprintf(t3, sizeof t3, "destroyed [%s]",
                std::string(S.P.fn_names[fi]).c_str());
  if (stage == "ssa" || all_stages)
    print_fn(U.ssa, t1, S.P.src);
  if (stage == "opt" || all_stages)
    print_fn(U.opt, t2, S.P.src);
  if (stage == "dst" || all_stages)
    print_fn(U.destroyed, t3, S.P.src);
}
static void dbg_cmd_ir(DbgSess &S, const std::vector<std::string> &tok) {
  long f = dbg_num(tok, 1, -1);
  std::string stage = tok.size() > 2 ? tok[2] : "opt";
  bool all_stages = stage == "all";
  if (f < 0 || f >= (long)S.P.fns.size())
    for (size_t fi = 0; fi < S.P.fns.size(); ++fi)
      dbg_ir_dump(S, fi, stage, all_stages);
  else
    dbg_ir_dump(S, (size_t)f, stage, all_stages);
}

// ---- cfg [FN]: CFG blocks/preds/succs (main: + the pre-opt analysis) -------
static void dbg_cfg_block(const Fn &F, uint32_t b) {
  std::printf("  b%u: [i%u..i%u) succ:", b, F.blocks[b].begin, F.blocks[b].end);
  if (F.succ0(b) != NONE)
    std::printf(" b%u", F.succ0(b));
  if (F.succ1(b) != NONE)
    std::printf(" b%u", F.succ1(b));
  if (!F.preds.empty() && !F.preds[b].empty()) {
    std::printf(" ; preds:");
    for (uint32_t p : F.preds[b])
      std::printf(" b%u", p);
  }
  std::printf("\n");
}
static void dbg_cmd_cfg(DbgSess &S, const std::vector<std::string> &tok) {
  long f = dbg_num(tok, 1, 0);
  if (f < 0 || f >= (long)S.P.fns.size())
    f = 0;
  const Fn &F = S.P.fns[f].ssa;
  if (f == 0)
    print_pre_main_analysis(S.P); // dominators/loops of pre-opt main
  for (uint32_t b = 0; b < F.nblocks(); ++b)
    dbg_cfg_block(F, b);
}

// ---- stats: optimization statistics summary --------------------------------
static void dbg_cmd_stats(DbgSess &S) {
  OptStats os = sum_ostats(S.P);
  std::printf("  fns: %zu | inlined %u | unrolled %u | dce %u | "
              "hoist %u | mem2reg %u | constcalls %u | tces %u | "
              "scctco %u | cdfolds %u | ifconv %u | traps killed %u | "
              "deadargs %u | fnspec %u | icf %u | jthread %u | argpromo %u | "
              "gvn %u\n",
              S.P.fns.size(), (unsigned)os.inlines, (unsigned)os.unrolls,
              (unsigned)os.dce, (unsigned)os.hoisted,
              (unsigned)os.mem2reg_lifts, (unsigned)os.constcalls,
              (unsigned)os.tces, (unsigned)os.scctco, (unsigned)os.cdfolds,
              (unsigned)os.ifconvs, (unsigned)os.traps_killed,
              (unsigned)os.deadargs, (unsigned)os.fnspecs, (unsigned)os.icf,
              (unsigned)os.jthreads, (unsigned)os.argpromos, (unsigned)os.gvns);
}

// ---- plan [FN]: emission plan (order, next, dead, kconsts) ------------------
static void dbg_plan_instr(const Fn &F, const EmitPlan &PL, uint32_t b,
                           uint32_t i) {
  const Instr &I = F.code[i];
  std::printf("i%u b%u k=%d dst=%u dead=%d kc=%d uses=%u\n", i, b, (int)I.k,
              I.dst, PL.dead[i] ? 1 : 0,
              (I.dst < PL.kc.size()) ? PL.kc[I.dst] : -1,
              (I.dst < PL.uses.size()) ? PL.uses[I.dst] : 0u);
}
static void dbg_cmd_plan(DbgSess &S, const std::vector<std::string> &tok) {
  long f = dbg_num(tok, 1, 0);
  if (f < 0 || f >= (long)S.P.fns.size())
    f = 0;
  const Fn &F = S.P.fns[f].destroyed;
  EmitPlan PL = make_emit_plan(F);
  std::printf("== fn %ld [%s] ==\norder:", f,
              std::string(S.P.fn_names[f]).c_str());
  for (uint32_t b2 : PL.order)
    std::printf(" b%u(nxt=b%u)", b2,
                PL.next[b2] == ~uint32_t(0) ? 999u : PL.next[b2]);
  std::printf("\n");
  for (uint32_t b = 0; b < F.nblocks(); ++b)
    for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i)
      dbg_plan_instr(F, PL, b, i);
}

// ---- bc [FN]: full bytecode listing (or one fn's range) --------------------
static void dbg_bc_fn_range(DbgSess &S, long f) {
  // list the ops of one fn (from its offset to the next fn's)
  uint32_t nf = S.D.S.nfns();
  if ((long)nf <= f) {
    std::printf("no fn %ld\n", f);
    return;
  }
  uint32_t lo = S.D.S.fn_off((uint32_t)f);
  uint32_t hi = (f + 1 < (long)nf) ? S.D.S.fn_off((uint32_t)f + 1)
                                   : (uint32_t)S.D.bc.size();
  for (size_t p = lo; p < hi;)
    std::printf("%s%6zu: %-18s\n", "  ", p,
                bc_disasm_one(S.D.bc, p, &p).c_str());
}
static void dbg_cmd_bc(DbgSess &S, const std::vector<std::string> &tok) {
  long f = dbg_num(tok, 1, -1);
  if (f < 0)
    std::printf("%s", emit_bc_listing(S.D.bc).c_str());
  else
    dbg_bc_fn_range(S, f);
}

// ---- bkpts: the language breakpoint table ----------------------------------
static std::string dbg_bkpt_obs(const DbgBk &B) {
  std::string obs{};
  for (size_t j = 0; j < B.names.size(); ++j) {
    if (j)
      obs += ", ";
    obs += B.names[j].empty() ? ("slot " + std::to_string(B.slots[j]))
                              : B.names[j];
  }
  return obs;
}
static void dbg_cmd_bkpts(DbgSess &S) {
  if (S.bkpts.empty())
    std::printf("no language breakpoints (write `breakpoint x;` in the "
                "source)\n");
  for (size_t k = 0; k < S.bkpts.size(); ++k) {
    const DbgBk &B = S.bkpts[k];
    char lb[48];
    const char *loc = S.SM.at(B.src, lb, sizeof lb) ? lb : "?";
    std::string fnn = B.fn < S.P.fn_names.size()
                          ? std::string(S.P.fn_names[B.fn])
                          : std::string("?");
    std::string obs = dbg_bkpt_obs(B);
    std::printf("  #%zu pc=%-6zu fn %-8s %-6s observes: %s\n", k, B.pc,
                fnn.c_str(), loc, obs.empty() ? "(nothing)" : obs.c_str());
  }
}

// ---- vars [FN]: named slots with live values — what survived opt -----------
// observed names from language breakpoints enrich the table (a value may
// live in a temp's slot after copy-propagation)
static void dbg_vars_enrich(DbgSess &S, std::vector<std::string> &names,
                            uint32_t f) {
  for (const DbgBk &B : S.bkpts)
    if (B.fn == f)
      for (size_t j = 0; j < B.slots.size() && j < B.names.size() &&
                         B.slots[j] < names.size();
           ++j)
        if (!B.names[j].empty() && names[B.slots[j]].empty())
          names[B.slots[j]] = B.names[j];
}
static uint32_t dbg_vars_fn(DbgSess &S, const std::vector<std::string> &tok) {
  long f = dbg_num(tok, 1, -1);
  if (f >= 0 && f < (long)S.P.fns.size())
    return (uint32_t)f;
  return S.D.S.fn_at(S.D.S.pc);
}
static void dbg_cmd_vars(DbgSess &S, const std::vector<std::string> &tok) {
  if (!S.D.S.started) {
    std::printf("(machine not started — vm reset / vm step first)\n");
    return;
  }
  uint32_t f = dbg_vars_fn(S, tok);
  std::vector<std::string> names = dbg_slot_names(S.P, f);
  std::vector<char> mh = dbg_slot_may_handle(S.P, f);
  dbg_vars_enrich(S, names, f);
  const std::vector<val_t> &fr =
      S.D.S.frames.empty() ? std::vector<val_t>{} : S.D.S.frames.back();
  std::string fnn =
      f < S.P.fn_names.size() ? std::string(S.P.fn_names[f]) : std::string("?");
  std::printf("named slots of fn %u (%s) — what survived optimization — at "
              "%s:\n",
              (unsigned)f, fnn.c_str(), S.loc_str(S.D.S.pc).c_str());
  size_t shown = 0;
  for (size_t s = 0; s < names.size() && s < fr.size(); ++s) {
    if (names[s].empty())
      continue; // temps: not user-visible
    std::printf("    %-12s = %s\n", names[s].c_str(),
                dbg_val(S.D.S, fr[s], s < mh.size() && mh[s]).c_str());
    ++shown;
  }
  if (!shown)
    std::printf("    (no named slots — every variable optimized to "
                "constants / registers)\n");
}

// ---- watch NAME / unwatch NAME|* -------------------------------------------
static void dbg_watch_add(DbgSess &S, const std::string &name) {
  for (const std::string &w : S.watches)
    if (w == name) {
      std::printf("already watching %s\n", name.c_str());
      return;
    }
  S.watches.push_back(name);
  std::printf("watching %s (shown at every stop; `vm continue` also stops "
              "when it changes)\n",
              name.c_str());
}
static void dbg_watch_del(DbgSess &S, const std::string &name) {
  for (size_t i = 0; i < S.watches.size(); ++i)
    if (S.watches[i] == name) {
      S.watches.erase(S.watches.begin() + (long)i);
      std::printf("unwatched %s\n", name.c_str());
      return;
    }
}
static void dbg_cmd_watch(DbgSess &S, const std::vector<std::string> &tok) {
  const std::string &cmd = tok[0];
  if (tok.size() < 2) {
    std::printf("usage: %s NAME\n", cmd.c_str());
    return;
  }
  if (cmd == "watch")
    dbg_watch_add(S, tok[1]);
  else if (tok[1] == "*") {
    S.watches.clear();
    std::printf("all watches cleared\n");
  } else
    dbg_watch_del(S, tok[1]);
}

// ---- check: all five runners + comparison ----------------------------------
static void dbg_check_row(const char *name, const RunOut &r,
                          std::string_view src) {
  std::printf("%-8s: ", name);
  print_run(r, src);
}
static void dbg_cmd_check(DbgSess &S) {
  RunOut oracle = run_ast(S.P.P.ast, S.P.P.root, S.C.steps, S.C.inputs);
  RunOut plain = run_prog(fns_of(S.P, 0), S.C.steps, S.C.inputs);
  RunOut optimized = run_prog(fns_of(S.P, 1), S.C.steps, S.C.inputs);
  RunOut lowered = run_prog(fns_of(S.P, 2), S.C.steps, S.C.inputs);
  std::vector<uint8_t> bc = emit_bytecode(fns_of(S.P, 2));
  RunOut vm = vm_run(bc, S.C.steps, S.C.inputs);
  const char *names[] = {"oracle", "ssa", "opt", "lower", "vm"};
  RunOut vals[5] = {oracle, plain, optimized, lowered, vm};
  for (int k = 0; k < 5; ++k)
    dbg_check_row(names[k], vals[k], S.P.src);
  bool ok = same_run(oracle, plain) && same_run(oracle, optimized) &&
            same_run(oracle, lowered) && same_run(oracle, vm);
  std::printf("%s\n", ok ? "ALL AGREE" : "*** MISMATCH ***");
}
