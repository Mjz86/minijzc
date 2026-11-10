#pragma once
// === debugger.hpp ========================================
// --debugger: an interactive, always-answering session over the compiled
// program. No timeouts, no waiting: every command dumps state instantly.
//
//   IR side      ir / cfg / stats / plan / opt   (pipeline inspection)
//   program side vm step/run/until/break/state   (the live machine)
//
// Checkpoint 16 broke the former 1200-line monolith into three layers:
//   dbg_support.hpp  machines, breakpoint scan, naming, snapshots (above)
//   this file        the SESSION state (DbgSess) + the command handlers,
//                    each a small static function over the session
//   run_debugger()   the thin read-tokenize-dispatch loop
//
// The VM half is built on the steppable VmCtx (see back/vm.hpp); the
// breakpoint semantics live with DbgSess::run_bounded below.

#include "dbg_support.hpp"

// ---- session
// ----------------------------------------------------------------- one
// --debugger invocation's whole mutable state. The former run_debugger locals
// (machine, source map, breakpoint tables, watches, snapshots) plus the little
// helpers that closed over them, promoted to methods.
struct DbgSess {
  const Cli &C;
  const std::string &src;
  Program P;
  DbgVm D;
  SrcMap SM;
  std::vector<uint32_t> pcsrc; // per-byte source offsets (line bps, steps)
  std::vector<DbgBk> bkpts;    // the language breakpoints (breakpoint x;)
  std::vector<std::string> watches;
  // value-change watchpoint state: last-seen value per watched name
  // (per current fn; switching fns refreshes silently)
  std::vector<std::pair<std::string, val_t>> wlast{};
  uint32_t wlast_fn{NONE};
  std::vector<Snap> snaps; // the `opt` explorer's snapshot stack
  size_t snap_fn{0};

  DbgSess(const Cli &c, const std::string &source, Program prog)
      : C(c), src(source), P(std::move(prog)), SM(P.src) {}

  uint32_t line_at(size_t pc) const {
    if (pc >= pcsrc.size() || pcsrc[pc] == NONE)
      return 0;
    return SM.line(pcsrc[pc]);
  }
  std::string loc_str(size_t pc) const {
    uint32_t l = line_at(pc);
    char buf[32];
    if (!l)
      return std::string("?");
    std::snprintf(buf, sizeof buf, "line %u", (unsigned)l);
    return std::string(buf);
  }
  const DbgBk *bkpt_at(size_t pc) const {
    for (const DbgBk &B : bkpts)
      if (B.pc == pc)
        return &B;
    return nullptr;
  }
  // report a stop at a language breakpoint: observed values + watches
  void report_bkpt(const DbgBk &B) {
    char lb[48];
    const char *loc = SM.at(B.src, lb, sizeof lb) ? lb : "?";
    std::string fnn = B.fn < P.fn_names.size() ? std::string(P.fn_names[B.fn])
                                               : std::string("?");
    std::printf("* breakpoint at %s in fn %u (%s) [pc %zu]\n", loc,
                (unsigned)B.fn, fnn.c_str(), B.pc);
    // the CURRENT frame is B.fn's frame (pc is inside it)
    if (!D.S.frames.empty()) {
      const std::vector<val_t> &fr = D.S.frames.back();
      for (size_t k = 0; k < B.slots.size(); ++k) {
        std::string nm = k < B.names.size() && !B.names[k].empty()
                             ? B.names[k]
                             : ("slot " + std::to_string(B.slots[k]));
        val_t v = B.slots[k] < fr.size() ? fr[B.slots[k]] : (val_t)0;
        std::printf("    %-10s = %s\n", nm.c_str(),
                    dbg_val(D.S, v, k < B.arry.size() && B.arry[k]).c_str());
      }
    }
  }
  void report_watches() {
    if (watches.empty() || D.S.frames.empty() || !D.S.started)
      return;
    uint32_t f = D.S.fn_at(D.S.pc);
    std::vector<std::string> names = dbg_slot_names(P, f);
    std::vector<char> mh = dbg_slot_may_handle(P, f);
    const std::vector<val_t> &fr = D.S.frames.back();
    for (const std::string &w : watches) {
      bool found = false;
      for (size_t s = 0; s < names.size() && s < fr.size(); ++s)
        if (names[s] == w) {
          std::printf("    watch %-10s = %s\n", w.c_str(),
                      dbg_val(D.S, fr[s], s < mh.size() && mh[s]).c_str());
          found = true;
          break;
        }
      if (!found)
        std::printf("    watch %-10s = <not live in fn %u>\n", w.c_str(),
                    (unsigned)f);
    }
  }
  // value-change watchpoints: resolve every watched name in the CURRENT
  // fn's frame and compare against the last-seen values. Returns true (and
  // prints the change) when any watched value changed; switching to a
  // different fn refreshes the snapshot silently (slots are per-fn).
  bool watch_step() {
    if (watches.empty() || D.S.frames.empty() || !D.S.started)
      return false;
    uint32_t f = D.S.fn_at(D.S.pc);
    if (f >= P.fns.size())
      return false;
    if (f != wlast_fn) { // fn switch: re-baseline, no report
      wlast_fn = f;
      wlast.clear();
      std::vector<std::string> names = dbg_slot_names(P, f);
      const std::vector<val_t> &fr = D.S.frames.back();
      for (const std::string &w : watches)
        for (size_t s = 0; s < names.size() && s < fr.size(); ++s)
          if (names[s] == w) {
            wlast.push_back({w, fr[s]});
            break;
          }
      return false;
    }
    std::vector<std::string> names = dbg_slot_names(P, f);
    const std::vector<val_t> &fr = D.S.frames.back();
    std::vector<char> mh = dbg_slot_may_handle(P, f);
    bool changed = false;
    for (const std::string &w : watches) {
      val_t now = 0;
      long slot = -1;
      for (size_t s = 0; s < names.size() && s < fr.size(); ++s)
        if (names[s] == w) {
          now = fr[s];
          slot = (long)s;
          break;
        }
      val_t before = 0;
      bool seen = false;
      for (auto &kv : wlast)
        if (kv.first == w) {
          before = kv.second;
          seen = true;
          break;
        }
      if ((seen && before != now) || (!seen && slot >= 0)) {
        std::printf(
            "* watch %s changed: %s -> %s [pc %zu, %s]\n", w.c_str(),
            seen ? dbg_val(D.S, before, false).c_str() : "<not live>",
            slot >= 0
                ? dbg_val(D.S, now, slot < (long)mh.size() && mh[slot]).c_str()
                : "<not live>",
            D.S.pc, loc_str(D.S.pc).c_str());
        changed = true;
      }
      if (slot >= 0) { // refresh the baseline
        bool had = false;
        for (auto &kv : wlast)
          if (kv.first == w) {
            kv.second = now;
            had = true;
          }
        if (!had)
          wlast.push_back({w, now});
      } else {
        for (size_t k = 0; k < wlast.size();)
          if (wlast[k].first == w)
            wlast.erase(wlast.begin() + (long)k);
          else
            ++k;
      }
    }
    return changed;
  }
  // the run/continue stop engine: stops BEFORE executing the op at pc when
  // it is a manual bp, a line bp or a language breakpoint; `resume_pc`
  // exempts the pc we stopped at for exactly one op (classic `continue`)
  void run_bounded(uint64_t maxops) {
    uint64_t ops = 0;
    auto stop_at = [&](size_t pc) -> int { // 0 none, 1 manual, 2 language
      if (pc >= D.bc.size())
        return 0;
      for (uint32_t bp : D.bps)
        if (pc == bp)
          return 1;
      if (D.bc[pc] == (uint8_t)Bc::Bkpt)
        return 2;
      return 0;
    };
    if (D.S.pc == D.resume_pc) {
      D.resume_pc = (size_t)-1; // resuming from THIS stop: run the op once
    } else {
      int st = stop_at(D.S.pc);
      if (st == 2) {
        report_bkpt(*bkpt_at(D.S.pc));
        report_watches();
        D.resume_pc = D.S.pc;
        return;
      }
      if (st == 1) {
        std::printf("breakpoint %zu hit (pc=%zu, %s)\n", D.S.pc, D.S.pc,
                    loc_str(D.S.pc).c_str());
        report_watches();
        D.resume_pc = D.S.pc;
        return;
      }
    }
    while (ops < maxops && vm_step_op(D.S)) {
      ++ops;
      if (D.S.halted || D.S.trapped || D.S.timeout)
        break;
      if (watch_step()) { // value-change watchpoint fired
        report_watches();
        return;
      }
      if (D.S.pc == D.resume_pc) { // just executed the resumed stop op
        D.resume_pc = (size_t)-1;
        continue;
      }
      int st = stop_at(D.S.pc);
      if (st == 2) {
        report_bkpt(*bkpt_at(D.S.pc));
        report_watches();
        D.resume_pc = D.S.pc;
        return;
      }
      if (st == 1) {
        std::printf("breakpoint %zu hit after %llu ops (pc=%zu, %s)\n", D.S.pc,
                    (unsigned long long)ops, D.S.pc, loc_str(D.S.pc).c_str());
        report_watches();
        D.resume_pc = D.S.pc;
        return;
      }
    }
    std::printf("%llu ops executed: pc=%zu steps=%llu%s%s%s\n",
                (unsigned long long)ops, D.S.pc, (unsigned long long)D.S.steps,
                D.S.halted ? " HALTED" : "", D.S.trapped ? " TRAPPED" : "",
                D.S.timeout ? " TIMEOUT" : "");
  }
};

// token[i] as a number, or `dflt` when absent/garbage
static long dbg_num(const std::vector<std::string> &tok, size_t i, long dflt) {
  return i < tok.size() ? atol(tok[i].c_str()) : dflt;
}

// ---- the command handlers (one per command family)
// ---------------------------

// src [N]: numbered source (the line numbers feed `vm line`)
static void dbg_cmd_src(DbgSess &S, const std::vector<std::string> &tok) {
  long only = dbg_num(tok, 1, 0);
  uint32_t ln = 1;
  size_t p0 = 0;
  while (p0 <= S.src.size()) {
    size_t pe = S.src.find('\n', p0);
    std::string row =
        S.src.substr(p0, pe == std::string::npos ? S.src.size() - p0 : pe - p0);
    if (!only || (long)ln == only) {
      std::printf("%s%4u | %.*s\n",
                  (long)ln == S.line_at(S.D.S.pc) && !only ? "->" : "  ",
                  (unsigned)ln, (int)row.size(), row.data());
      if (only)
        break;
    }
    if (pe == std::string::npos)
      break;
    p0 = pe + 1;
    ++ln;
  }
}

// fns: function table
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

// ir [FN] [ssa|opt|dst|all]
static void dbg_cmd_ir(DbgSess &S, const std::vector<std::string> &tok) {
  long f = dbg_num(tok, 1, -1);
  std::string stage = tok.size() > 2 ? tok[2] : "opt";
  bool all_stages = stage == "all";
  auto dump = [&](size_t fi) {
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
  };
  if (f < 0 || f >= (long)S.P.fns.size())
    for (size_t fi = 0; fi < S.P.fns.size(); ++fi)
      dump(fi);
  else
    dump((size_t)f);
}

// cfg [FN]: CFG blocks/preds/succs (main: + the pre-opt analysis)
static void dbg_cmd_cfg(DbgSess &S, const std::vector<std::string> &tok) {
  long f = dbg_num(tok, 1, 0);
  if (f < 0 || f >= (long)S.P.fns.size())
    f = 0;
  const Fn &F = S.P.fns[f].ssa;
  if (f == 0)
    print_pre_main_analysis(S.P); // dominators/loops of pre-opt main
  for (uint32_t b = 0; b < F.nblocks(); ++b) {
    std::printf("  b%u: [i%u..i%u) succ:", b, F.blocks[b].begin,
                F.blocks[b].end);
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
}

// stats: optimization statistics summary
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

// plan [FN]: emission plan (order, next, dead, kconsts)
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
    for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
      const Instr &I = F.code[i];
      std::printf("i%u b%u k=%d dst=%u dead=%d kc=%d uses=%u\n", i, b, (int)I.k,
                  I.dst, PL.dead[i] ? 1 : 0,
                  (I.dst < PL.kc.size()) ? PL.kc[I.dst] : -1,
                  (I.dst < PL.uses.size()) ? PL.uses[I.dst] : 0u);
    }
}

// bc [FN]: full bytecode listing (or one fn's range)
static void dbg_cmd_bc(DbgSess &S, const std::vector<std::string> &tok) {
  long f = dbg_num(tok, 1, -1);
  if (f < 0) {
    std::printf("%s", emit_bc_listing(S.D.bc).c_str());
  } else {
    // list the ops of one fn (from its offset to the next fn's)
    uint32_t nf = S.D.S.nfns();
    if ((long)nf <= f) {
      std::printf("no fn %ld\n", f);
    } else {
      uint32_t lo = S.D.S.fn_off((uint32_t)f);
      uint32_t hi = (f + 1 < (long)nf) ? S.D.S.fn_off((uint32_t)f + 1)
                                       : (uint32_t)S.D.bc.size();
      size_t p = lo;
      while (p < hi) {
        std::printf("%s%6zu: %-18s\n", "  ", p,
                    bc_disasm_one(S.D.bc, p, &p).c_str());
      }
    }
  }
}

// bkpts: the language breakpoint table
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
    std::string obs{};
    for (size_t j = 0; j < B.names.size(); ++j) {
      if (j)
        obs += ", ";
      obs += B.names[j].empty() ? ("slot " + std::to_string(B.slots[j]))
                                : B.names[j];
    }
    std::printf("  #%zu pc=%-6zu fn %-8s %-6s observes: %s\n", k, B.pc,
                fnn.c_str(), loc, obs.empty() ? "(nothing)" : obs.c_str());
  }
}

// vars [FN]: named slots with live values — what survived optimization
static void dbg_cmd_vars(DbgSess &S, const std::vector<std::string> &tok) {
  if (!S.D.S.started) {
    std::printf("(machine not started — vm reset / vm step first)\n");
    return;
  }
  uint32_t f =
      dbg_num(tok, 1, -1) >= 0 && dbg_num(tok, 1, -1) < (long)S.P.fns.size()
          ? (uint32_t)dbg_num(tok, 1, -1)
          : S.D.S.fn_at(S.D.S.pc);
  std::vector<std::string> names = dbg_slot_names(S.P, f);
  std::vector<char> mh = dbg_slot_may_handle(S.P, f);
  // observed names from language breakpoints enrich the table (a value may
  // live in a temp's slot after copy-propagation)
  for (const DbgBk &B : S.bkpts)
    if (B.fn == f)
      for (size_t j = 0; j < B.slots.size() && j < B.names.size() &&
                         B.slots[j] < names.size();
           ++j)
        if (!B.names[j].empty() && names[B.slots[j]].empty())
          names[B.slots[j]] = B.names[j];
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

// watch NAME / unwatch NAME|*
static void dbg_cmd_watch(DbgSess &S, const std::vector<std::string> &tok) {
  const std::string &cmd = tok[0];
  if (tok.size() < 2) {
    std::printf("usage: %s NAME\n", cmd.c_str());
    return;
  }
  if (cmd == "watch") {
    for (const std::string &w : S.watches)
      if (w == tok[1]) {
        std::printf("already watching %s\n", tok[1].c_str());
        return;
      }
    S.watches.push_back(tok[1]);
    std::printf("watching %s (shown at every stop; `vm continue` also stops "
                "when it changes)\n",
                tok[1].c_str());
  } else if (tok[1] == "*") {
    S.watches.clear();
    std::printf("all watches cleared\n");
  } else {
    for (size_t i = 0; i < S.watches.size(); ++i)
      if (S.watches[i] == tok[1]) {
        S.watches.erase(S.watches.begin() + (long)i);
        std::printf("unwatched %s\n", tok[1].c_str());
        return;
      }
  }
}

// check: all five runners + comparison
static void dbg_cmd_check(DbgSess &S) {
  RunOut oracle = run_ast(S.P.P.ast, S.P.P.root, S.C.steps, S.C.inputs);
  RunOut plain = run_prog(fns_of(S.P, 0), S.C.steps, S.C.inputs);
  RunOut optimized = run_prog(fns_of(S.P, 1), S.C.steps, S.C.inputs);
  RunOut lowered = run_prog(fns_of(S.P, 2), S.C.steps, S.C.inputs);
  std::vector<uint8_t> bc = emit_bytecode(fns_of(S.P, 2));
  RunOut vm = vm_run(bc, S.C.steps, S.C.inputs);
  const char *names[] = {"oracle", "ssa", "opt", "lower", "vm"};
  RunOut vals[5] = {oracle, plain, optimized, lowered, vm};
  for (int k = 0; k < 5; ++k) {
    std::printf("%-8s: ", names[k]);
    print_run(vals[k], S.P.src);
  }
  bool ok = same_run(oracle, plain) && same_run(oracle, optimized) &&
            same_run(oracle, lowered) && same_run(oracle, vm);
  std::printf("%s\n", ok ? "ALL AGREE" : "*** MISMATCH ***");
}

// ---- the pass explorer (`opt` and its subcommands)
// ---------------------------

// opt [FN] | show [N] | diff [A [B]] | break PASS | list
static void dbg_cmd_opt(DbgSess &S, const std::vector<std::string> &tok) {
  const std::string &sub = tok.size() > 1 ? tok[1] : "";
  auto snap_idx = [&](const std::string &s,
                      long dflt) -> long { // name or index
    if (!s.empty() && (s[0] >= 'a' && s[0] <= 'z')) {
      for (size_t k = 0; k < S.snaps.size(); ++k)
        if (S.snaps[k].name == s)
          return (long)k;
      return -1;
    }
    return s.empty() ? dflt : atol(s.c_str());
  };
  if (sub == "show") {
    if (S.snaps.empty()) {
      std::printf("(no snapshots yet — run `opt [FN]` first)\n");
    } else {
      long n = tok.size() > 2 ? snap_idx(tok[2], (long)S.snaps.size() - 1)
                              : (long)S.snaps.size() - 1;
      if (n < 0 || n >= (long)S.snaps.size())
        std::printf("no snapshot #%ld (0..%zu)\n", n, S.snaps.size() - 1);
      else
        std::printf("%s", S.snaps[(size_t)n].ir.c_str());
    }
  } else if (sub == "diff") {
    if (S.snaps.size() < 2) {
      std::printf("(need at least 2 snapshots — run `opt [FN]` first)\n");
    } else {
      long a = tok.size() > 2 ? snap_idx(tok[2], 0)
                              : 0; // default: overall (first vs last)
      long b = tok.size() > 3
                   ? snap_idx(tok[3], (long)S.snaps.size() - 1)
                   : (tok.size() > 2 ? a + 1 : (long)S.snaps.size() - 1);
      if (a < 0 || b < 0 || a >= (long)S.snaps.size() ||
          b >= (long)S.snaps.size() || a == b) {
        std::printf("usage: opt diff [A [B]] (snapshots 0..%zu)\n",
                    S.snaps.size() - 1);
      } else {
        std::printf("== diff: #%ld (%s) -> #%ld (%s) ==\n", a,
                    S.snaps[(size_t)a].name.c_str(), b,
                    S.snaps[(size_t)b].name.c_str());
        std::string d =
            unified_diff(S.snaps[(size_t)a].ir, S.snaps[(size_t)b].ir);
        std::printf("%s", d.empty() ? "  (identical)\n" : d.c_str());
      }
    }
  } else if (sub == "break") {
    // a breakpoint BETWEEN passes: locate PASS, show where the pipeline
    // stands and what that pass itself changed
    if (S.snaps.empty()) {
      std::printf("(no snapshots yet — run `opt [FN]` first)\n");
    } else if (tok.size() < 3) {
      std::printf("usage: opt break PASS (e.g. opt break sccp)\n");
    } else {
      long k = snap_idx(tok[2], -1);
      if (k < 0) {
        std::printf("no snapshot named '%s' (see `opt` timeline)\n",
                    tok[2].c_str());
      } else {
        std::printf("* stopped after pass #%ld [%s] (fn %zu [%s])\n", k,
                    S.snaps[(size_t)k].name.c_str(), S.snap_fn,
                    std::string(S.P.fn_names[S.snap_fn]).c_str());
        if (k > 0) {
          std::string d =
              unified_diff(S.snaps[(size_t)k - 1].ir, S.snaps[(size_t)k].ir);
          std::printf("%s", d.empty() ? "  (this pass changed nothing)\n"
                                      : d.c_str());
        } else {
          std::printf("  (#0 is the baseline — nothing precedes it)\n");
        }
        std::printf("(opt show %ld dumps the full IR at this point; "
                    "opt diff for other pairs)\n",
                    k);
      }
    }
  } else if (sub == "list") {
    PassCfg cfg = pass_cfg_of(S.C);
    const bool states[PASS_TOGGLE_COUNT] = {
        cfg.p_fold,     cfg.p_copyprop, cfg.p_canon,     cfg.p_cse,
        cfg.p_unreach,  cfg.p_splice,   cfg.p_dce,       cfg.p_mem2reg,
        cfg.p_trapelim, cfg.p_licm,     cfg.p_divmagic,  cfg.p_reassoc,
        cfg.p_unroll,   cfg.p_lcf,      cfg.p_constcall, cfg.p_scctco,
        cfg.p_tailrec,  cfg.p_cdfold,   cfg.p_ifconv,    cfg.do_inline,
        cfg.p_sccp,     cfg.p_ldelim,   cfg.p_callcse,   cfg.p_deadargs,
        cfg.p_fnspec,   cfg.p_icf,      cfg.p_jthread,   cfg.p_argpromo,
        cfg.p_gvn,      cfg.p_unswitch, cfg.p_lsimp};
    std::printf("pass toggle table (%u names, -O%u):\n",
                (unsigned)PASS_TOGGLE_COUNT, (unsigned)cfg.level);
    for (uint32_t i = 0; i < PASS_TOGGLE_COUNT; ++i)
      std::printf("  %-10s %s%s", PASS_TOGGLE_NAMES[i],
                  states[i] ? "on " : "OFF", (i % 4) == 3 ? "\n" : "   ");
    if (PASS_TOGGLE_COUNT % 4)
      std::printf("\n");
  } else if (!sub.empty() && sub != "run") {
    std::printf("opt subcommand? try: opt [FN] | show | diff | break | "
                "list\n");
  } else {
    long f = sub.empty() ? dbg_num(tok, 1, 0) : atol(sub.c_str());
    if (f < 0 || f >= (long)S.P.fns.size())
      f = 0;
    S.snaps.clear();
    S.snap_fn = (size_t)f;
    if (!dbg_collect_snaps(S.P, (size_t)f, S.C, S.snaps)) {
      std::printf("fn %ld: no snapshots (-O0 leaves the IR untouched)\n", f);
    } else {
      dbg_print_timeline(S.snaps, (size_t)f, S.P);
      std::printf("(opt show N | opt diff A B | opt break PASS to "
                  "inspect)\n");
    }
  }
}

// ---- the machine half (`vm` subcommands)
// --------------------------------------

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
    long n = dbg_num(tok, 2, 1);
    if (n < 1)
      n = 1;
    for (long k = 0; k < n; ++k) {
      if (S.D.S.halted || S.D.S.trapped || S.D.S.timeout)
        break;
      size_t at = S.D.S.pc;
      std::string d = bc_disasm_one(S.D.bc, at);
      bool alive = vm_step_op(S.D.S);
      std::printf("  %6zu: %-18s ; %s stack(%zu)", at, d.c_str(),
                  S.loc_str(at).c_str(), S.D.S.st.size());
      if (!S.D.S.st.empty())
        std::printf(" top=%lld", (long long)S.D.S.st.back());
      if (S.D.bc[at] == (uint8_t)Bc::Bkpt) { // show what it observes
        const DbgBk *B = S.bkpt_at(at);
        if (B && !S.D.S.frames.empty()) {
          const std::vector<val_t> &fr = S.D.S.frames.back();
          std::printf(" ; ");
          for (size_t j = 0; j < B->slots.size(); ++j) {
            if (j)
              std::printf(", ");
            std::string nm = j < B->names.size() && !B->names[j].empty()
                                 ? B->names[j]
                                 : ("slot " + std::to_string(B->slots[j]));
            val_t v = B->slots[j] < fr.size() ? fr[B->slots[j]] : (val_t)0;
            std::printf(
                "%s=%s", nm.c_str(),
                dbg_val(S.D.S, v, j < B->arry.size() && B->arry[j]).c_str());
          }
        }
      }
      std::printf("\n");
      if (!alive)
        break;
    }
    if (S.D.S.halted)
      std::printf("  HALTED after %llu steps\n",
                  (unsigned long long)S.D.S.steps);
    if (S.D.S.trapped)
      std::printf("  TRAPPED at pc=%zu (%s)\n", S.D.S.pc,
                  S.loc_str(S.D.S.pc).c_str());
    if (S.D.S.timeout)
      std::printf("  TIMEOUT (step limit)\n");
  } else if (sub == "run" || sub == "continue") {
    long maxops = dbg_num(tok, 2, 1000000000L);
    S.run_bounded((uint64_t)maxops);
    if (sub == "run") {
      std::printf("output (%zu):", S.D.S.R.out.size());
      for (val_t v : S.D.S.R.out)
        std::printf(" %lld", (long long)v);
      std::printf("\n");
    }
  } else if (sub == "until") {
    long target = dbg_num(tok, 2, -1);
    if (target < 0) {
      std::printf("usage: vm until PC\n");
    } else {
      uint64_t ops = 0;
      while (vm_step_op(S.D.S)) {
        ++ops;
        if (S.D.S.pc == (size_t)target)
          break;
        if (S.D.S.halted || S.D.S.trapped || S.D.S.timeout)
          break;
      }
      std::printf(
          "stopped at pc=%zu (target %ld) after %llu ops%s%s%s\n", S.D.S.pc,
          target, (unsigned long long)ops, S.D.S.halted ? " HALTED" : "",
          S.D.S.trapped ? " TRAPPED" : "", S.D.S.timeout ? " TIMEOUT" : "");
    }
  } else if (sub == "break") {
    long target = dbg_num(tok, 2, -1);
    if (target >= 0) {
      S.D.bps.push_back((uint32_t)target);
      std::printf("breakpoint set at pc %ld (%s)\n", target,
                  S.loc_str((size_t)target).c_str());
    } else {
      std::printf("usage: vm break PC (or vm line L for a source line)\n");
    }
  } else if (sub == "line") {
    long L = dbg_num(tok, 2, -1);
    if (L <= 0) {
      std::printf("usage: vm line L (source line — see `src`)\n");
    } else {
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
      if (added)
        std::printf("%zu breakpoint(s) set on line %ld (`vm breaks`, "
                    "`vm continue`)\n",
                    added, L);
      else
        std::printf("no bytecode on line %ld (see `src` for numbering)\n", L);
    }
  } else if (sub == "unbreak") {
    if (tok.size() > 2 && tok[2] == "*") {
      S.D.bps.clear();
      std::printf("all breakpoints cleared\n");
    } else {
      long target = dbg_num(tok, 2, -1);
      for (size_t i = 0; i < S.D.bps.size(); ++i)
        if (S.D.bps[i] == (uint32_t)target) {
          S.D.bps.erase(S.D.bps.begin() + (long)i);
          std::printf("breakpoint %ld removed\n", target);
          break;
        }
    }
  } else if (sub == "breaks") {
    if (S.D.bps.empty())
      std::printf("no manual breakpoints (%zu language breakpoint(s))\n",
                  S.bkpts.size());
    for (uint32_t bp : S.D.bps)
      std::printf("  %u: %s (%s)\n", bp, bc_disasm_one(S.D.bc, bp).c_str(),
                  S.loc_str(bp).c_str());
  } else if (sub == "state") {
    dbg_vm_state(S.D, S.P);
  } else if (sub == "dis") {
    dbg_vm_dis(S.D, dbg_num(tok, 2, 16), dbg_num(tok, 2, 32));
  } else {
    std::printf("vm subcommand? try: reset step run continue until "
                "break line unbreak breaks state dis\n");
  }
}

// ---- help + the session loop
// ---------------------------------------------------

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
      "                       and its diff against the previous one\n"
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
  std::printf(
      "=== minijz debugger — %zu fn(s), %zu bytes of bytecode, %llu step "
      "budget ===\n",
      S.P.fns.size(), S.D.bc.size(), (unsigned long long)C.steps);
  if (!S.bkpts.empty())
    std::printf("program has %zu language breakpoint(s): `bkpts` lists them; "
                "`vm continue` stops there\n",
                S.bkpts.size());
  std::printf("program uses input(): %s (%s); type `help` for commands\n",
              S.P.uses_input ? "yes" : "no",
              C.stdin_path.empty() ? "no --stdin tape, input() reads 0"
                                   : ("tape: " + C.stdin_path).c_str());

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
    // tokenize
    std::vector<std::string> tok{};
    {
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
    }
    if (tok.empty())
      continue;
    const std::string &cmd = tok[0];
    if (cmd == "quit" || cmd == "q" || cmd == ":q")
      break;
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
  std::printf("(debugger session ended)\n");
  return 0;
}
