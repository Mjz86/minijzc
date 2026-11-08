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

All five bugs were found by running the differential fuzzer over seed
ranges it had never seen before (`--fuzz=N --fuzz-from=S`), plus
`examples/general.mjz` (which previously miscompiled at -O3). Each fix has
a `regr_*` demo in `src/minijz/apps/demo.hpp`, so `--selftest` fails if
any of them regresses. The count went from 186 to 206 checks.

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
