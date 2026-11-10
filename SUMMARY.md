# Checkpoint 19 — body-if loop unswitching, the version-merge exits, the lcf-sim polarity fix

branchA @ (one commit on top of checkpoint 18's `d697225`). The
checkpoint-18 CONTINUE backlog, the natural next step first: the
classic LLVM "trivial" unswitch of an invariant INNER branch, the
unswitch_shape.hpp code-quality split that came with it, the
lcf-simulator polarity bug the new bait exposed within minutes (the
checkpoint-17 tradition continues), and the next fuzz windows. Battery
all green, re-verified immediately before the commit.

## What landed

### 1. BODY-IF unswitching (`unswitch` extended, still toggle #30)

The head test is a real while-condition (it varies per iteration), but
a body branch `bif: cbr(INV) A/B` — INV SSA-proven loop-invariant
(defined outside, dominating the preheader), both arms in-loop and each
entered only through bif — versions the loop:

```
pre: br h  ->  pre: cbr(INV) h / h'
               h  = the INV-true version:  bif: br A  (B dies)
               h' = a full fresh-vreg clone, the INV-false version:
                    bif': br B'                              (A' dies)
```

BOTH versions keep running the loop (the header test stays live in
each) — sound because the original picked the SAME arm every
iteration: INV is an SSA value, it cannot change mid-loop. No purity
gates beyond INV's invariance: each version executes its chosen arm's
side effects per iteration exactly like the original did.

Every exit edge — the header's exit arm INCLUDED (no zero-iteration
edge is replaced here) — gets a VERSION-MERGE block VM: preds {exit
source, its clone}, one phi per loop-defined value read at or through
the exit target (`phi(v @ src, fresh v @ clone)`), then br to the
target. That holds the <=2-preds invariant the 2-arg phi
representation needs. Outside reads of loop-defined values are routed
by a DELIVERED walk: pure break arms (single in-loop pred) read the VM
directly; single-pred chains pass the value up; a 2-pred merge whose
values arrive per-pred — the post-loop print of a loop with both a
header exit and a guarded break, which the SSA builder left as PLAIN
reads because the header phi dominates the whole post-loop region —
gets a fresh phi born at the merge (one arg per post-pred: the VM for
an in-loop pred, the recursively delivered value for an outside pred).
The walk is validated (cycle-guarded, sources checked) BEFORE the
commit's first edit. The dead arms and their merge phis degenerate
under unreach + copyprop in the same tier round, the way the
checkpoint-17/18 abandoned loop bodies do.

24 new unit checks pin: the canonical polarity pair (the +1 version
lands exactly on the bound, the +2 version overshoots — the exit value
DIFFERS between versions, so the VM merge is load-bearing),
per-arm observation prints, the break-arm read (a plain outside use of
a loop-defined value at a pure arm), the 2-pred-merge delivered phi,
both rejections (a WRITTEN flag is not invariant; an arm that breaks
leaves the loop), and guarded division inside the arms.

### 2. Latent bug fixed: the LCF general simulator's exit polarity

The new bait miscompiled bif2-style programs (unswitch + splice + lcf
all required): after versioning, the true version's loop — entry
threading + splice can carry the `break` condition AS the header test
(exit-on-TRUE) — hit `lcf_sim_fold`, which evaluated the header test
with continue-style polarity only (exit on FALSE): it concluded
zero trips and folded the exit through the WRONG edge (the break arm)
with ENTRY values. `LcfSimIn` now carries `exit_on_true` and the
simulator evaluates the loop's own polarity, the way the analytic trip
counter has since checkpoint 17's fix of the same landmine class.

### 3. Code quality: the unswitch split

`opt/loop/unswitch.hpp` (706 lines with the compound matcher) now:

- `opt/loop/unswitch_shape.hpp` (~300) — the shared layer: the
  `cond_invariant` gate, the `ExitEdge` inventory (the body-if path
  passes primary=NONE: its header exit is a REGULAR exit edge), the
  checkpoint-18 compound matcher (moved verbatim), the checkpoint-19
  `BifShape` matcher, and the clone infrastructure (`CloneMap`
  bmap/fresh allocation + the append-only `emit_loop_clone` with the
  `bypass` parameter — NONE keeps the cloned header's test, the
  body-if shape; the trivial/compound shapes strip it).
- `opt/loop/unswitch_bif.hpp` (~340) — the body-if scan (the
  reverse-reachability collection + the delivered-walk validation) and
  the commit (clone, version rewrites, VM emission, retarget, guard,
  delivered emission, rewire).
- `unswitch.hpp` (~430) — the slim driver: path selection (trivial →
  body-if → compound), the shared checks, and the checkpoint-17/18
  trivial/compound commits on the shared helpers.

### 4. Fuzz

- `MJZ_BIF_SEED_FLOOR = 13631`: seeds >= this plant
  `while (cn < N) { if (bf > 0) cn+1; else cn+2; print cn; } print cn;`
  — a variant head test plus an invariant body branch, both arms
  converging, per-iteration and post-loop prints making the
  version-dependent values observable (the exit overshoot differs per
  version, so a miswired VM merge shows). 133 of the first 300
  new-window programs carry one (172 loops); committed fixtures keep
  byte-identical programs (no new toggles, rotation stays %19).
- The fuzz report's vocabulary line grew a
  `w/ body-if bait: N (M loops)` field.
- Windows-adjacent run in THIS session: rotation 12331-12630,
  12731-13030, 13031-13330, 13331-13630 re-verified, the new
  13631-13930 and the following 13931-14230 both clean, random-cfg
  10801-11000 / 12731-12930 / 12931-13130 re-verified, the new
  random-cfg 13131-13330 and 13631-13830 (bait under random pass
  configs: 91 programs, 117 loops) clean. 9 native-parity checks in
  the bait window (fixtures committed as fuzz_native/fz13631..13639).

## Battery (re-verified before the commit)

selftest 207/207 | ctest 40/40 (new bifunswitch_debugger_session) |
units 6215/6215 (+24 body-if checks) | `format`/`check-format` clean |
fuzz 1-200 (selftest), 12331-12630, 12731-13030, 13031-13330,
13331-13630, 13631-13930, 13931-14230, random-cfg 10801-11000,
12731-12930, 12931-13130, 13131-13330, 13631-13830 — all clean.

## Known limits

- The bif matcher requires each arm entered ONLY through bif (the
  canonical if-lowering); shared arm blocks stay unversioned.
- The delivered walk bails on outside cycles (sibling loops reading
  this loop's values at their headers) — the scan's visiting guard;
  such shapes do not unswitch (coverage, not correctness).
- A real Windows/MSVC run remains the one unverified step; this
  session's sandbox had no MSVC/zig/MinGW either (and no clang++ — the
  native-parity checks ran on g++). Nothing in this checkpoint touches
  build/platform code (one ctest + one demo pair added).
