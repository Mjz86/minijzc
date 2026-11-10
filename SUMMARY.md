# Checkpoint 18 — compound-condition loop unswitching, the lcf split, pass-breakpoint auto-pause

branchA @ (one commit on top of checkpoint 17's `f651041`). The
checkpoint-17 CONTINUE backlog, all four items in one commit:
the unswitch extension for short-circuit conditions, the
lcf.hpp code-quality split, the debugger's `opt break`
auto-pause, and the next fuzz windows. Battery all green,
re-verified immediately before the commit.

## What landed

### 1. Compound-condition loop unswitching (`unswitch` extended, still toggle #30)

The while-lowering spreads `while (a && b)` / `while (a || b)` over a
diamond: the header tests a, the elbow E tests b, the 1/0 blocks
TT/FF feed the merge mm whose phi+cbr IS the loop's exit edge. The
trivial unswitch bailed on those (the header's cbr has BOTH arms
in-loop, so `loop_shape_of` fails and nothing versions the loop).

When BOTH leaf tests are SSA-proven loop-invariant (defined outside,
dominating the preheader — licm's header scan + the ordinary hoist put
the compares in the preheader by the time tier 2 runs), the whole
guard chain is now versioned as one:

```
&& : pre: cbr(a) g2 / fx ; g2: cbr(b) H' / fx ; fx: br X
|| : pre: cbr(a) t1 / g2 ; g2: cbr(b) t1 / fx ; t1: br H' ; fx: br X
```

(per-arm polarity: && exits on either false, || enters on either
true). The true path H' is a full fresh-vreg clone whose header
branches straight to the REAL body — the entire condition diamond is
bypassed and becomes unreachable in the clone (unreach removes it;
in the clone the condition cannot change, so the dead exit edge is
semantically harmless, exactly like the original). The mm->X edge
becomes the zero-iteration edge: X's phi args keyed to mm are rekeyed
to the new false-side block fx and take each header phi's ENTRY value
(the zero-iteration original left the variables untouched); break
edges are rekeyed to their clones as before; merge phis at X keep the
per-path meanings of header phis read on the false path. All the
pred-count math preserves the ≤2 preds invariant on the live CFG (the
transient extra preds are dead original/diamond blocks, removed by
unreach within the same tier round).

Safety gates beyond the trivial path's: the diamond blocks
(h/E/TT/FF/mm) must be pure — Const/Copy/Bin with `nt` on Div/Mod —
because the false path skips them and skipped side effects would be a
miscompile (the invariance check already rejects anything that
computes the conditions in-loop; this is belt and braces), and the
bypass target must be entered only through mm (no phis to rekey).
Nested short-circuits (`a && b && c`, `(a && b) || c`) stay
unversioned — the matcher handles exactly two leaf tests.

Unit checks pin all three polarity paths for both && and ||, the
non-invariant rejection, and a guarded-division loop (the && guard is
control-flow-sensitive; the versioning must not hoist the division).

### 2. Code quality: the lcf split (+ constcall's real home)

`opt/loop/lcf.hpp` was the tree's biggest function (~1150 lines of one
`opt_lcf`). Now:

- `opt/loop/lcf_sim.hpp` — the general any-condition simulator
  (`lcf_sim_fold`: the "fold the GCD demo to a constant" path) + the
  shared budgets.
- `opt/loop/lcf_rt.hpp` — the runtime-trip guarded closed form
  (`lcf_rt_fold` + the `LcfEvo` affine-recurrence record).
- `lcf.hpp` (now ~707) — the driver: scan, loop shape, induction,
  trip counts, the const-trip closed forms and the exact simulation
  fallback. Behavior is byte-identical: the extractions are verbatim
  moves behind explicit call contracts (the selftest's LCF demos,
  `lcf_rt_*` included, pin the counts).
- The const-call evaluator — which historically squatted in
  `loop/lcf_rt.hpp` under a header comment describing the runtime
  trip counts — moved to its real home
  `opt/interprocedural/constcall.hpp` (driver/callgraph/callcse/
  minijz.hpp includes updated).

### 3. Debugger: `opt break` auto-pause

`opt break PASS` now ARMS the pass breakpoint in addition to showing
the current view. The next `opt [FN]` collection PAUSES there: the
timeline truncates after that pass's first snapshot, the pause banner
names it, and the pass's diff against the previous snapshot is
printed — the breakpoint between passes now actually stops, the way
`vm break` stops execution. `opt continue` disarms and re-collects
the full timeline. New scripted ctest `passpause_debugger_session`
arms sccp + unswitch, pauses, inspects with `opt show`, continues, and
five-way checks.

### 4. Fuzz

- `MJZ_CC_SEED_FLOOR = 13331`: seeds >= this plant
  `while (fl1 > 0 && fl2 > 0)` / `||` loops with both flags invariant
  (never assigned inside) and termination riding on a guarded break —
  the compound-unswitch bait. 127/300 programs in the new window carry
  one (169 loops); committed fixtures keep byte-identical programs
  (no new toggles, so the rotation stays %19 for everything >= 12731).
- The fuzz report's vocabulary line grew a
  `w/ compound-cond bait: N (M loops)` field.
- Windows run in THIS session: rotation 13031-13330 and 13331-13630
  (the new compound window), random-cfg 12931-13130, plus re-verified
  12331-12630 / 12731-13030 / random-cfg 10801-11000 and 12731-12930 —
  all clean, no timeouts. 8 native-parity checks in the bait window
  (fixtures committed as fuzz_native/fz13331..13339 minus one
  timeout-oracle seed).

## Battery (re-verified before the commit)

selftest 207/207 | ctest 39/39 (new passpause session) | units
6191/6191 (+16 compound-unswitch checks) | `format`/`check-format`
clean | fuzz 1-200 (selftest), 12331-12630, 12731-13030,
13031-13330, 13331-13630, random-cfg 10801-11000, 12731-12930,
12931-13130 — all clean.

## Known limits

- Compound unswitch handles exactly TWO leaf tests (`a && b`,
  `a || b`); deeper boolean trees stay unversioned (documented in
  the README wishlist notes).
- A real Windows/MSVC run remains the one unverified step; this
  session's sandbox had no zig/MinGW either, so nothing was
  re-cross-verified Windows-shaped — nothing in this checkpoint
  touches build/platform code (one ctest added), and the ckpt-16
  Windows machinery is unchanged.
