# SUMMARY — checkpoint 30: the backend kernels lifted + DSE

Three strands, one session: the backend's analysis components became
library kernels (regalloc's liveness/interference, emit_plan's
RPO/trace layout, stack_alloc's closure/escape scan), the
OPT_CHECKLIST's rotate got the decomposition treatment, and the
LLVM-gap backlog's DSE landed as a new pass — kernel in mjz_lib,
window policy in the compiler. The mjz_lib work now rides a TRACKED
BRANCH (graph-opt-kernels, six commits) — the diff is reviewable
with `git diff main..graph-opt-kernels` inside the vendored copy.

## 1. opt/liveness.hpp + opt/layout.hpp + opt/escape.hpp — the backend analyses, generic

The founding rule applied to the BACKEND's own algorithms (the
component-extraction arc: opt_ns 18 sections, then ssa_ns, now the
backend trio):

* LIVENESS (`calculate_block_liveness` + `calculate_interference_rows`):
  the backward round-robin fixpoint (live-out = the union of
  successor live-ins; sites scanned backward def-kills-use) and the
  interference rows (a def conflicts with everything live after it;
  the entry defs pairwise always — fuzz seed 804's founding
  `mov %rsi,%rbx` bug, now documented in the KERNEL). regalloc.hpp
  materializes tables (the fuse/kc visibility reader stays) and
  calls; the coloring and home policy are untouched.
* LAYOUT (`calculate_reverse_postorder` + `calculate_trace_layout`):
  the slot-order DFS RPO (loop headers precede bodies) and the trace
  chains (smallest-score open successor, ties to the earliest slot —
  a binary backend's false arm falls through). N-ary on purpose: a
  3-way branch chains by the same rule. emit_plan's phases 1-3 are
  now three calls with the pred-count score.
* ESCAPE (`calculate_copy_closure` + `scan_position_escapes`): the
  monotone alias fixpoint and the positional scan (safety flags per
  operand position; missing flags are unsafe — the analysis fails
  closed). stack_alloc's whole job is now the POSITION POLICY table
  (which operand of which kind may see a handle), materialized once
  for all candidates.

Fuzzers `opt.liveness` (vs the gen/kill equations swept in the
OPPOSITE order — a different algorithm, not a copy — plus a
var-major pairwise interference enumeration with a fresh per-site
live-after rescan), `opt.trace` (vs a RECURSIVE DFS RPO + the greedy
replica + structural invariants), `opt.escape` (vs a BFS closure +
a var-major scan). All three: byte-identical 146-run parity for the
adapters, 30k seeds clean.

## 2. rotate decomposed (round eight)

opt_rotate 405 lines -> 25-line driver + rotate_impl.hpp: RotCtx,
the gates (rot_shape_ok / rot_exit_ok / rot_header_phis_ok /
rot_collect_hbody), the classification (rot_mark_def_sets,
rot_after_region, rot_classify_reads), the value maps (rot_gmap/
lmap/rmap/xmap, the generic rot_rw, rot_test_cbr), and the commit
tail (rot_mint_vregs preserving the OBSERVABLE gv/lv-then-pv/xv
allocation order; rot_emit_pre/header/latch/exit + the in-loop/F/
verbatim arms; rot_succ_rewire). Verified byte-identical: 146-run
parity, both committed fuzz windows (16131-16330 rotation-shaped,
14731-14930 random-cfg) identical modulo timing, audit identical.
OPT_CHECKLIST: rotate marked done; the stale mem2reg NEXT UP entry
corrected (that was round seven's work, already done).

## 3. DSE — the in-window dead-store pass (the LLVM-gap backlog)

`opt/scalar/dse.hpp` (tier 1, directly after ldelim — forwarded
loads are Copies, transparent here, so the windows are as wide as
they can get) over `opt_ns::dse_ns::calculate_dead_stores`
(mjz_lib/opt/dse.hpp):

* THE GATE: a single-proven handle, a KNOWN CONST index, a known
  const length, index in [0, L) — the slot key is EXACT and the
  access cannot trap. Both are load-bearing: a trap reports kind AND
  line, so a MOVED trap is an observable change.
* THE WINDOW: Div/Mod, gate-failing accesses, Print/Input/Call/
  Ret/Exit/Bkpt/Alloc break it; Const/Copy/plain-Bin/Phi/Len are
  transparent.
* THE RULES (in order, one per pair — both firing would leave the
  slot unwritten): SAME-VALUE (a redundant re-write; the SURVIVOR
  keeps the entry, so a chain of equal stores all dies against the
  first) and OVERWRITE (a fresh store kills the unread remembered
  store).
* TWO INTEGRATION BUGS CAUGHT BEFORE COMMIT: the kernel's dead
  ordinals are ALL-EVENT ordinals — the first draft paired them
  through a stores-only array and dropped the WRONG instruction (the
  --opt-diff IR dump caught it; the kernel was right, the client
  mapping was wrong); and sum_ostats (the per-fn merge) did not know
  the new counters. The fuzzer's differential gate also caught one
  wrong REFERENCE formulation (same-valued stores ahead are
  TRANSPARENT to the overwrite scan — the survivor-chain semantics)
  before any integration: the gns.rmasp precedent holding.

Witness: `a[i]=7; print a[i]; a[0]=1; a[0]=2; print a[0]; a[1]=3;
print a[1]; a[1]=4; a[1]=3; print a[1]` (the runtime index keeps
mem2reg away) -> DSE 2+0, both kills the trace predicts, oracle ==
ssa == lower == vm.

## The branch tracking (the user's ask)

mjz_lib is its own git repo, now on branch `graph-opt-kernels`:
six commits (the graph headers, the CSR rework, the 18 opt kernels,
the backend trio, the dse kernel), 32 files / +7,300 lines over
main — `git -C mjz_lib diff main..graph-opt-kernels` is the review
surface, and the .git rides in the zip so the history ships. The
main repo is 13 commits ahead of origin/main (not pushed, per the
protocol).

## The battery (all green)

selftest 208/208 | units 6476/6476 | demo 206/206 | kernel campaign
60 components x 30,000 seeds (44.3M checks; opt.liveness 2.0M,
opt.dse 581k), 0 failures | e2e differential fuzz ~5,100 seeds across
four windows (1-1477, 10000-11200 pre-DSE; 1-1300, 20000-21200
post-DSE), 0 mismatches | 146-run byte-identical parity for every
refactor | fuzz windows 16131-16330 + 14731-14930 identical modulo
timing (pre-DSE) | stress ALL PASS | bugrepros 10/10 | audit table
regenerated IDENTICAL | header order 112 + umbrella | includes
499/499.

## The build shortcut (unchanged from checkpoint 29)

The cmake shim + scripts/rebuild.sh; the recipe is in CONTINUE.md.
clang-format 16 was NOT available this session (the cosmetic gate is
recorded in OPT_CHECKLIST's protocol; the new code follows the tree
style by hand).
