# Checkpoint 20 — tail duplication, the lcf-rt guard-bound miscompile, the recurrence chains

branchA @ (one commit on top of checkpoint 19's `bbf3c7b`). The
CONTINUE backlog's named next step — tail duplication — plus the two
things the pre-checkpoint probes surfaced: a REAL latent miscompile
in the runtime-trip closed form (shipped in checkpoint 18, survived
every fuzz window since because the fuzzer's loops always carry
per-iteration prints the LCF purity gate rejects) and the dead
recurrence-chain math the single-use absorption restriction kept
unreachable. Battery all green, re-verified immediately before the
commit.

## What landed

### 1. Tail duplication (`taildup`, toggle #32, tier 1 — `opt/scalar/taildup.hpp`)

A SMALL block T shared by exactly two predecessors, each ending in a
DEDICATED `br T`, is duplicated into both of them:

```
A: ... br T            A: ... [T-body w/ A's phi args] <T's term>
B: ... br T      ==>   B: ... [T-body w/ B's phi args] <T's term>
T: [phis] body              (T becomes unreachable; unreach removes it)
    br U
```

The merge block and its jump disappear from every path. T's phis do
not duplicate, they MOVE: each continuation U (preds == {T} exactly,
so after the duplication U's preds are exactly the two arms) gets one
phi per escaping value. Routing is per-USE — a value can escape
through BOTH continuations at once (read in U0's region and in U1's),
so routing by value would wire one arm through the wrong supplier.
A 2-pred re-merge whose arms arrive from different suppliers gets a
phi BORN there (one arg per pred, each arg the pred's supplier) —
the checkpoint-19 delivered-walk discipline, one nesting level per
re-merge: the in-loop if/else whose merged value feeds the loop
header's phi through the post-arm join.

The gates (soundness envelope, all in the pass header):
- T is not a loop header — a header whose body duplicated into its
  own latch would be loop ROTATION proper; this tree's loop
  consumers (unroll/lcf/licm) all match the canonical test-first
  shape, so the merge whose continuation IS the header (the latch
  merge) stays unduplicated by design.
- T's body <= 12 instrs of Const/Copy/Bin/Print (a Print printed
  once per path before prints once per path after; a may-trap
  Div/Mod is fine for the same reason — the trap ran once per path
  before and its copy runs once per path after).
- T's terminator Br/Cbr/Exit/Ret (the fn tail duplicated into both
  arms — no routing); every successor must have preds == {T}
  exactly, holding the <=2-preds invariant the 2-arg phi
  representation needs. Conditional preds stay unduplicated (they
  would need edge blocks this IR does not have); the survivors of
  tier-0's splice are exactly the fn tails and the cbr
  continuations — a following if.
- Anything else (a join of >2 preds, a cycle among unresolved
  blocks) bails the candidate.

The correlated-merge jackpot: `if (c) x=1; else x=9; y=x+5`
duplicates y=x+5 into both arms where tier-0 folding pre-resolves
it to 6 and 14 — the merge then selects between two constants. One
duplication per call; the fixpoint driver re-analyzes. Runs in tier
1 after gvn, before jthread (gvn's congruence cleaning first gives
the matcher its exact 2-pred shape; the duplicated bodies fold in
the same round).

### 2. Latent miscompile fixed: the lcf-rt guard read an in-loop bound

Reproducible on checkpoint 19, missed by every fuzz window:
`let i = input(); while (i < 40) { i = i + 1; s = s + 2; }` —
oracle 817, compiled 3. The runtime-trip guard block (emitted in
the preheader) read the bound vreg whose defining `Const` still
lived INSIDE the loop header: the while-lowering materializes
loop-test constants in the header, and licm only duplicates such a
Const out when it hoists an instruction reading it — the header
compare is never hoisted (its phi operand is loop-carried), so a
constant bound routinely stays defined inside the loop. Reading
that vreg from the guard is use-before-def (the guard executes
before the first header execution); destruction's register
allocator then coalesces the slot with the in-loop def and the
guard reads garbage. `LcfRtIn` now carries the bound's defining
block (`defb`, filled by lcf.hpp), and when the bound is an
in-loop Const the guard materializes a FRESH local copy at its top
(the same move licm's dup_const makes — a Const's value is
position-independent) and reads THAT everywhere it names the bound.
The fuzzer missed the class because its loops always carried
per-iteration prints, which the LCF purity gate rejects — the new
rt-lcf bait (below) plants exactly the missing vocabulary.

### 3. The recurrence chains: linform's ANALYSIS mode

`linform`'s absorption walk required `uses[v] == 1` (reassociation
RE-emits the chain it expands, so absorbing a shared def there
would duplicate or delete live computation). The lcf recurrence
scan only READS the linear form — the whole loop is deleted when
the closed form commits — so a shared def absorbs there soundly:
the scan now passes `analysis=true`, multi-use defs absorb, and the
MULTI-USE induction update (whose users are the feedback phi + the
accumulator it feeds: always >= 2) finally enters the linear form.
`while (i < B) { s = s + i; i = i + 1; }` becomes the
sum-of-series recurrence s' = s + i + 1 (b != 0) — the T(n) series
math in BOTH the analytic trip counter and the runtime Tv
construction has carried b != 0 handling since checkpoint 18 but
could never fire: the single-use restriction made every
induction-dependent recurrence a non-phi leaf term, and the scan
rejected it. Sum closed forms (read-first AND read-after-update,
step != 1 with an induction-dependent increment, far-past-the-bound
zero-trip arithmetic) are pinned by the new unit checks.

### 4. Fuzz

- `MJZ_TAILDUP_SEED_FLOOR = 14231`: seeds >= this plant TWO new
  flavors — the TAILDUP BAIT (an if/else merge feeding a second
  if (the cbr continuation — fn-level) or a loop body whose merged
  value feeds the header phi through the post-arm join (the born
  site), with per-iteration prints observing both arms, so a
  mis-routed merge phi or born phi shows immediately) and the
  RT-LCF BAIT (a PURE-body loop with a RUNTIME start and a constant
  bound/step, observed only PAST the loop — the exact vocabulary
  that would have caught the guard-bound miscompile; half the
  plants make the accumulator read the induction itself: the
  recurrence chain). 300-program bait window: 182 taildup baits
  (277 merges), 112 rt-lcf baits (133 loops), 581 taildups
  performed, all clean; 9 native-parity checks (fixtures committed
  as fuzz_native/fz14231..14239).
- The fuzz report's vocabulary line grew the
  `w/ taildup bait: N (M merges)` and `w/ rt-lcf bait: N (M loops)`
  fields; the stats line grew `taildups`.
- New off-case 19: seeds >= 14231 with `seed % 20 == 19` disable
  taildup — same discipline as the 18 existing cases, new seeds
  only, so every committed fixture keeps its exact old config.
- This session's runs: the bait window 14231-14530 (with the 9
  native parity checks), the following rotation window 14531-14830,
  random-cfg 13831-14030 and 14031-14230 (the backlog's 13831+
  floor) all clean; random-cfg 13631-13830 and rotation 13631-13930
  re-verified clean.

### 5. The debugger session ctest + tour demo

`taildup_debugger_session` (ctest, the ckpt-19 bifunswitch
convention): the tour demo's loop body hides an if/else merge whose
value feeds the header phi through the post-arm join (the born phi)
and whose terminator is the second if's cbr — `opt break taildup`
pauses the timeline right after the first duplication (the arms
carry the copied tail, the born phi keys the latch), `opt show`
dumps it, `opt continue` collects in full (the exit-tail merge
fires in round 2), and the five-way check proves the duplicated
program end to end. Demo pair: `demos/taildup_tour.mjz` + `.in`.

## Battery (re-verified before the commit)

selftest 207/207 | ctest 41/41 (new taildup_debugger_session) |
units 6262/6262 (+44 checkpoint-20 checks: the taildup series (a)
  fn-tail merge, (b) correlated merge, (c) if-chain, (d) in-loop
  merge, (e) growth-cap rejection, (f) the --disable=taildup toggle,
  (g) the miscompile repro + zero/negative-start trips, (h)-(k) the
  recurrence chains) | `format`/`check-format` clean | fuzz 1-200
(selftest), 13631-13930 (re-verified), 14231-14530 (bait, 9 native
parity), 14531-14830, random-cfg 13631-13830 (re-verified),
13831-14030, 14031-14230 — all clean.

## Known limits

- taildup deliberately refuses the loop-header continuation (the
  latch merge) and the loop header itself — loop rotation proper is
  the documented next step, not a missing piece here.
- The born-phi routing resolves ONE nesting level per re-merge (the
  delivered-walk discipline); deeper escapes bail the candidate
  (coverage, not correctness).
- A real Windows/MSVC run remains the one unverified step; this
  session's sandbox had no MSVC/zig/MinGW (and native parity ran on
  g++). Nothing in this checkpoint touches build/platform code (one
  ctest + one demo pair + fixtures added).
