#pragma once
// === dbg_opt.hpp ========================================
// the --debugger PASS EXPLORER (`opt` and its subcommands), split out
// of debugger.hpp: snapshot collection (`opt [FN]`), show / diff /
// break / continue / list, each a small function over the session.

#include "dbg_sess.hpp"

// snapshot reference: a lowercase NAME (its timeline index) or an index
static long dbg_snap_idx(const DbgSess &S, const std::string &s, long dflt) {
  if (!s.empty() && (s[0] >= 'a' && s[0] <= 'z')) {
    for (size_t k = 0; k < S.snaps.size(); ++k)
      if (S.snaps[k].name == s)
        return (long)k;
    return -1;
  }
  return s.empty() ? dflt : atol(s.c_str());
}

// ---- opt show [N]: full IR of snapshot N (default: last) -------------------
static void dbg_opt_show(DbgSess &S, const std::vector<std::string> &tok) {
  if (S.snaps.empty()) {
    std::printf("(no snapshots yet — run `opt [FN]` first)\n");
    return;
  }
  long n = tok.size() > 2 ? dbg_snap_idx(S, tok[2], (long)S.snaps.size() - 1)
                          : (long)S.snaps.size() - 1;
  if (n < 0 || n >= (long)S.snaps.size())
    std::printf("no snapshot #%ld (0..%zu)\n", n, S.snaps.size() - 1);
  else
    std::printf("%s", S.snaps[(size_t)n].ir.c_str());
}

// ---- opt diff [A [B]]: unified IR diff between snapshots -------------------
static void dbg_opt_diff(DbgSess &S, const std::vector<std::string> &tok) {
  if (S.snaps.size() < 2) {
    std::printf("(need at least 2 snapshots — run `opt [FN]` first)\n");
    return;
  }
  long a = tok.size() > 2 ? dbg_snap_idx(S, tok[2], 0)
                          : 0; // default: overall (first vs last)
  long b = tok.size() > 3 ? dbg_snap_idx(S, tok[3], (long)S.snaps.size() - 1)
                          : (tok.size() > 2 ? a + 1 : (long)S.snaps.size() - 1);
  if (a < 0 || b < 0 || a >= (long)S.snaps.size() ||
      b >= (long)S.snaps.size() || a == b) {
    std::printf("usage: opt diff [A [B]] (snapshots 0..%zu)\n",
                S.snaps.size() - 1);
    return;
  }
  std::printf("== diff: #%ld (%s) -> #%ld (%s) ==\n", a,
              S.snaps[(size_t)a].name.c_str(), b,
              S.snaps[(size_t)b].name.c_str());
  std::string d = unified_diff(S.snaps[(size_t)a].ir, S.snaps[(size_t)b].ir);
  std::printf("%s", d.empty() ? "  (identical)\n" : d.c_str());
}

// ---- opt break PASS: a breakpoint BETWEEN passes ---------------------------
// locate PASS, show where the pipeline stands and what that pass itself
// changed — and ARM it, so the next `opt [FN]` collection auto-pauses
// there (checkpoint 18: the pause actually stops the timeline, the way a
// vm breakpoint stops execution)
static void dbg_opt_break_hit(DbgSess &S, long k) {
  std::printf("* stopped after pass #%ld [%s] (fn %zu [%s])\n", k,
              S.snaps[(size_t)k].name.c_str(), S.snap_fn,
              std::string(S.P.fn_names[S.snap_fn]).c_str());
  if (k > 0) {
    std::string d =
        unified_diff(S.snaps[(size_t)k - 1].ir, S.snaps[(size_t)k].ir);
    std::printf("%s",
                d.empty() ? "  (this pass changed nothing)\n" : d.c_str());
  } else {
    std::printf("  (#0 is the baseline — nothing precedes it)\n");
  }
  std::printf("(opt show %ld dumps the full IR at this point; "
              "opt diff for other pairs)\n",
              k);
}
static void dbg_opt_break(DbgSess &S, const std::vector<std::string> &tok) {
  if (tok.size() < 3) {
    std::printf("usage: opt break PASS (e.g. opt break sccp)\n");
    return;
  }
  S.pass_break = tok[2]; // armed (re-arming another pass just swaps)
  std::printf("* pass breakpoint armed: %s\n"
              "  (the next `opt [FN]` pauses after its first snapshot; "
              "`opt continue` disarms and collects in full)\n",
              tok[2].c_str());
  long k = dbg_snap_idx(S, tok[2], -1);
  if (k < 0) {
    if (S.snaps.empty())
      std::printf("(not collected yet — run `opt [FN]` to pause there)\n");
    else
      std::printf("(no snapshot named '%s' in the current timeline; "
                  "it will arm for the next collection only if that "
                  "pass runs)\n",
                  tok[2].c_str());
  } else {
    dbg_opt_break_hit(S, k);
  }
}

// ---- opt continue: resume a paused collection ------------------------------
// disarm + re-collect the full timeline
static void dbg_opt_continue(DbgSess &S) {
  if (S.pass_break.empty()) {
    std::printf("(no pass breakpoint armed — `opt break PASS` first)\n");
    return;
  }
  std::printf("* pass breakpoint disarmed (was: %s) — collecting in "
              "full\n",
              S.pass_break.c_str());
  S.pass_break.clear();
  S.snaps.clear();
  long stop_idx = -1;
  if (!dbg_collect_snaps(S.P, S.snap_fn, S.C, S.snaps, "", stop_idx))
    std::printf("fn %zu: no snapshots (-O0 leaves the IR untouched)\n",
                S.snap_fn);
  else
    dbg_print_timeline(S.snaps, S.snap_fn, S.P);
}

// ---- opt list: the pass toggle table ----------------------------------------
static void dbg_opt_list(DbgSess &S) {
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
    std::printf("  %-10s %s%s", PASS_TOGGLE_NAMES[i], states[i] ? "on " : "OFF",
                (i % 4) == 3 ? "\n" : "   ");
  if (PASS_TOGGLE_COUNT % 4)
    std::printf("\n");
}

// ---- opt [FN]: (re-)collect the timeline ------------------------------------
// the auto-pause: the timeline stops after the armed pass, its diff is
// shown, and `opt continue` collects the rest
static void dbg_report_pause(DbgSess &S, long stop_idx) {
  std::printf("* paused after pass #%ld [%s] — pass breakpoint %s\n", stop_idx,
              S.snaps[(size_t)stop_idx].name.c_str(), S.pass_break.c_str());
  if (stop_idx > 0) {
    std::string d = unified_diff(S.snaps[(size_t)stop_idx - 1].ir,
                                 S.snaps[(size_t)stop_idx].ir);
    std::printf("%s",
                d.empty() ? "  (this pass changed nothing)\n" : d.c_str());
  }
  std::printf("(opt show %ld dumps the full IR here; "
              "opt continue collects the rest of the timeline)\n",
              stop_idx);
}
static void dbg_opt_collect(DbgSess &S, const std::string &sub,
                            const std::vector<std::string> &tok) {
  long f = sub.empty() ? dbg_num(tok, 1, 0) : atol(sub.c_str());
  if (f < 0 || f >= (long)S.P.fns.size())
    f = 0;
  S.snaps.clear();
  S.snap_fn = (size_t)f;
  long stop_idx = -1;
  if (!dbg_collect_snaps(S.P, (size_t)f, S.C, S.snaps, S.pass_break,
                         stop_idx)) {
    std::printf("fn %ld: no snapshots (-O0 leaves the IR untouched)\n", f);
    return;
  }
  dbg_print_timeline(S.snaps, (size_t)f, S.P);
  if (stop_idx >= 0)
    dbg_report_pause(S, stop_idx);
  else
    std::printf("(opt show N | opt diff A B | opt break PASS to "
                "inspect)\n");
}

// opt [FN] | show [N] | diff [A [B]] | break PASS | continue | list
static void dbg_cmd_opt(DbgSess &S, const std::vector<std::string> &tok) {
  const std::string &sub = tok.size() > 1 ? tok[1] : "";
  if (sub == "show")
    dbg_opt_show(S, tok);
  else if (sub == "diff")
    dbg_opt_diff(S, tok);
  else if (sub == "break")
    dbg_opt_break(S, tok);
  else if (sub == "continue")
    dbg_opt_continue(S);
  else if (sub == "list")
    dbg_opt_list(S);
  else if (!sub.empty() && sub != "run")
    std::printf("opt subcommand? try: opt [FN] | show | diff | break | "
                "continue | list\n");
  else
    dbg_opt_collect(S, sub, tok);
}
