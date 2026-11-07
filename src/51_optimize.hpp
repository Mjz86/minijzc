#pragma once
struct Optimized {
  Fn fn{};         // SSA after optimization
  OptStats os{};
  An an{};         // final analysis (post-opt)
};
constexpr bool optimize(Fn &fn, OptStats &os, An &final_an, PassCfg cfg) {
  if (!cfg.do_opt || cfg.level == 0) { // -O0: untouched IR, fresh analysis
    final_an = analyze(fn);
    return false;
  }
  bool any = false;
  const uint32_t cap = cfg.fp_limit ? cfg.fp_limit : 24;
  if (cfg.debug >= 1) {
    char nb[64] = "<fn>";
    if (!cfg.dbg_name.empty())
      std::snprintf(nb, sizeof nb, "%.*s", (int)cfg.dbg_name.size(),
                    cfg.dbg_name.data());
    std::printf("==== optimize %s (level %u, fp-limit %u) ====\n", nb,
                (unsigned)cfg.level, (unsigned)cap);
  }
  // ---- tier 0: cheap simplification to its fixpoint -----------------------
  // (each round must make progress; dominance passes see a fresh An)
  auto tier0 = [&]() -> bool {
    bool ch_any = false;
    for (uint32_t it = 0; it < cap; ++it) {
      bool ch = false, c = false;
      c = opt_constprop(fn, os); // fold/identities/branch folds
      dbg_pass(cfg.debug, fn, "fold", c);
      ch |= c;
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_COPYPROP
      if (cfg.p_copyprop) {
        c = opt_copyprop(fn, os); // copies -> uses of the source
        dbg_pass(cfg.debug, fn, "copyprop", c);
        ch |= c;
      }
#endif
      normalize_phis(fn);
      if (cfg.p_canon) {
        c = opt_canon(fn, os); // canonical forms + NOT-of-compare
        dbg_pass(cfg.debug, fn, "canon", c);
        ch |= c;
      }
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_CSE
      if (cfg.p_cse) {
        An A = analyze(fn);
        c = opt_cse(fn, A, os); // dominating recomputation -> reuse
        dbg_pass(cfg.debug, fn, "cse", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (cfg.p_unreach) {
        c = opt_remove_unreachable(fn, os); // dead blocks + phi degeneration
        dbg_pass(cfg.debug, fn, "unreach", c);
        ch |= c;
      }
      normalize_phis(fn);
#ifndef MJZ_DBG_NO_SPLICE
      if (cfg.p_splice) {
        c = opt_splice(fn, os); // collapse single-pred/succ jump chains
        dbg_pass(cfg.debug, fn, "splice", c);
        ch |= c;
      }
#endif
      normalize_phis(fn);
      if (cfg.p_dce) {
        c = opt_dce(fn, os);
        dbg_pass(cfg.debug, fn, "dce", c);
        ch |= c;
      }
      normalize_phis(fn);
      os.iters = it + 1;
      if (!ch) break;
      ch_any = true;
    }
    return ch_any;
  };
  any |= tier0();
  // ---- tier 1: middle passes; cheap fixpoint after each round -------------
  if (cfg.level >= 2) {
    for (uint32_t outer = 0; outer < cap; ++outer) {
      bool ch = false, c = false;
      if (cfg.p_mem2reg) {
        An A = analyze(fn);
        c = opt_mem2reg(fn, A, os, cfg.lift_ok, cfg.stkarr_ok);
        dbg_pass(cfg.debug, fn, "mem2reg", c);
        ch |= c;
        normalize_phis(fn);
      }
      if (cfg.p_trapelim) {
        An A = analyze(fn);
        c = opt_trapelim(fn, A, os); // ranges + aliasing -> nt flags
        dbg_pass(cfg.debug, fn, "trapelim", c);
        ch |= c;
        normalize_phis(fn);
      }
#ifndef MJZ_DBG_NO_LICM
      if (cfg.p_licm) {
        An A = analyze(fn);
        c = opt_licm(fn, A, os); // hoist invariants to the preheader
        dbg_pass(cfg.debug, fn, "licm", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_DIVMAGIC
      if (cfg.p_divmagic) {
        c = opt_divmagic(fn, os); // /constant -> mulhi+shift
        dbg_pass(cfg.debug, fn, "divmagic", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_REASSOC
      if (cfg.p_reassoc) {
        c = opt_reassoc(fn, os); // canonicalize +/-/*c chains (feeds LCF)
        dbg_pass(cfg.debug, fn, "reassoc", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (!ch) break;
      any = true;
      tier0();
    }
  }
  // ---- tier 2: expensive CFG restructuring, only after 0/1 fixpoint -------
  if (cfg.level >= 3) {
    for (uint32_t outer = 0; outer < cap; ++outer) {
      bool ch = false;
#ifndef MJZ_DBG_NO_UNROLL
      if (cfg.p_unroll && cfg.unroll) {
        An A = analyze(fn);
        bool c = opt_unroll(fn, A, os, cfg.unroll);
        dbg_pass(cfg.debug, fn, "unroll", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
#ifndef MJZ_DBG_NO_LCF
      if (cfg.p_lcf) {
        An A2 = analyze(fn); // unrolling may have reshaped the CFG
        bool c = opt_lcf(fn, A2, os); // closed forms / simulators
        dbg_pass(cfg.debug, fn, "lcf", c);
        ch |= c;
        normalize_phis(fn);
      }
#endif
      if (!ch) break;
      any = true;
      tier0();
      // one tier-1 round: restructuring exposes new opportunities
      if (cfg.level >= 2) {
        bool c2 = false;
        if (cfg.p_mem2reg) {
          An A = analyze(fn);
          c2 |= opt_mem2reg(fn, A, os, cfg.lift_ok, cfg.stkarr_ok);
          normalize_phis(fn);
        }
        if (cfg.p_trapelim) {
          An A = analyze(fn);
          c2 |= opt_trapelim(fn, A, os);
          normalize_phis(fn);
        }
        if (cfg.p_licm) {
          An A = analyze(fn);
          c2 |= opt_licm(fn, A, os);
          normalize_phis(fn);
        }
        if (cfg.p_divmagic) {
          c2 |= opt_divmagic(fn, os);
          normalize_phis(fn);
        }
        if (cfg.p_reassoc) {
          c2 |= opt_reassoc(fn, os);
          normalize_phis(fn);
        }
        (void)c2;
        tier0();
      }
    }
  }
  normalize_phis(fn);
  final_an = analyze(fn);
  return any;
}

// --- 9f2. stack-allocation analysis (for the C backend) ---------------------
// An `array(n)` with constant n whose handle only ever flows (through Copies)
// to Load/Store/Len as the HANDLE operand — never into a phi (an older
// iteration's array could otherwise be observed after the same instruction
// re-allocated its slot), never into arithmetic/comparisons/print/return/call
// arguments, never stored as a value (array-of-arrays) — is provably local:
// its handle value is unobservable, so the C backend can keep it in a stack
// array (zero-filled at the allocation point, exactly like the arena).
// Handles ARE ordinary values: any other use makes the numeric value
// observable and pins the array to the arena (indices must match every
// other runner). Budget: per array <= 16384 elements and <= 1 MiB total,
// to keep native frames far below the default 8 MiB stack.
inline constexpr val_t MJZ_STK_ARR_MAX_ELEMS = 16384;
inline constexpr val_t MJZ_STK_ARR_TOTAL_ELEMS = 131072;
constexpr void analyze_stack_allocs(Fn &fn) {
  fn.stk_arrays.clear();
  const uint32_t nb = fn.nblocks();
  if (!nb) return;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nv) return;
  std::vector<uint32_t> ndefs(nv, 0);
  for (const Instr &I : fn.code)
    if (I.dst != NONE && I.dst < nv) ++ndefs[I.dst];
  std::vector<char> kc(nv, 0);
  std::vector<val_t> kv(nv, 0);
  for (const Instr &I : fn.code)
    if (I.k == K::Const && I.dst != NONE && I.dst < nv && ndefs[I.dst] == 1) {
      kc[I.dst] = 1;
      kv[I.dst] = I.c;
    }
  val_t total = 0;
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      const Instr &I = fn.code[i];
      if (I.k != K::Alloc) continue;
      if (I.a == NONE || I.a >= nv || !kc[I.a]) continue; // runtime length
      val_t len = kv[I.a];
      if (len < 0 || len == 0 || len > MJZ_STK_ARR_MAX_ELEMS) continue;
      if (total + len > MJZ_STK_ARR_TOTAL_ELEMS) continue;
      // alias closure: the alloc dst + transitive Copy destinations
      std::vector<char> ali(nv, 0);
      ali[I.dst] = 1;
      for (bool grew = true; grew;) {
        grew = false;
        for (const Instr &C : fn.code)
          if (C.k == K::Copy && C.a != NONE && C.a < nv && C.dst < nv &&
              ali[C.a] && !ali[C.dst]) {
            ali[C.dst] = 1;
            grew = true;
          }
      }
      // escape scan over the FINAL alias set
      bool esc = false;
      for (uint32_t b2 = 0; b2 < nb && !esc; ++b2)
        for (uint32_t j = fn.blocks[b2].begin; j < fn.blocks[b2].end && !esc;
             ++j) {
          const Instr &U = fn.code[j];
          auto bad = [&](uint32_t v) { return v != NONE && v < nv && ali[v]; };
          switch (U.k) {
          case K::Copy: // source: alias extension (ok); dst cannot re-define
            break;
          case K::Load: // handle position ok; the INDEX is a value use
            if (bad(U.b2)) esc = true;
            break;
          case K::Store: // handle ok; index and stored value are value uses
            if (bad(U.b2) || bad(U.c2)) esc = true;
            break;
          case K::Len: // handle position ok
            break;
          default: // Bin/Cbr/Print/Call/Ret/Phi/Exit/Input: any alias use
            if (bad(U.a) || bad(U.b2) || bad(U.c2)) esc = true;
            for (uint32_t v : U.args)
              if (bad(v)) esc = true;
            break;
          }
        }
      if (esc) continue;
      total += len;
      Fn::StkArr sa{};
      sa.root = I.dst;
      sa.len = len;
      for (uint32_t v = 0; v < nv; ++v)
        if (ali[v]) sa.vregs.push_back(v);
      fn.stk_arrays.push_back(std::move(sa));
    }
}
