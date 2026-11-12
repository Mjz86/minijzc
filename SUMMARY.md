# Checkpoint 23 — the code-quality continuation (break the large functions, break the big files)

branchA @ (one commit on top of checkpoint 22's `b17dbbb` merge). The
CONTINUE backlog's item 4 named the two next-biggest files —
`apps/debugger.hpp` (~970) and `opt/interprocedural/cdfold.hpp` (~809)
— and this checkpoint consumed both with the support-layer +
slim-driver treatment: every large function decomposed into small
ones (~10 lines where the logic allows), every big file split into
focused layers. Pure refactor: zero behavior change, verified
byte-identical by the strongest differential available.

## What landed

### 1. cdfold: the 642-line matcher, decomposed (`cdfold.hpp` 809 -> 33 + `cdfold_impl.hpp`)

The single 642-line `opt_cdfold` (one giant lambda per loop header)
became a 33-line driver plus a support layer of ~35 small constexpr
functions:

- **the algebra** (CdAff, cd_aff_eq, cd_term) unchanged; the bounded
  resolver decomposed (cd_resolve dispatches to cd_resolve_bin /
  cd_resolve_phi; the mul/addsub cases are cd_aff_mul / cd_aff_addsub)
  — preserving the original's asymmetries exactly: `const * value` is
  still REJECTED (only value-left or both-const folds), the depth cap
  stays 8, the def-site scan still sees the CURRENT block count.
- **cd_def_site** replaces TEN inline copies of the last-def scan,
  with the scan bound passed EXPLICITLY (nb vs fn.nblocks()) — the
  difference is load-bearing and now documented in one place: the
  resolver sees blocks appended by earlier folds in the same pass
  call, the shape matchers deliberately do not (conservative).
- **the matchers**, one per phase in the old order: cd_membership,
  cd_loop_edges (pre/latch), cd_collect_hphis, cd_classify_latches,
  cd_find_selector, cd_find_countdown (nP/bH), cd_classify_adds
  (cd_add_of), cd_classify_exits (cd_cbr_np_zero / cd_cbr_bh_zero_trap
  over a shared cd_eq_zero_vreg — the `Eq(v, Const 0)` test shape),
  cd_check_xblocks (cd_x_ret / cd_x_br / cd_x_uses_ok), cd_sel_sound
  (entry + toggle proofs), cd_body_pure (cd_instr_pure). All state in
  one CdShape; the match is cd_match (14 lines of orchestration).
- **the emitter**: CdGen (the two instr buffers — guard `g` into the
  preheader, closed form `cf` — with nt()/konst()/bin()/ckonst()/
  cbin()); cd_emit_guard, cd_emit_parity, cd_emit_toggle,
  cd_emit_triangle (the dead `2N` vreg allocation KEPT — vreg
  numbering is part of the byte-identical contract), cd_emit_add_one,
  cd_emit_closed_forms; cd_rewrite_uses (cd_rewrite_op per operand);
  cd_patch_pre, cd_cf_terminator (cd_cf_term_sel for the multi-member
  select), cd_splice_pre (the every-block range shift), cd_append_cf,
  cd_sever_loop; orchestrated by cd_apply in EXACTLY the old order —
  the outside-use rewrite still precedes the two post-rewrite
  validations, and os.cdfolds++ still fires only on full success.
- The dead state the original accumulated is gone: kval (write-only),
  Xt/Xf (never read), the `(void)db`, the const*const re-resolve, the
  `g = 0` dead init. The landmine comments (the copies-must-rewrite-
  dst class lives in rotate's list; the appended-block range shift,
  the CFb-current-count collision, the sever-don't-delete rationale)
  all carried over to the helpers they describe.

### 2. debugger: the 970-line session, decomposed (970 -> 171 + four focused files)

- **`dbg_sess.hpp`** — DbgSess (the one-session mutable state) with
  the two engines that were 60+ line methods:
  watch_step (68) -> watch_slot_of / watch_last / watch_rebaseline /
  watch_report / watch_refresh + a 15-line driver; run_bounded (61)
  -> stop_at (the old lambda, now a method) + stop_report (the stop
  report shared by the two manual-bp sites — the pre-loop message
  has no ops count, the in-loop one does; the ops argument carries
  the distinction) + a slim loop. report_watches -> watch_row.
- **`dbg_cmds.hpp`** — the eleven command families; the ir dump
  lambda, the vars slot-enrichment, the bkpt observation-string
  builder, the watch add/del paths are now named functions
  (dbg_ir_dump, dbg_vars_enrich, dbg_bkpt_obs, dbg_watch_add/del,
  dbg_src_row, dbg_cfg_block, dbg_plan_instr, dbg_bc_fn_range,
  dbg_check_row).
- **`dbg_opt.hpp`** — the pass explorer: dbg_snap_idx (the old
  closure, hoisted), dbg_opt_show / diff / break (with
  dbg_opt_break_hit) / continue / list / collect (with
  dbg_report_pause); dbg_cmd_opt is a 17-line dispatcher.
- **`dbg_vm.hpp`** — the machine half: per-subcommand functions; the
  traced single step is dbg_vm_step_one + dbg_vm_show_bkpt (the
  breakpoint observation), the stop messages dbg_vm_stop_msgs, the
  line-breakpoint walk dbg_line_bps; dbg_cmd_vm is a 19-line
  dispatcher.
- **`debugger.hpp`** — the slim session: help text, banner
  (dbg_banner), tokenizer (dbg_tokenize), dispatch (dbg_dispatch),
  and run_debugger's 25-line loop (was 89).

### 3. Verification (the no-behavior-change proof)

- A scripted `--debugger` session exercising EVERY command path
  (src/fns/ir/cfg/stats/plan/bc/bkpts/vars/watch/unwatch, opt
  break/continue/show/diff/list/collect, vm
  reset/step/run/continue/break/line/unbreak/breaks/until/dis/state,
  check, help) diffs BYTE-IDENTICAL against the pre-refactor binary;
  a second session over the rotate tour demo likewise.
- `--check-all` runtime parity identical; the register-audit table
  regenerates byte-identically (RESULTS.md untouched by
  scripts/regen_audit_results.py: 710/573/137/424 preserved).
- selftest 207/207 | ctest 44/44 | units 6328/6328 |
  check-format clean | header_order 97/97 + umbrella (92 before: the
  five new headers are each self-contained) | include portability
  410/410 | fuzz spot checks on the 15531 / 14231 / 16131 windows
  clean.

### 4. Measured shape

457 -> 527 functions (+70 small ones); <= 10-line functions 301 ->
332; the 71+ bucket 62 -> 58: the 642 (opt_cdfold), 158 (dbg_cmd_opt),
143 (dbg_cmd_vm), 89 (run_debugger) and 68 (watch_step) line
functions are gone. Largest function in the touched files: 80
(dbg_vm_state, pre-existing in dbg_support.hpp). File layout after
the splits: cdfold.hpp 33 / cdfold_impl.hpp 930 (mostly the carried
landmine documentation); debugger.hpp 171 / dbg_sess.hpp 271 /
dbg_cmds.hpp 301 / dbg_opt.hpp 198 / dbg_vm.hpp 197 / dbg_support.hpp
510 (unchanged).

## Known limits

- The next-biggest functions are untouched by design (one
  checkpoint's scope): opt_lcf (~640), opt_unroll (~470),
  opt_constprop (~450), opt_taildup (~419), gen_stmt (~408),
  opt_rotate (~405), opt_unswitch (~389). The same treatment
  applies; the backlog names them in order.
- cdfold_impl.hpp is 930 lines with small functions — the size is
  the carried-over phase documentation, not function bulk. A further
  algebra/matcher/emitter three-way split is possible if the file
  bothers anyone.
- A real Windows/MSVC run remains the one unverified step (no MSVC
  in the sandbox; native parity ran on g++).
