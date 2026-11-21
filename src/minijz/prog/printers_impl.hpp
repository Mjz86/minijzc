#pragma once
// === printers_impl.hpp ========================================
// the printer bodies: fn_ir_str, print_fn, unified_diff, print_run,
// text_of and sum_ostats (see printers.hpp / program.hpp). Included by
// printers.hpp in the header-only build (inline, via MJZ_PRINTER_FN) and
// by tu/runners.cpp in the multi-TU build (plain external linkage) --
// include it nowhere else.

#include "printers.hpp"
#include "program.hpp" // sum_ostats (Program / FnUnit / OptStats)

MJZ_PRINTER_FN std::string fn_ir_str(const Fn &fn, const char *title,
                                     std::string_view src) {
  std::string s{};
  char buf[192];
  SrcMap SM{src}; // when given, every instr line gains `; @line:col`
  s += "  ---- ";
  s += title;
  s += " ----\n";
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    if (fn.blocks[b].begin == fn.blocks[b].end && fn.succ0(b) == NONE &&
        fn.succ1(b) == NONE && b != 0)
      continue; // isolated dead block
    std::snprintf(buf, sizeof buf, "  b%u:", (unsigned)b);
    s += buf;
    if (!fn.preds.empty() && !fn.preds[b].empty()) {
      s += "              ; preds:";
      for (uint32_t p : fn.preds[b]) {
        std::snprintf(buf, sizeof buf, " b%u", (unsigned)p);
        s += buf;
      }
    }
    s += "\n";
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      s += "    ";
      switch (I.k) {
      case K::Const:
        std::snprintf(buf, sizeof buf, "%s = %lld", vname(fn, I.dst).c_str(),
                      (long long)I.c);
        s += buf;
        break;
      case K::Copy:
        std::snprintf(buf, sizeof buf, "%s = %s", vname(fn, I.dst).c_str(),
                      vname(fn, I.a).c_str());
        s += buf;
        break;
      case K::Bin:
        std::snprintf(buf, sizeof buf, "%s = %s %s %s",
                      vname(fn, I.dst).c_str(), vname(fn, I.a).c_str(),
                      bname(I.b), vname(fn, I.b2).c_str());
        s += buf;
        break;
      case K::Br:
        std::snprintf(buf, sizeof buf, "br b%u", (unsigned)I.t);
        s += buf;
        break;
      case K::Cbr:
        std::snprintf(buf, sizeof buf, "cbr %s ? b%u : b%u",
                      vname(fn, I.a).c_str(), (unsigned)I.t, (unsigned)I.f);
        s += buf;
        break;
      case K::Print:
        std::snprintf(buf, sizeof buf, "print %s", vname(fn, I.a).c_str());
        s += buf;
        break;
      case K::Phi: {
        std::snprintf(buf, sizeof buf, "%s = phi ", vname(fn, I.dst).c_str());
        s += buf;
        if (I.a != NONE) {
          std::snprintf(buf, sizeof buf, "[%s from b%u]",
                        vname(fn, I.a).c_str(), (unsigned)I.t);
          s += buf;
        }
        if (I.b2 != NONE) {
          std::snprintf(buf, sizeof buf, "%s[%s from b%u]",
                        I.a != NONE ? " " : "", vname(fn, I.b2).c_str(),
                        (unsigned)I.f);
          s += buf;
        }
        break;
      }
      case K::Exit:
        s += "exit";
        break;
      case K::Alloc:
        std::snprintf(buf, sizeof buf, "%s = alloc %s",
                      vname(fn, I.dst).c_str(), vname(fn, I.a).c_str());
        s += buf;
        break;
      case K::Load:
        std::snprintf(buf, sizeof buf, "%s = %s[%s]", vname(fn, I.dst).c_str(),
                      vname(fn, I.a).c_str(), vname(fn, I.b2).c_str());
        s += buf;
        break;
      case K::Store:
        std::snprintf(buf, sizeof buf, "%s[%s] = %s", vname(fn, I.a).c_str(),
                      vname(fn, I.b2).c_str(), vname(fn, I.c2).c_str());
        s += buf;
        break;
      case K::Len:
        std::snprintf(buf, sizeof buf, "%s = len %s", vname(fn, I.dst).c_str(),
                      vname(fn, I.a).c_str());
        s += buf;
        break;
      case K::Input:
        std::snprintf(buf, sizeof buf, "%s = input()",
                      vname(fn, I.dst).c_str());
        s += buf;
        break;
      case K::Depth:
        std::snprintf(buf, sizeof buf, "%s = depth()",
                      vname(fn, I.dst).c_str());
        s += buf;
        break;
      case K::Call: {
        std::snprintf(buf, sizeof buf, "%s = f%u(", vname(fn, I.dst).c_str(),
                      (unsigned)I.c);
        s += buf;
        for (size_t k = 0; k < I.args.size(); ++k) {
          s += k ? ", " : "";
          s += vname(fn, I.args[k]);
        }
        s += ")";
        break;
      }
      case K::Ret:
        if (I.a == NONE)
          s += "ret 0";
        else {
          std::snprintf(buf, sizeof buf, "ret %s", vname(fn, I.a).c_str());
          s += buf;
        }
        break;
      case K::Bkpt:
        s += "bkpt";
        for (size_t k = 0; k < I.args.size(); ++k) {
          s += k ? ", " : " ";
          s += vname(fn, I.args[k]);
        }
        break;
      }
      if (SM.ok() && I.src != NONE) {
        char lb[48];
        if (SM.at(I.src, lb, sizeof lb)) {
          s += "  ; @";
          s += lb;
        }
      }
      s += "\n";
    }
  }
  return s;
}

MJZ_PRINTER_FN void print_fn(const Fn &fn, const char *title,
                             std::string_view src) {
  std::printf("%s", fn_ir_str(fn, title, src).c_str());
}

MJZ_PRINTER_FN std::string unified_diff(const std::string &a,
                                        const std::string &b, int ctx) {
  auto split = [](const std::string &s) {
    std::vector<std::string> ls{};
    size_t p = 0;
    for (;;) {
      size_t n = s.find('\n', p);
      if (n == std::string::npos) {
        if (p < s.size())
          ls.push_back(s.substr(p));
        break;
      }
      ls.push_back(s.substr(p, n - p));
      p = n + 1;
    }
    return ls;
  };
  std::vector<std::string> A = split(a), B = split(b);
  const size_t n = A.size(), m = B.size();
  if (n + m == 0)
    return {};
  // LCS direction table (rolled rows for lengths; directions kept whole)
  std::vector<uint32_t> prev(m + 1, 0), cur(m + 1, 0);
  std::vector<std::vector<uint8_t>> dir(n ? n : 1,
                                        std::vector<uint8_t>(m ? m : 1, 0));
  for (size_t i = n; i-- > 0;) {
    for (size_t j = m; j-- > 0;) {
      if (A[i] == B[j]) {
        cur[j] = 1 + prev[j + 1];
        dir[i][j] = 1; // keep: common line
      } else if (prev[j] >= cur[j + 1]) {
        cur[j] = prev[j];
        dir[i][j] = 2; // drop A[i]
      } else {
        cur[j] = cur[j + 1];
        dir[i][j] = 3; // insert B[j]
      }
    }
    std::swap(prev, cur);
    std::fill(cur.begin(), cur.end(), 0u);
  }
  // walk: one body row per emitted line (' ' kept, '-' removed, '+' added)
  std::vector<std::pair<char, std::string>> body{};
  {
    size_t i = 0, j = 0;
    while (i < n && j < m) {
      if (dir[i][j] == 1) {
        body.push_back({' ', A[i]});
        ++i;
        ++j;
      } else if (dir[i][j] == 2) {
        body.push_back({'-', A[i]});
        ++i;
      } else {
        body.push_back({'+', B[j]});
        ++j;
      }
    }
    while (i < n)
      body.push_back({'-', A[i++]});
    while (j < m)
      body.push_back({'+', B[j++]});
  }
  // rows to show: every change +/- `ctx` context rows around it
  std::vector<char> show(body.size(), 0);
  for (size_t k = 0; k < body.size(); ++k)
    if (body[k].first != ' ')
      for (size_t d = (size_t)ctx + 1; d-- > 0;) {
        if (k >= d)
          show[k - d] = 1;
        if (k + d < body.size())
          show[k + d] = 1;
      }
  // hunks; ai/bi track the 1-based line numbers in A/B as we walk the body
  std::string s{};
  size_t ai = 0, bi = 0;
  bool in_hunk = false;
  char ln[512];
  for (size_t k = 0; k < body.size(); ++k) {
    const char op = body[k].first;
    if (!show[k]) {
      if (op == ' ') {
        ++ai;
        ++bi;
      }
      continue;
    }
    if (!in_hunk) {
      std::snprintf(ln, sizeof ln, "@@ -%zu +%zu @@\n", ai + 1, bi + 1);
      s += ln;
      in_hunk = true;
    }
    std::snprintf(ln, sizeof ln, "%c%s\n", op, body[k].second.c_str());
    s += ln;
    if (op != '+')
      ++ai;
    if (op != '-')
      ++bi;
    if (k + 1 < body.size() && !show[k + 1]) {
      in_hunk = false;
      s += "\n";
    }
  }
  return s;
}

MJZ_PRINTER_FN void print_run(const RunOut &R, std::string_view src) {
  if (R.trapped) {
    // located trap when the runner recorded where it died (the SSA interp
    // + the oracle do; VM/native keep the plain contract)
    if (!src.empty() && R.trap_src != NONE && R.trap_src < src.size()) {
      SrcMap SM{src};
      char lb[48];
      if (SM.at(R.trap_src, lb, sizeof lb)) {
        std::printf("TRAP (%s at %s)\n",
                    R.trap_kind ? R.trap_kind : "runtime error", lb);
        return;
      }
    }
    std::printf("TRAP\n");
    return;
  }
  if (R.timeout) {
    std::printf("TIMEOUT\n");
    return;
  }
  for (size_t i = 0; i < R.out.size(); ++i)
    std::printf("%s%lld", i ? " " : "", (long long)R.out[i]);
  if (R.out.empty())
    std::printf("(no output)");
  std::printf("\n");
}

MJZ_PRINTER_FN std::string text_of(const RunOut &R) {
  std::string s{};
  char buf[32];
  for (val_t v : R.out) {
    std::snprintf(buf, sizeof buf, "%lld\n", (long long)v);
    s += buf;
  }
  if (R.trapped)
    s += "TRAP\n";
  else if (R.timeout)
    s += "TIMEOUT\n";
  return s;
}

// ---- summed optimization statistics (moved from program.hpp)
MJZ_RUNNER_FN OptStats sum_ostats(const Program &P) {
  OptStats t{};
  for (const FnUnit &U : P.fns) {
    t.folds += U.ostats.folds;
    t.copies += U.ostats.copies;
    t.brs_folded += U.ostats.brs_folded;
    t.blocks_gone += U.ostats.blocks_gone;
    t.dce += U.ostats.dce;
    t.hoisted += U.ostats.hoisted;
    t.copyprops += U.ostats.copyprops;
    t.cses += U.ostats.cses;
    t.splices += U.ostats.splices;
    t.cbr_same += U.ostats.cbr_same;
    t.cbr_reconv += U.ostats.cbr_reconv;
    t.phi_degens += U.ostats.phi_degens;
    t.canon_swaps += U.ostats.canon_swaps;
    t.canon_cmps += U.ostats.canon_cmps;
    t.inlines += U.ostats.inlines;
    t.unrolls += U.ostats.unrolls;
    t.reassocs += U.ostats.reassocs;
    t.reassoc_gone += U.ostats.reassoc_gone;
    t.lcf_loops += U.ostats.lcf_loops;
    t.lcf_sims += U.ostats.lcf_sims;
    t.lcf_gensims += U.ostats.lcf_gensims;
    t.constcalls += U.ostats.constcalls;
    t.deadallocs += U.ostats.deadallocs;
    t.tces += U.ostats.tces;
    t.tce_merges += U.ostats.tce_merges;
    t.cdfolds += U.ostats.cdfolds;
    t.ifconvs += U.ostats.ifconvs;
    t.lcf_rt += U.ostats.lcf_rt;
    t.divmagics += U.ostats.divmagics;
    t.notcombs += U.ostats.notcombs;
    t.traps_killed += U.ostats.traps_killed;
    t.mem2reg_lifts += U.ostats.mem2reg_lifts;
    t.mem2reg_dses += U.ostats.mem2reg_dses;
    t.scctco += U.ostats.scctco;
    t.scctco_folds += U.ostats.scctco_folds;
    t.sccp_consts += U.ostats.sccp_consts;
    t.sccp_brs += U.ostats.sccp_brs;
    t.ld_fwds += U.ostats.ld_fwds;
    t.ld_cses += U.ostats.ld_cses;
    t.rangecmps += U.ostats.rangecmps;
    t.cmpfacts += U.ostats.cmpfacts;
    t.callcses += U.ostats.callcses;
    t.deadargs += U.ostats.deadargs;
    t.fnspecs += U.ostats.fnspecs;
    t.icf += U.ostats.icf;
    t.jthreads += U.ostats.jthreads;
    t.argpromos += U.ostats.argpromos;
    t.gvns += U.ostats.gvns;
    t.unswitches += U.ostats.unswitches;
    t.body_unswitches += U.ostats.body_unswitches;
    t.lsimps += U.ostats.lsimps;
    t.taildups += U.ostats.taildups;
    t.rotations += U.ostats.rotations;
    t.dse_overwrites += U.ostats.dse_overwrites;
    t.dse_sames += U.ostats.dse_sames;
    if (U.ostats.iters > t.iters)
      t.iters = U.ostats.iters;
  }
  return t;
}
