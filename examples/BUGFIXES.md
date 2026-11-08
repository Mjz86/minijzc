# MiniJZ bug fixes — 2026-11 (found via differential fuzzing)

> **Merged-tree note (3-branch merge, 2026-11-08).** This write-up comes
> from the bugfix branch; it was written against the OLD monolith layout
> (`src/NN_*.hpp`). In this repo the affected code lives in the split tree:
>
> | then (monolith)       | now (split tree)                               |
> |-----------------------|------------------------------------------------|
> | `src/50b_scctco.hpp`  | `src/minijz/opt/interprocedural/scctco.hpp`    |
> | `src/50c_tco.hpp`     | `src/minijz/opt/interprocedural/tailrec.hpp`   |
> | `src/40_opt_core.hpp` | `src/minijz/opt/scalar/canon.hpp`              |
> | `src/80_demo.hpp`     | `src/minijz/apps/demo.hpp`                     |
>
> All five fixes are present in this tree. Bugs 1, 2, 4, 5 were re-fixed
> independently on this line (f8ad66c, 45fc321, e49d59c) with the same
> root causes; bug 3's empty-block guard (§3) was ported from the bugfix
> branch during the merge, and the five `regr_*` demos + the
> `DEMO_INS_NOTNOT` tape were ported verbatim into `src/minijz/apps/
> demo.hpp` (`--selftest`: 186 → 206 checks). See §5 for one extra
> correction this tree carries beyond the bugfix branch's own fix.
> Historical repro programs for all five live in `bugrepro/`.

All six bugs were found by running the differential fuzzer over seed
ranges it had never seen before (`--fuzz=N --fuzz-from=S`, for bug 6 with
`--fuzz-cfg=random`), plus `examples/general.mjz` (which previously
miscompiled at -O3). Bugs 1–5 each have a `regr_*` demo in
`src/minijz/apps/demo.hpp`, so `--selftest` fails if any of them
regresses (the count went from 186 to 206 checks); bug 6 is a compile-time
hang guarded by `bugrepro/s7406.mjz` under `ctest -L regr`.

## 1. `regr_scc_dispatch` — scctco sibling copy kept reading the entry argument

**File:** `src/50b_scctco.hpp` → `src/minijz/opt/interprocedural/scctco.hpp` (`promote()`)

A sibling function inlined into an SCC member has its parameters mapped to
the *call-site arguments*. When a back edge into the copy arrived, the copy
grew merge phis and `promote()` rewrote the copy region — but skipped the
copy's own entry block. The entry block's body (e.g. an `n % 4 == 3`
dispatch test) kept reading the **raw entry argument**, so every loop
iteration re-tested the first entry's value: an `a → b → b` chain read `a`'s
`n/4` at every hop, returned the wrong member's base value or trapped.

**Fix:** rewrite the entry block's body *after* the NV prepended phis
(`begin + NV .. end`) from `root_vals` to the merge-phi results.

## 2. `regr_self_multisite` — tce_self emitted its latch chain backwards

**File:** `src/50c_tco.hpp` → `src/minijz/opt/interprocedural/tailrec.hpp` (`tce_self()`)

`tce_self` pre-assignes latch block ids `Mb[j] = nb + j` but emitted the
blocks in the loop `for (j = k-1; j >= 1; --j)`, appending M_{k-1}'s code
first. For k ≥ 3 tail sites the block at `Mb[1]`'s id actually contained
M_{k-1}'s code — the chain was reflected: M_2 → M_3 → M_2 cycled forever,
the loop header's latch predecessor held the wrong phis, and `phi-degen`
then deleted the header phis entirely (the body read the raw parameter).
`fun_easy` (4 self tail sites) timed out.

**Fix:** emit the chain in forward order (`for (j = 1; j < k; ++j)`), so
the j-th appended block is M_j as the pre-assigned ids assume.

## 3. Empty blocks forged ghost CFG edges (both tce transforms)

**File:** `src/50c_tco.hpp` → `src/minijz/opt/interprocedural/tailrec.hpp` (`tce_self`, `tce_merge`)

The `succ` reconstruction after both transforms read
`code[blocks[b].end - 1]` for *every* block — including empty (dead)
blocks, where that index is the **previous block's terminator**. Empty
blocks "inherited" successors and appeared as extra predecessors of
unrelated blocks, polluting every later analysis. `scctco_resucc()` in
50b already guarded this ("reading code[x-1] would steal the PREVIOUS
block's terminator"); 50c's two copies did not.

**Fix:** skip empty ranges in both loops, exactly like `scctco_resucc`.
*(Merged-tree status: ported into `tailrec.hpp`'s two succ-rebuild loops
in the 3-branch merge; no fuzzer repro was ever known to trigger it — it
was found by code inspection and is latent-corruption hardening. The
regression proof is the full battery staying green.)*

## 4. `regr_scc3_members` — tce_merge's header dispatch skipped the sel==2 test

**File:** `src/50c_tco.hpp` → `src/minijz/opt/interprocedural/tailrec.hpp` (`tce_merge()`)

For k ≥ 3 members the header's false arm must enter the dispatch chain at
its *first* block (id 2, the `sel == 2` test). The old formula
`(from_blk + 1 <= k - 2) ? from_blk + 2` sent k==3 straight to member 0
and k≥4 to block 3 — either way the `sel == 2` case never reached member
2, which was then deleted as unreachable. A merged 3-member countdown
(`m3a/m3b/m3c` from the fuzzer) returned the wrong member's base value
(8 instead of 9); `general.mjz`'s 4-member `fun` family was also wrong.

**Fix:** `ff = (k > 2) ? 2 : bmap_base[0]`.

## 5. `regr_notnot_compare` — `!!(a < b)` compiled to `a <= b`

**File:** `src/40_opt_core.hpp` → `src/minijz/opt/scalar/canon.hpp` (`opt_canon`, NOT-of-combine)

The comparison-negation table (`inv`) maps each comparison to its
involutive partner: the correct pairs are **`Lt ↔ Ge` and `Le ↔ Gt`**
(`NOT(a<b) = a>=b`, `NOT(a<=b) = a>b`, `NOT(a>b) = a<=b`,
`NOT(a>=b) = a<b`). The original table had the `Gt` and `Ge` rows'
results swapped (`Gt→Lt`, `Ge→Le`), so a doubly-negated compare
(`print !(!(input() < z))`) came out as `input() <= z`. Only observable
when the boundary value is live (e.g. the EOF read of `input()` returns
0: `0 <= 0` is 1 where `0 < 0` is 0), which is why fuzz seed 1211 was
the first to hit it.

**Fix (merged narrative):** the bugfix branch corrected only the `Ge`
row (`case B::Ge: return B::Lt;`) and left `Gt → Lt` — still wrong
(`!(a>b)` must be `a<=b` = `Le`, not `a<b` = `Lt`), so its table
remained broken at `Gt`. The two other branches fixed BOTH rows
independently (branch1 "fix04" f2d8dba; this line "bug4" e49d59c).
This tree carries the complete fix: the involutive table above, with
the bug documented in a comment at the site in
`src/minijz/opt/scalar/canon.hpp`. **Do not port the bugfix branch's
one-character variant.**

## 6. `s7406` — copyprop's resolve() walked into a self-loop and hung the compiler

**File:** `src/minijz/opt/scalar/copyprop.hpp` (`opt_copyprop`)

Unlike 1–5 this was not a miscompile but a **compile-time hang**, found by
`--fuzz-cfg=random` on seed 7406 (`-O1`, random tier, `dce` disabled) while
widening the fuzz window past 6200: the 50-seed chunk containing the seed
sat for >100 s without finishing before the fix, and the surrounding
100-seed window runs in ~15 s after it. Phi degeneration had materialized *crossed,
loop-carried* copies around a loop: `t473 = t496` (blk 12), `t521 = t473`
(blk 13), `t496 = t521` (blk 18). Each source is defined only on the
incoming edge — execution-sound, but **not dominance-sound**, breaking the
pass's "the copy's source dominates the copy" argument. Building the rename
map then recorded `ren[t496] = resolve(t521) = t496` — a SELF-LOOP — and
the next `resolve(t496)` (loop 2 rewrites the blk-12 copy's own source use)
spun in `while (ren[v] != NONE)` forever. With `dce` enabled the dead copies
never survived long enough to cross, which is why only random pass configs
ever saw it.

**Fix:** never record a self-loop entry — when `resolve(a) == dst` the copy
is part of a crossed pair and simply stays **unpropagated** (uses keep their
names; conservative and sound), plus a step cap in `resolve()` as
belt-and-braces. Seed 7406 now compiles in ~3 ms with all five runners
agreeing (`0 0 3 1 0 285`), i.e. the crossed copies were semantically
correct all along; only the propagation assumption was wrong.

## 7. `s4412` — iv_refine's Gt/Ge normalization swap leaked through the caller's bindings

**File:** `src/minijz/opt/analysis/intervals.hpp` (`iv_refine`, used by
`range_analyze`'s branch-edge refinement)

Found by `--fuzz-cfg=random` on seed 4412 (`-O2`, `canon`/`dce`/`unroll` and
five more passes disabled) while validating the five new portable
optimizations (SCCP, load forwarding/CSE, range-proved compare folding,
compare-domain algebraic identities, pure-call CSE). `iv_refine` normalizes
`Gt`/`Ge` facts to the `Lt`/`Le` direction by **swapping x and y through the
caller's references** — but never swapped them back. The caller then wrote
the (correctly refined, but now *mis-paired*) intervals back: `x` — bound
to the comparison's *left* operand — received the *right* operand's
interval and vice versa. On every `>`/`>=`-guarded branch edge the two
operand intervals were silently **exchanged**: `while (c6 > 0)` pinned
`c6` to `[0,0]` in the loop body while the constant `0` grew to `[1,2]`,
so `t53 = c6 - 1` was "proven" `[-1,-1]` — and the second iteration's
`t53 = 0` was then "impossible". A second symptom of the same leak:
endpoint crossings produced garbage intervals like `[2..0]` that
`iv_hull` smeared across the state instead of the empty interval.

**Why it never miscompiled before:** the poisoned intervals only fed
`nt` (can-this-trap) *metadata* — the guarded instruction still sat behind
an intact runtime branch, so a wrongly-proven-safe division never executed
with a trapping divisor. The new range-proved compare folding was the first
consumer that made **control flow** follow the intervals: it folded the
`c6 == 0` half of the `||` guard to constant `0`, rerouted both loop
iterations into the division block, and the program trapped where the
oracle printed `1 1`. A textbook latent-soundness-bug activation: the new
pass didn't cause the wrong intervals, it *exposed* them.

**Fix:** track the normalization swap and restore the caller's bindings
before returning (the refinement arithmetic itself was always correct in
the mirrored space), plus an empty-interval safety net when endpoint
refinements cross (`lo > hi` → bottom, not a garbage hull seed). Guarded
by `bugrepro/s4412.mjz` under `ctest -L regr`
(`bugrepro_s4412_rangecfg`: `-O2 --disable=canon,dce`, the minimal
discriminating config — with `canon` or `dce` on, they restructure the
loop before the fold, which is why the plain `-O3` gates never saw it).

## Verification

*Original (bugfix branch):*
* `--selftest` : 206/206 checks (5 new regression demos)
* fuzz sweeps, 5 seed ranges (1, 1000, 2000, 5000, 10000; 1400 programs
  total): **0 mismatches, 0 timeouts**
* all four constexpr proofs (`MJZ_CX_CHECK=1..4`) still compile
* `examples/general.mjz` now compiles and runs correctly at -O0..-O3
  (it previously miscompiled at -O3 and the un-fused paths trapped)

*This (merged) tree — full battery re-run after the 3-branch merge:*
* `--selftest`: 206/206 checks (the 5 `regr_*` demos ported verbatim)
* unit tests: 6095/6095 checks; constexpr proofs: 6/6
* differential fuzz: seeds 1–6200 clean (fixed rotation + random cfg)
* `examples/general.mjz` correct at -O0..-O3, both build modes
  (single-TU and multi-cpp)

*Post-merge continuation (2026-11-08, checkpoint 08):*
* bug 6 (§6) fixed: copyprop crossed-copy resolve() self-loop — the
  compiler hung on seed 7406; now ~3 ms, all five runners agree
* 7 headers made truly include-order independent (standalone compile
  proof: `scripts/header_order_test.sh`, wired into `ctest -L regr`)
* unit tests: 6100/6100 (divmagic fn-fuzz across all 5 runners +
  C-truncation expectations); `ctest -L regr`: 21 gates green
* differential fuzz widened: seeds 6201–8400 rotation + 6201–7500
  random configs clean after the fix

*Post-merge continuation (2026-11-08, checkpoint 10 — portable opt batch):*
* five architecture-independent passes added: SCCP (tier 0, incl.
  branch pruning), store→load forwarding + load CSE (`ldelim`, tier 1),
  range-proved compare folding (inside `trapelim`, shares the interval
  walk), compare-domain algebraic identities (`x&x`, `x|x`,
  `0-(0-x)`, `cmp&1`, `cmp%2`, `cmp>>k`, `cmp^1`, double negation),
  and pure-call CSE (`callcse`, tier 2)
* bug 7 (§7) fixed while validating the batch: the random-config fuzz
  (seed 4412) exposed the latent `iv_refine` Gt/Ge exchange; gate
  `bugrepro_s4412_rangecfg` added (22 `regr` gates now)
* validation after the fix: selftest 206/206; unit tests 6109/6109;
  constexpr proofs 6/6; differential fuzz seeds 1–1000 and 2001–3000
  clean; random-config fuzz seeds 4001–4600 clean (incl. 4412);
  `--fuzz=1000` completes in ~6 min (bug-6 hang fix holds)
* fuzz-window stats at seed 1–1000: 76 311 SCCP constants
  (19 261 branches pruned), 2932 range-cmps, 112 load fwd,
  616 cmp-facts, all five runners agreeing on every program

*Post-merge continuation (2026-11-09, checkpoint 11 — branchB folded
into branchA's split-TU tree):*
* git-level merge of branchB (five portable passes + bug 7) into
  branchA (9-real-TU restructure, clang-format tree, interactive
  debugger): 8 conflicts resolved by porting B's semantic deltas onto
  A's split-tree versions wholesale (README's prescribed strategy) —
  pass_cfg 23-name toggle table, iv_refine caller-binding restore,
  trapelim range-fold, fold compare-domain facts, driver wiring,
  stats/report lines, fuzz rotation case 10 (`seed % 11`)
* stale header-slice TUs (src/tu/scalar.cpp, interproc.cpp — deleted
  by A's restructure, modified by B) dropped; the new pass headers
  compile inside `tu/prog.cpp` via `opt/driver.hpp` as A's layout
  prescribes
* build hygiene restored after the merge: the three new headers and
  the ported regions clang-formatted (`check-format` green again)
* validation of the merged tree: selftest 206/206 (single-TU AND
  multi-cpp), unit tests 6109/6109, cx proofs 6/6, `ctest -L regr`
  25/25 (incl. the new s4412 gate + header_order over 67 headers),
  `ctest -L fuzz` 4/4, rotation window 9501–10300 clean,
  random-config window 8501–9100 clean (SCCP 39 452 consts,
  call-CSE firing)
