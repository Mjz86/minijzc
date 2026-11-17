# SUMMARY — checkpoints 33 + 34: dedup round two + unswitch decomposed

Two commits, one session, both under the user's standing directives:
keep deduplicating every round (recorded in CONTINUE.md as the
per-round protocol), and keep working the OPT_CHECKLIST queue.

## 1. The dedup campaign, round two (commit 1 — "dedup:")

The window-8/10/12 twin scan re-run after ckpt 31/32; ten waves, all
proved identical before sharing, byte-identical parity at every step:

* **`for_each_operand` + `rewrite_operands`** (ir/instr.hpp): the
  fixed a/b2/c2/args operand traversal, defined once. Eleven
  hand-rolled mark/chk/one/rw enumerations collapsed across
  rotate_impl, unswitch (x2), lcf_impl, lcf_rt, lcf_sim, regalloc,
  astrun, vn_sites' completed-map rewrite, and bump_uses_of's own
  body. reassoc's bump copy (a ckpt-31 miss) and jthread's use census
  became plain `bump_uses_of` calls.
* **`vn_run`** (opt/scalar/vn_sites.hpp): the cse/gvn driver — both
  passes were verbatim twins outside their site-fill policy and stats
  counter. cse is now a one-line policy; gvn states its three entry
  kinds (Bins, non-param consts, complete phis).
* **`remap_vreg_fields` adoption**: callgraph's and tailrec's inline
  five-field permutation copies joined the shared primitive (their
  mapv maps NONE to NONE, so the shared form's guards are vacuous —
  proved and documented in instr.hpp).
* **`any_fn_in_closure`** (opt/interprocedural/constcall.hpp): the
  call-graph closure worklist; closure_has_depth and
  fn_pure_closure are predicate one-liners over it.
* **The in-file twins**: vm_step's eleven trap-stop copies -> one
  `trap_out` lambda; astrun's And/Or mirror arms -> one arm with the
  polarity flag; emit_c's Load/Store bounds-check emission ->
  `emit_arr_bounds`; diag's line scan -> `line_of`; parser's if/while
  `'( expr )'` sequence -> `paren_expr`.
* **Rejected on record** (CONTINUE.md): intervals' census (deliberately
  excludes Phi edge args), cmp_txt vs bname (emission subset with a
  load-bearing default vs the full diagnostic table), deadargs/icf's
  Call-walk openings, the seed_consts decl prologue.

Net -60 lines; ~150 lines of duplication converted to single
definitions.

## 2. Unswitch decomposed (commit 2 — round ten of the continuation)

`opt_unswitch` 381 lines -> a 35-line driver + `unswitch_impl.hpp`:
`UsCtx` (the per-loop context), `us_shape_ok` (the trivial/compound
pick + the shared gates), `us_exits_ok`, `us_false_region`,
`us_outside_uses_ok` (the outside-use safety scan), and the commit
tail (`us_guard_blocks`, `us_emit_guards`, `us_merge_phis`,
`us_rewire`). The bif path and the shared shape/clone machinery stay
in unswitch_bif*.hpp / unswitch_shape.hpp.

**The round's lesson (the landmine)**: the first draft zero-initialized
UsCtx's guard-block ids; the monolith initialized them to NONE per
header. The `&&` compound shape never allocates t1, so
`if (U.t1 != NONE)` fell through and emitted the `||`-only
`t1: br H'` into BLOCK 0 — seed 20029's `-O3 --disable=mem2reg` build
then looped forever (timeout, not wrong output). The 146-run parity
did NOT catch it (the audit programs never exercise compound-&&); the
committed differential fuzz window 20000-20300 caught it on the first
run. Fix: NONE-default every id field (documented in UsCtx's
contract). Rules recorded in CONTINUE.md: a context struct replacing
function locals must copy the locals' INITIALIZERS exactly, and the
wide fuzz windows are part of every decomposition's gate.

## 3. Verification (both commits)

Dedup commit: parity 146/146 byte-identical, fuzz windows 1-300 +
20000-20300 identical modulo timing, selftest 208/208, units
6482/6482, header order 116/116 + umbrella, includes 529/529, stress
ALL PASS. Unswitch commit: parity 146/146 again, both fuzz windows
again, selftest 208, units 6482, header order 117/117 + umbrella (the
new impl header), includes 535/535, audit table regenerated
identically, kernel fuzz 3000 all components 0 fails, e2e fuzz 600
clean, stress ALL PASS.

The String clone: pristine `main`, `origin/main` at ce6ea95; the local
branch `graph-opt-kernels` (tip 2cb6c7d) unchanged. The queue after
this (OPT_CHECKLIST.md): tailrec's tce_merge (~370), argpromo (~408),
unswitch_shape (433) — the per-round dedup protocol and both standing
requirements in force.
