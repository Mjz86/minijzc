# Checkpoint 27 — unswitch_bif decomposed (round five) + the LLVM-gap backlog

branchA @ (one commit on top of checkpoint 26's fold commit). The
OPT_CHECKLIST queue's next named target, consumed:
`opt/loop/unswitch_bif.hpp`'s unswitch_body_if — at 432 lines the
biggest function left in the opt/ tree — got the cdfold/lcf/unroll/
fold treatment, under the byte-identical discipline. Plus the
standing plan ask: the LLVM optimization-list coverage analysis
(127 items mapped to minijzc passes) is now a ranked backlog inside
OPT_CHECKLIST.md, minus the excluded families (vectorization,
cache, instruction selection).

## What landed

### unswitch_bif: the 432-line monolith, decomposed (`unswitch_bif.hpp` 517 -> 87 + `unswitch_bif_impl.hpp` 767)

The body-if unswitch (checkpoint 19's pass — the versioning of a
loop on an invariant INNER branch, whose delivered walk restores
SSA dominance after the guard splits the loop in two) became a
21-line driver plus a support layer of 57 small constexpr
functions + the BifCtx struct:

- **BifCtx**: fn/S/inL/bs (the rewrite target, the scan tables,
  the loop flags, the matched shape) + h/pre/nb/nv + the scan
  state (exits, loopdef, arm_edge, vm_vals, reads) + the commit
  state (C the CloneMap, edge_vm, vm_phi, dcache) — the locals the
  monolith held, grouped; dcache is shared by the delivered walk
  and the rewire sweep so the sweep reads exactly what the walk
  produced.
- **the gates**: bif_preheader_ok (a dedicated `br h' block — the
  guard replaces it wholesale) and bif_growth_ok (1..120 in-loop
  instrs).
- **the classification**: bif_mark_loopdefs (SSA one-def scan) and
  bif_mark_arms (bif_is_pure_arm: an outside block whose only pred
  is an in-loop exit source — the block whose value provider is the
  one VM below it).
- **the reverse-reachability flood**: bif_rr + bif_rr_step — every
  exit target that reaches b through outside blocks; the worklist
  walks PREDECESSOR edges and an in-loop pred means x is itself an
  exit target (recorded — x, not p: the edge p->x leaves the loop).
- **the outside-use scan**: phi args keyed to an in-loop pred are
  exit edges (the VM entering the block merges the arg); outside
  keys and plain operands are READS collected over rr into
  vm_vals[t] + reads (bif_phi_arg_src / bif_outside_arg_src /
  bif_plain_src / bif_collect_read). A read no exit reaches sets
  bad — dominance says it cannot execute; bail untouched.
- **the routes validator**: bif_routes (+ chain/merge/pred helpers)
  proves every read providable at its block entry — pure arm -> the
  VM carries it, chain -> the pred routes, merge -> one arg per
  pred — cycle-guarded, all BEFORE the commit's first edit: a
  mid-commit bail would leave a half-versioned fn.
- **the commit tail**: bif_clone_loop (bypass NONE — the
  while-test stays live in BOTH versions), bif_rewrite_branches
  (bif: br A / bif': br B'), the VM blocks (bif_emit_vm + phis:
  one phi per vm_vals entry, keyed {exit source, its clone}),
  bif_retarget_exits (both sides enter their VMs), bif_emit_guard
  (pre: cbr(INV) h / h'), the delivered walk (bif_delivered +
  arm/chain/merge walkers, bif_emit_merge_phi with the
  range-shift bookkeeping, memoized in dcache), and the rewire
  sweep (bif_rekey_phi for in-loop keys -> the VM,
  bif_rewrite_use for plain reads -> the delivered value).
- **the one retirement**: the monolith's `pi = 3' failure sentinel
  became a direct `return NONE' — same observable result (no phi,
  no dcache entry), one less piece of hidden state; documented in
  the impl header's THEORY block next to the
  validate-then-edit argument.

40 of the 57 functions are at most 10 lines; the rest are integral
steps (the flood step, the designated-initializer phi emitters,
the merge walkers). Every scan bound, emission order, counter
increment and preserved asymmetry is the original's.

### the plan: the LLVM-gap backlog (OPT_CHECKLIST.md)

The 127-item coverage analysis (download/LLVM_OPT_COVERAGE.md;
62% of the applicable items — IR-cleanup 86, control-flow 80,
memory 77, interprocedural 76, scalar 68, loop 54,
target-profitability 36, backend 34) became a ranked backlog:
4 high-value (PRE/LCM, DSE killing-store chains, dependence
analysis as the enabler, SCEV-as-shared-analysis), 9 medium (code
sinking, runtime-trip partial unrolling, loop predication,
aggressive DCE, splice chains, function outlining, attribute
inference, register coalescing, InstCombine rule growth), 4
machine-layer (block placement, asm peephole, spill placement,
shrink-wrapping), and the deliberately-not-planned list with
reasons on record — vectorization, cache, and instruction
selection excluded by request; scheduling delegated to the OoO
core; LCSSA deferred until PRE exists.

## The verification (all green)

- parity: 146 runs byte-identical vs the pre-refactor binary
  (47 programs x 3 variants, stdin-aware, + 3 debugger tours +
  --demo + the -O3 stats report) via scripts/parity_diff.sh
- audit: examples/audit/RESULTS.md regenerated — git clean
  (identical table)
- differential fuzz: 16131-16280 and 14231-14430, old vs new —
  identical modulo timing lines, zero mismatches (the stats line:
  36 unswitches, 49 body-if bait programs exercised)
- selftest 207/207; ctest 44/44; units 6328/6328; check-format
  clean; header order 101/101 + umbrella; includes 427/427

## Layout deltas

- new: src/minijz/opt/loop/unswitch_bif_impl.hpp (767 lines, the
  support layer + the full theory banner)
- rewritten: src/minijz/opt/loop/unswitch_bif.hpp (517 -> 87: the
  theory intro + the 21-line driver)
- OPT_CHECKLIST.md: unswitch_bif moved to Done; the LLVM-gap
  backlog section added
- README.md: this checkpoint's ledger entry (section 13)
