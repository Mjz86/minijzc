#pragma once
// === dbg_sess.hpp ========================================
// the --debugger SESSION layer, split out of debugger.hpp (the
// code-quality continuation: break the big functions, break the big
// files): the one-session mutable state (DbgSess) and the little
// engines that close over it -- the watchpoint machinery and the
// run/continue stop engine, each decomposed into small methods. The
// command families live in dbg_cmds.hpp / dbg_opt.hpp / dbg_vm.hpp;
// the session loop and dispatch stay in debugger.hpp.
//
// The VM half is built on the steppable VmCtx (see back/vm.hpp): every
// slot vector, the operand stack, the arena, the input position and the
// output tape are member state, so `vm state` shows EVERYTHING and
// `vm step` moves exactly one bytecode op.

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
  // the armed pass breakpoint (checkpoint 18): `opt break PASS` arms
  // it; the next `opt [FN]` collection PAUSES after that pass's first
  // snapshot (the timeline is truncated there, the pass's diff is
  // shown); `opt continue` disarms and re-collects in full
  std::string pass_break{};

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
    for (const std::string &w : watches)
      if (!watch_row(names, mh, fr, w))
        std::printf("    watch %-10s = <not live in fn %u>\n", w.c_str(),
                    (unsigned)f);
  }
  // one watched name's current value row; false when not live here
  bool watch_row(const std::vector<std::string> &names,
                 const std::vector<char> &mh, const std::vector<val_t> &fr,
                 const std::string &w) {
    for (size_t s = 0; s < names.size() && s < fr.size(); ++s)
      if (names[s] == w) {
        std::printf("    watch %-10s = %s\n", w.c_str(),
                    dbg_val(D.S, fr[s], s < mh.size() && mh[s]).c_str());
        return true;
      }
    return false;
  }

  // ---- value-change watchpoints --------------------------------------
  // resolve every watched name in the CURRENT fn's frame and compare
  // against the last-seen values. Returns true (and prints the change)
  // when any watched value changed; switching to a different fn
  // refreshes the snapshot silently (slots are per-fn).
  long watch_slot_of(const std::vector<std::string> &names,
                     const std::vector<val_t> &fr, const std::string &w,
                     val_t &now) const {
    for (size_t s = 0; s < names.size() && s < fr.size(); ++s)
      if (names[s] == w) {
        now = fr[s];
        return (long)s;
      }
    return -1;
  }
  val_t watch_last(const std::string &w, bool &seen) const {
    for (const auto &kv : wlast)
      if (kv.first == w) {
        seen = true;
        return kv.second;
      }
    return 0;
  }
  // fn switch: re-baseline every watch, no report
  void watch_rebaseline(const std::vector<std::string> &names,
                        const std::vector<val_t> &fr) {
    wlast.clear();
    for (const std::string &w : watches) {
      val_t now{};
      if (watch_slot_of(names, fr, w, now) >= 0)
        wlast.push_back({w, now});
    }
  }
  void watch_report(const std::string &w, bool seen, val_t before, val_t now,
                    long slot, const std::vector<char> &mh) {
    std::printf(
        "* watch %s changed: %s -> %s [pc %zu, %s]\n", w.c_str(),
        seen ? dbg_val(D.S, before, false).c_str() : "<not live>",
        slot >= 0
            ? dbg_val(D.S, now, slot < (long)mh.size() && mh[slot]).c_str()
            : "<not live>",
        D.S.pc, loc_str(D.S.pc).c_str());
  }
  // refresh (or drop, when not live anymore) w's baseline
  void watch_refresh(const std::string &w, val_t now, long slot) {
    if (slot < 0) {
      for (size_t k = 0; k < wlast.size();)
        if (wlast[k].first == w)
          wlast.erase(wlast.begin() + (long)k);
        else
          ++k;
      return;
    }
    bool had = false;
    for (auto &kv : wlast)
      if (kv.first == w) {
        kv.second = now;
        had = true;
      }
    if (!had)
      wlast.push_back({w, now});
  }
  bool watch_step() {
    if (watches.empty() || D.S.frames.empty() || !D.S.started)
      return false;
    uint32_t f = D.S.fn_at(D.S.pc);
    if (f >= P.fns.size())
      return false;
    std::vector<std::string> names = dbg_slot_names(P, f);
    const std::vector<val_t> &fr = D.S.frames.back();
    if (f != wlast_fn) { // fn switch: re-baseline, no report
      wlast_fn = f;
      watch_rebaseline(names, fr);
      return false;
    }
    std::vector<char> mh = dbg_slot_may_handle(P, f);
    bool changed = false;
    for (const std::string &w : watches) {
      val_t now = 0;
      long slot = watch_slot_of(names, fr, w, now);
      bool seen = false;
      val_t before = watch_last(w, seen);
      if ((seen && before != now) || (!seen && slot >= 0)) {
        watch_report(w, seen, before, now, slot, mh);
        changed = true;
      }
      watch_refresh(w, now, slot);
    }
    return changed;
  }

  // ---- the run/continue stop engine -----------------------------------
  // stops BEFORE executing the op at pc when it is a manual bp, a line
  // bp or a language breakpoint; `resume_pc` exempts the pc we stopped
  // at for exactly one op (classic `continue`)
  int stop_at(size_t pc) const { // 0 none, 1 manual, 2 language
    if (pc >= D.bc.size())
      return 0;
    for (uint32_t bp : D.bps)
      if (pc == bp)
        return 1;
    if (D.bc[pc] == (uint8_t)Bc::Bkpt)
      return 2;
    return 0;
  }
  // the shared stop report: the language bp's observed values or the
  // manual-bp line (with the ops count once running), then the watches;
  // arms the resume exemption
  void stop_report(int st, uint64_t ops) {
    if (st == 2)
      report_bkpt(*bkpt_at(D.S.pc));
    else if (ops)
      std::printf("breakpoint %zu hit after %llu ops (pc=%zu, %s)\n", D.S.pc,
                  (unsigned long long)ops, D.S.pc, loc_str(D.S.pc).c_str());
    else
      std::printf("breakpoint %zu hit (pc=%zu, %s)\n", D.S.pc, D.S.pc,
                  loc_str(D.S.pc).c_str());
    report_watches();
    D.resume_pc = D.S.pc;
  }
  void run_bounded(uint64_t maxops) {
    uint64_t ops = 0;
    if (D.S.pc == D.resume_pc) {
      D.resume_pc = (size_t)-1; // resuming from THIS stop: run the op once
    } else {
      int st = stop_at(D.S.pc);
      if (st) {
        stop_report(st, ops);
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
      if (st) {
        stop_report(st, ops);
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
