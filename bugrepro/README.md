# bugrepro/ — historical repro programs (bug ledger, examples/BUGFIXES.md)

| file | bug | what it exercises |
|------|-----|-------------------|
| `fun1s.mjz` | 1 | tce_self latch chain order (4 self tail sites) |
| `fun4.mjz` | 2 | tce_merge header dispatch, 4-member SCC ring |
| `m3ring.mjz` | 2 | tce_merge 3-member ring returning the wrong base value |
| `ring5.mjz` | 2 | 5-member mutual tail recursion ring |
| `s1211.mjz`, `s1211min.mjz` | 4 | `!!(a < b)` compiled to `a <= b` (fuzz seed 1211) |
| `s1070.mjz` | 3 | scctco sibling entry-block rotation (fuzz seed 1070) |
| `t1.mjz` | 3 | minimal scctco promote() shape |
| `s7406.mjz` + `s7406.in` | 6 | copyprop crossed-copy resolve() self-loop: compiler HANG at `-O1 --disable=dce` (fuzz seed 7406, random cfg) |
| `s4412.mjz` | 7 | iv_refine Gt/Ge swap leak: `while (c > 0)` pinned the counter to [0,0], range folding rewired the `||` guard around `99/c6` into a div-by-0 trap at `-O2 --disable=canon,dce` (fuzz seed 4412, random cfg) |

Bugs 1–4 predate the split tree (monolith-era, see the file-mapping table in
`examples/BUGFIXES.md`); bug 5 (empty-block ghost successors) is
latent-hardening from the merge; bug 6 was found widening the random-config
fuzz window past seed 6200; bug 7 (interval-analysis operand exchange on
Gt/Ge branch edges) is a latent soundness bug that the new range-proved
compare folding turned into an observable miscompile.

`ctest -L regr` runs every `*.mjz` here through `--check-all` at `-O3` and
`-O3 --disable=scctco`, plus dedicated entries for `s7406.mjz`'s hang config
and `s4412.mjz`'s range-fold config, and the hand-written stress battery +
header self-containment proof.
