# Checkpoint 26 — fold decomposed (round four) + the persisted checklist/parity tooling

branchA @ (one commit on top of checkpoint 25's unroll commit).
The CONTINUE backlog's item 1 consumed its next named target:
`opt/scalar/fold.hpp`'s opt_constprop — at 450 lines the biggest
function left in the tree (the whole 497-line file was one
function plus banners) — got the cdfold/lcf/unroll treatment,
under the byte-identical discipline. Plus two recovery asks that
landed as tooling: the decomposition queue is now a committed file
(OPT_CHECKLIST.md) after the checklist was lost between sessions,
and the parity differential is a committed script
(scripts/parity_diff.sh) instead of ad-hoc bash.

## What landed

### fold: the 450-line monolith, decomposed (`fold.hpp` 497 -> 67 + `fold_impl.hpp` 629)

The single opt_constprop became an 11-line driver plus a support
layer of 28 small constexpr functions + the FoldCtx struct:

- **FoldCtx**: the def-index map (defi), the per-round known-const
  tables (known/kval), nv0, and the fn/os references — the state
  the original held in pass-wide locals, now grouped.
- **the def-index**: fold_def_index — vreg -> its def's instr
  index, the LAST def winning (SSA: normally the only one; the
  guard defensive, the original's). Built ONCE at pass entry: this
  pass rewrites instrs in place, never moves them, so the indices
  stay valid all 12 rounds.
- **the fixpoint mechanics**: fold_reset_tables (every live Const
  re-enters the tables per round — a Const MADE by round k is
  visible to round k+1; consts made DURING a round wait for the
  rebuild: that is the observable iteration structure), fold_round
  (rebuild + the full block/instr scan, `|=` never
  short-circuiting), fold_ctx (the factory).
- **the compare-domain machinery**: fold_cmp_def (is a vreg
  EXACTLY one compare result? re-checks D.k == K::Bin at USE time,
  so a def folded to Const this round no longer qualifies — the
  original's behavior), fold_inv_cmp (predicate inversion:
  Lt<->Ge, Le<->Gt, Eq<->Ne), the fact trio fold_cmp_keep
  ((cmp)&1, (cmp)%2 keep the compare), fold_cmp_zero ((cmp)&~1,
  (cmp)>>k are exactly 0), fold_cmp_not ((cmp)^1 = the negated
  compare), gated by fold_bin_cmpfacts (b-side known, a-side a
  compare; mutually exclusive guards, so keep->zero->not preserves
  the original five-way order exactly).
- **the two-def identity**: fold_bin_double_neg (0-(0-x) = x, with
  the I.b2 != I.a guard against self-matching).
- **the nine algebraic families**: add/sub, mul, div/mod, and, or,
  xor, shift, mulhi, same-vreg — annihilators before identities,
  ka before kb, each sub-case's field writes and counters exactly
  the original's. The same-vreg table deliberately does NOT fold
  Add/Mul/Div/Mod (x+x changes the value; x/x can TRAP).
- **the one dedup**: the former 20-case inline wrapped-arithmetic
  switch is now instr.hpp's SHARED w_bin — provably the same table
  (same w_* per op, same {cmp,false} results; w_bin's unreachable
  trailing {0,true} vs the original's value-initialized Rv{} is
  dead in both: the B enum has exactly the 18 covered values).
  cdfold's dedup precedent, documented in the contract header.
- **the per-kind handlers**: fold_copy (Copy of a known const ->
  Const, os.folds), fold_phi (same vreg -> Copy; same const ->
  Const), fold_depth (main's frame count is provably 1), fold_cbr
  (identical arms -> Br; known cond -> Br toward the taken arm;
  fn.succ rewritten in the terminator's own field order).
- **the dispatch**: fold_instr — the original's switch, ka/kb
  looked up BEFORE the switch (every kind sees the same operand
  facts), and fold_bin — the cascade, first match wins (the
  original's `break`s), the phase list reading top to bottom:
  cmpfacts -> double-neg -> add/sub -> mul -> div/mod -> and ->
  or -> xor -> shifts -> mulhi -> same-vreg -> full-const.

The byte-identical contract is fold_impl.hpp's header block; the
one landmine worth naming: the 12-round structure is observable
(os.iters — the pass's return value drives the pipeline's
fixpoint), so the round structure — rebuild, scan, quiet-round
exit, the cap — is preserved to the character.

### The recovery tooling (lost-checklist incident)

- **OPT_CHECKLIST.md** (repo root): the standing decomposition
  queue — done list (cdfold/lcf/unroll/fold, with contracts),
  the priority-ordered remainder (unswitch_bif next, then taildup,
  mem2reg, rotate, scctco_member, unswitch, argpromo, tce_merge,
  ...), the >700-line files list, the standing traps (the
  `| tail` pipeline landmine, the stdin-aware differential, the
  preserved asymmetries, the no-loop-shape-sharing rule), and the
  protocol. Survives session loss by living in the repo.
- **scripts/parity_diff.sh**: the differential as a committed
  gate — every .mjz in the corpus x three flag variants
  (default/-O3/--check-all), stdin-aware, + three scripted
  debugger tours + --demo + the -O3 pass-statistics report; 146
  runs this checkpoint, all byte-identical.

## Verification (the strongest available differential)

- parity_diff.sh: **146 runs, 0 mismatches** (stdout + stderr +
  exit codes) vs the pre-refactor binary
- register-audit table regenerated **byte-identically**
  (710 vregs / 137 spill slots / 424 traffic)
- differential fuzz old-vs-new, 16131-16280 and 14231-14430:
  **identical modulo timing lines, zero mismatches**
- selftest **207/207**, ctest **44/44**, units **6328/6328**
- check-format clean; header order **100/100** + umbrella
  (fold_impl.hpp is the 100th); includes **422/422**

## Measured shape

- fold.hpp: 497 -> 67 lines (driver: 11)
- fold_impl.hpp: 629 lines, 28 constexpr functions + FoldCtx,
  largest ~28 lines (house norm)
- the tree's opt/ biggest functions are now all under ~440 lines:
  next targets unswitch_bif (~432), taildup (~419), mem2reg (~415)

## Known limits

- unchanged from checkpoint 25: no MSVC in the sandbox (the
  `vs-msvc` run remains the one unverified step); fuzz floors are
  seed-anchored (a new bait flavor needs a NEW floor constant).
