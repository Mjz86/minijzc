# SUMMARY — checkpoint 29: the polynomial reducible-MFAS + breq

BranchA @ one commit on top of checkpoint 28's taildup commit. Two
deliverables, one graph library, one compiler pass; the full battery
green and the widened fuzz campaign clean. This file is the tour;
CONTINUE.md is the resume manual.

## 1. graph/rmas.hpp — the polynomial minimum feedback arc set for REDUCIBLE flow graphs

`calculate_minimum_feedback_arc_set_reducible(root, graph, weights)` —
Ramachandran's answer (J. Algorithms 9, 1988; the min-max companion SIAM
J. Disc. Math. 1990) as the library spells it: one Dinic min cut per
natural loop, polynomial, exact — validated against the exact 2^V sweep
on every seed.

* THE CONTRACT CHECK: strip the arcs whose target DOMINATES their
  source, demand a DAG — both directions constructive; an irreducible
  input (or an unreachable node) reports itself in the result and
  returns empty; the exact exponential sweep above it remains the
  answer for irreducible graphs.
* THE STRUCTURE: cycles partition by their unique on-cycle header
  (two headers on one simple cycle would dominate each other); the
  natural loops are laminar; an outer cycle crosses an inner region
  only along header->exit TRANSIT paths, never the inner back arcs.
* THE PACKING (dual witness, rides home in packing_weight): the cycles
  packed loop by loop, INNERMOST FIRST, as MIN-COST flows over the
  SHARED arc capacities — an arc's cost is the number of enclosing
  loops whose header paths cross it, so the flow spends the private
  arcs first and saves the contested transit for the loops that have
  no alternative. A feasible packing bounds every FAS from below (the
  min-max theorem); when it meets the committed weight the minimum is
  proven outright.
* THE CUTS (the answer): OUTERMOST FIRST, each loop's arcs priced by
  the packing loads of its proper inner loops — w(e) minus what the
  inner flows route over e. The loads are the right ledger where
  per-arc drops are not: deleting one of two parallel transit arcs
  merely REROUTES the inner flow (drop credits undercount the pair),
  while the loads sum to the inner flow value over ANY of its cuts —
  the group credit is exact. A fully loaded arc is free to the outer
  loop because the inner loop's own next cut pays it back. Each
  committed cut is filtered to arcs still on some header path.
* THREE DESIGNS DIED ON THE GATE to get here (each pinned by a fuzzer
  seed, each a named lesson in the header comment): the isolated
  innermost cut (seed ex1: pays the cheap back arc, the outer pays
  again in the tail), the isolated outermost cut (the mirror), and
  the drop-discounted ledger with mandatory-arc filters (seed 2601:
  the lone crossing arc carries the whole flow unsaturated — min-cut
  membership is NOT the credit; seed 496: parallel pairs reroute).
  The shipped design holds the exact minimum on EVERY seed.
* VALIDATION: gns.rmasp in the component fuzzer (the differential vs
  the exact 2^V sweep on every seed, the packing bound, the
  irreducible-witness-empty check, the order certificate) — 30,000
  seeds, 232,656 checks, 0 failures; PLUS one-off big-n batteries:
  400 seeds at n=6..14 and 2000 deep-nest seeds (tall trees, 3-level
  loop chains), all exact.

## 2. opt/scalar/breq.hpp — branch folding on reconverging transparent arms

The OPT_CHECKLIST's "aggressive DCE (the control-dependence kind)".
A Cbr whose two arms are PRIVATE (entered only from the branch),
phi-free, br-only RELAYS that reconverge on one join — or one arm IS
the join and the other relays into it — with the join's phis carrying
the SAME vreg on both arm edges, folds to `br J`: the phis rekey to
the single folded edge (the duplicate-pred shape, which phi_degens
collapses next round), the relay arms go unreachable for unreach, the
condition loses a use for dce. fold's cbr_same owns the
identical-TARGETS case; breq owns the RECONVERGING case — the shape
`if (c) {} else {}` leaves behind after the arms are stripped to the
bone. One fold per invocation; the tier-0 loop fixpoints it.

* Wired as toggle `breq` (tier 0, on at -O1+), stat `cbr~` in the -O3
  report, PASS_TOGGLE_NAMES/apply_pass_flag/describe_cfg/cli all
  updated; the fuzzer's --fuzz-cfg=random exercises it on/off.
* Witness: `fn f(x){ if (x>3) { return 9; } return 9; } print(f(input()))`
  folds to one straight-line return (cbr~ 1, dead-blk 2, dce 4) with
  oracle == compiled.
* FIXED ON THE WAY: dbg_opt's toggle table was 31 entries against
  PASS_TOGGLE_COUNT 33+ (taildup/rotate already overran it; breq
  would have too) — completed; and the tree re-formatted
  clang-format-16 clean (12 files the last session left drifted).

## The battery (all green)

selftest 208/208 | units 6476/6476 | demo 206/206 | cx checks 7/7 |
kernel campaign 59 components x 30,000 seeds (43.7M checks), 0 failures | e2e
differential fuzz 2,400 seeds (windows 1-800/801-1600/1601-2400) +
400 random-config seeds, 0 mismatches | fuzz_native_gate | stress
battery ALL PASS | all debugger tours + bugrepros | header order
109+umbrella | includes 481/481 | check-format clean tree-wide
(clang-format 16.0.6) | audit table regenerated IDENTICAL (breq does
not fire on the 20 audit programs).

## The build shortcut this checkpoint needs

The sandbox recycles the pip venv (cmake lives there); the tree now
carries scripts/shim/cmake — a minimal shim that fakes the
build-system checks and RUNS link scripts — plus scripts/rebuild.sh
(`find src -name '*.cpp' -exec touch {} +; make -C build`) because
the shim skips cmake_depends (header mtimes don't always propagate;
touch the TUs). CMakeCache.txt's CMAKE_COMMAND points at the shim.
If the shim directory is missing (a recycle), recreate it — the
recipe is at the top of the file.
