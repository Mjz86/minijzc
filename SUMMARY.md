# Checkpoint 22 — loop rotation proper (the latch merge, consumed exactly as predicted)

branchA @ (one commit on top of checkpoint 21's `9d4d234`). The
CONTINUE backlog named it — "loop rotation proper (the taildup gates
already point at it: the latch merge — the merge whose continuation
IS the loop header — is the exact shape rotation consumes)" — and
this checkpoint consumed it. One new pass (`rotate`, toggle #33),
one selftest-caught regression class fixed on the spot, one fuzz
vocabulary gap closed (rotation bait), the audit table regenerated
against the rotated code, and the strongest fixture-stability
discipline yet (the below-floor gate, verified on both older
windows). Battery all green, re-verified immediately before the
commit.

## What landed

### 1. Loop rotation proper (`rotate`, toggle #33, tier 2, LAST loop consumer — `opt/loop/rotate.hpp`)

The canonical test-first loop becomes test-last with a one-shot
guard. The design decision that makes it exact in THIS IR:

- The whole non-phi h-body MOVES (never copies-in-place) to the two
  edges — a guard copy appended to the preheader (the 1st header
  run) and an edge test appended to the latch (the (i+1)-th header
  run). Moving (not cloning into place) is what preserves side-effect
  counts: k trips = k+1 header runs in both forms, and the guard
  copy IS the 1st while the latch copy of iteration i IS the (i+1)-th
  (nothing sits between a latch end and the next header top), so
  Prints, Inputs, Calls, Div/Mod traps and Bkpt observations all
  fire identically in count and order.
- The header phis keep keys AND args unchanged — pre still enters h
  (the guard's loop arm) and the latch still branches back, so
  nothing re-keys; preds(h) is literally the same {pre, latch}.
- Loop-internal readers of a moved def get a fresh header phi
  carrying (guard copy, latch copy) — exactly the (pre, latch)
  pattern the original phis use. The after-loop region F (reachable
  from X loop-free) reads an h-phi through a fresh exit phi whose
  args are the header phi's OWN (pre_arg, latch_arg) pair relocated
  past the loop — and a moved h-body def through (guard copy, latch
  copy).
- Result: one control transfer fewer per iteration (the header cbr +
  the latch br fuse into the latch cbr); tier-0 splices the now-empty
  fall-through chain afterwards.

Soundness envelope (the strict unroll/lcf family): canonical shape
(loop_shape_of), unique pre + latch with latch != h, dedicated
`br h` preheader, the ONLY exit edge is the header's (exit_inventory
empty — no breaks) with preds(X) == {h}, a single-pred X carrying a
phi bails (paranoia — the tier-0 degenerater should have removed
those), h-body <= ROT_BODY_MAX (8) instrs, and outside uses of
loop-defined values only through the HDEF set (h-phis + h-body defs,
the only values that exist on a 0-trip path) and only in F — anything
else bails (dominance forbade it before the rotation anyway). The
guard reads only values dominating the preheader — the checkpoint-20
lcf-rt use-before-def lesson is structurally impossible here: every
h-body read is an h-phi (mapped to pre_args), an earlier h-body def
(the guard's own copy), or a def dominating h which necessarily
dominates pre.

Position: tier 2, after cdfold. Unswitch/unroll/lcf all require the
test-first shape, so they get their chance first and the rotated loop
(header ends in Br) is simply invisible to them afterwards — one
rotation per call, the fixpoint driver re-analyzes, nothing
re-rotates.

### 2. The selftest-caught regression: the header-increment loop

The `break_continue` showcase (`while (1) { i = i+1; if (i > 10)
break; ... }`) failed with an infinite loop on the first working
draft — tier-0 folds the constant header test and splices the break
into the header's cbr, so the INCREMENT lives in the header: an
h-phi whose latch arg IS a moved h-body def. Three phi paths needed
routing through the moved def's fresh header phi:

- the header phi's own latch-side arg (the back edge must carry the
  def's per-position value, not the moved def itself);
- the latch copy's read of that phi (it needs the value the phi takes
  at the NEXT header position — which is exactly the phi_v carrier);
- the exit phi's latch-side arg (the (k+1)-th computation, i.e. the
  latch copy's own output).

Plus the copy-dst rewrite the checkpoint-20 landmine list already
named: the first draft's copies kept the ORIGINAL dst while the
guard/latch cbrs read the fresh vregs — both copies defined the same
vreg, the tests read never-defined values (the oracle-vs-compiled
mismatch was immediate). The unit series (c) pins the exact program.

### 3. The fuzz vocabulary gap: rotation bait

The window's first run reported `rotations: 0` — the fuzzer's plain
while loops are constant-bound and constant-driven (SCCP/unroll eat
every one of them; the rt-lcf bait loops are pure, so lcf eats
those). The missing vocabulary, now planted for seeds >= 15531
(MJZ_ROTATE_SEED_FLOOR, fuzz_gen): a RUNTIME start (`input() % 12` —
clamped, because this bait EXECUTES its loop — no closed form), a
constant bound/step (unroll cannot count, unswitch has no
invariant), and a PER-ITERATION PRINT (the LCF purity gate
rejects) — the exact while that survives every earlier consumer and
lands in the rotation. 121 bait loops in the 15531-15830 window,
143 rotations performed, native parity on 20 fixtures.

### 4. The strengthened fixture discipline (the below-floor gate)

fuzz_cfg_for now disables `p_rotate` for every seed BELOW the floor
(not just an in-window off-case for variety): the pass joins the
fuzz config only from 15531 up, so the committed fixtures of the
older windows keep regenerating byte-identically under this and
future binaries. Verified empirically on BOTH surviving windows:
fz14831..14839 (checkpoint 21's) and fz14231..14239 (checkpoint
20's) regenerate byte-for-byte with the new binary. Inside the
window, case 20 (seed % 21 == 20) keeps the test-first shape for
1-in-21 seeds — pass-interaction variety, the same shape as every
window before it.

### 5. The register audit against the rotated code

The checkpoint-21 audit exists to measure exactly this kind of
change: `scripts/regen_audit_results.py` (committed) regenerates
examples/audit/RESULTS.md from the current binary — the prose
headers and input lines preserved verbatim, the tables and outputs
rebuilt from fresh `--reg-audit --check-all` runs. The twenty
programs' totals moved 644 vregs / 537 housed / 107 spill slots /
345 traffic -> 710 / 573 / 137 / 424: the rotation's guard +
latch-edge copies and the routing phis add live values at the loop
edges (the FIR filter went spill-free -> 8 slots — the measured
price of one fewer control transfer per iteration; horner,
checksums, trial division, the Life census and the deep-recursion
pair stay spill-free). The audit is the observer, not the judge:
the tradeoff is now on the record.

### 6. Demo + gates

`demos/rotate_tour.mjz` + `.in`: three whiles — the plain impure
natural loop, a CALL IN THE LOOP CONDITION (the whole moved header
keeps its side-effect count — probe's prints prove k+1), and
after-loop reads (the exit phis). `rotate_tour` ctest (five runners
+ the audit) and `rotate_debugger_session` (scripted: `opt break
rotate` pauses the timeline, `opt show 33` dumps the rotated loop —
guard in b0, phis + fall-through Br in the header, edge test in the
latch, exit phis at X). 34 new unit checks (the series a-k: shape,
five-runner parity, zero-trip, the header-increment regression,
call counts, nested, polarity flip, the surviving-break bail, the
lcf precedence, the toggle, bkpt parity, a two-caller callee's own
rotation).

## Battery (re-verified before the commit)

selftest 207/207 | ctest 44/44 (new rotate_tour +
rotate_debugger_session) | units 6328/6328 (+3 toggle-table growth,
+34 rotation checks) | `format`/`check-format` clean | fuzz
15531-15830 (143 rotations, 20 parity fixtures committed) +
15831-16130 (167 rotations) + 14831-15130 re-verified +
14231-14530 re-verified + random-cfg 14431-14730 and 15531-15830
(67 rotations) — all clean, 0 mismatches; the old windows
regenerate their fixtures byte-identically.

## Known limits

- A single-block self-loop (latch == header — an inlined loop
  spliced into one block) bails: the documented envelope, not a
  silent skip (the unit test (k) documents it).
- Loops with a SURVIVING break edge stay test-first (exit_inventory
  non-empty) — the strict unroll/lcf family discipline; tier-0 folds
  the classic `while(1)+break` into the header so most of those DO
  rotate (the unit test (c) is exactly that program).
- The audit table regenerated at 137 spill slots vs 107: rotation
  trades register pressure for per-iteration branch count on this
  backend's five-register pool — the honest measurement, not a
  regression to hide (live-range splitting / rematerialization of
  the guard copies are the natural follow-ups, and the audit now
  measures them).
- A real Windows/MSVC run remains the one unverified step (no
  MSVC/zig/MinGW in this sandbox; native parity ran on g++).
