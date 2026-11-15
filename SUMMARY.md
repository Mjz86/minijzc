# The generic-ssa branch — the uintlen_t SSA kernel, now with the
# pure placement layer (two commits on the ckpt-28 main)

This zip: branch `generic-ssa`, TWO commits past checkpoint 28.

**Commit 1 — the kernel.** `src/minijz/ir/ssa_generic.hpp`
(`mjz::ssa_ns`): SSA construction as pure index math — uintlen_t
blocks/vars/sites/events, version_t templates, CSR forests, no
Instr/Fn/phi-instruction anywhere. The three hard parts, generic:
multi-branch phis as N-ARY RECORDS (arity = pred count, any count —
unit 16d proves a 3-pred join this IR cannot spell); renaming as
Cytron's dominator-tree walk driven by event streams, output pure
tables (reaching_def / def_ordinal / visit_order / phi records with
per-edge-slot entries); instruction removal as a NON-EVENT (emit no
events) plus the reflow plan (the flat-code rebuild index math three
passes used to hand-roll). an.hpp's ssa_construct, mem2reg's
promotion and pred_norm's merge chains became thin adapters,
byte-identical.

**Commit 2 — the contract change (this delivery).** The placement
assumptions moved OUT of the algorithm and INTO the wrapper, per the
"algorithms must not depend on how the compiler works" rule:

  * NEW `mjz_lib/mjz_lib/graph/phis.hpp` (graph_ns, in the vendored
    String clone — zip-only, not in this repo's git): the PURE
    Cytron relation. `calculate_phi_placements(df, defs)` = the
    var-major map, row v = IDF(defs(v)), with NO client assumptions
    — every def count places (single-def vars get their DF(def)
    phis; the wrapper drops them as IR policy), pred rows are not
    even an input (an entry sitting in its own cycle places at a
    ONE-PRED block — no <2-pred assumption anywhere),
    `calculate_inv_phi_placements` = the block-major transpose.
  * NEW `make_basic_inv_forest_universe(rows, node_count)`
    (csr.hpp): the transpose for BIPARTITE relations — the square
    make_basic_inv_forest sizes by the row count and would index
    out of bounds on var-space-into-block-space rows; the universe
    variant takes the target space explicitly and fills ascending
    (the emission order vreg minting depends on).
  * `ssa_ns::calculate_phi_placements` is now THE COMPILER'S POLICY
    WRAPPER: same signature, same output, same parity — computed as
    the pure placement + the two IR filters (the <2-defs skip, the
    >=2-pred-slot rule with duplicates counting). an.hpp and
    mem2reg unchanged.

Unit 16i pins the layer gap both directions (single-def loop-body
var places in the pure layer, is dropped by the wrapper; the
entry-cycle ARITY-1 phi record lowers through rename_ssa untouched;
the non-square universe transpose both ways). Verified: 146-run
stdin-aware parity byte-identical, differential fuzz 16501-16650 +
16651-16800 identical modulo timing, audit table regenerated
identically, full battery green (selftest 207, ctest 44, units 6385,
header order 102/102, includes 431/431, check-format clean on
clang-format 16). Upstream note: phis.hpp + the csr.hpp universe
transpose + the algo.hpp include belong in the String repo.

---

# Checkpoint 28 — taildup decomposed (round six) + fuzz_parity.sh

branchA @ (one commit on top of checkpoint 27's unswitch_bif
commit). The OPT_CHECKLIST queue's next named target, consumed:
`opt/scalar/taildup.hpp`'s opt_taildup — at 419 lines the biggest
function left in the opt/ tree — got the cdfold/lcf/unroll/fold/
unswitch_bif treatment, under the byte-identical discipline. Plus
one tooling continuation: the differential-fuzz half of the
protocol is now a committed script (scripts/fuzz_parity.sh), the
ckpt-26 parity_diff.sh precedent applied to the fuzz windows.

## What landed

### taildup: the 419-line monolith, decomposed (`taildup.hpp` 483 -> 94 + `taildup_impl.hpp` 719)

The tail-duplication pass (checkpoint 20's feature — the small
2-pred merge T duplicated into both dedicated preds, T's phis
MOVING to routing phis at the continuations and born phis at
re-merges) became a 24-line driver plus a support layer of 28
small constexpr functions + the TdCtx struct:

- **TdCtx**: fn/A/T/P0/P1/U0/U1 (the rewrite target, the analysis,
  the candidate) + nb/nv (the pass-entry universe — the sub/rw
  guards read nv so the commit's own fresh vregs are invisible to
  the substitution scans) + tterm (saved pre-edit) + the scan
  tables (phi_dst/a/b, defv, local0/1, route, bsites, isdef,
  need0/1, bvals, bad) + the commit state (uphi0/1, bphi).
- **the gates**: td_preds_ok + td_dedicated_br (preds(T) == {P0,
  P1}, both ending in a dedicated `br T', T not a loop header —
  that is rotation's), td_content_ok + td_collect_phi +
  td_phi_arg (the phis keyed to both preds, the body <=
  TAILDUP_BODY_MAX of Const/Copy/Bin/Print, the terminator Br/Cbr/
  Exit/Ret), td_continuations (Br: one target; Cbr: two, a
  degenerate collapses; Exit/Ret: none), td_succs_ok (each
  continuation T-private and phi-free at its top).
- **the routing fixpoint**: td_route_seed (dominance: exactly one
  continuation dominating b -> its phi; T/P0/P1 dead), then
  td_route_step + td_born_site under td_route_fixpoint — the mini
  SSA construction: single-pred chains pass their supplier down,
  same-supplier merges read it, different-supplier merges get a
  fresh phi BORN there (the loop-latch feedback edge is exactly
  this shape — what makes in-loop merges duplicable). Round cap
  nb+1, block order, in-place in-round visibility: all the
  original's.
- **the outside-use scan**: td_outside_uses_ok + td_check_use +
  td_mark_need — every outside use of a T-defined value must be
  routable (plain use: its block's route; phi arg: the KEYED
  PRED's route), and each use marks its need down the supplier
  chain (need0/need1 for the uphis, bvals for the born phis on the
  route, recursively). A stray read bails the candidate BEFORE the
  commit's first edit — validate, then edit.
- **the commit tail**: td_alloc_locals (fresh per-copy vregs,
  local0 then local1 per def), td_emit_uphis (the continuations'
  routing phis, one per needed defv, U0 then U1), td_emit_bphis
  (site order = creation order, so a site's args reference only
  EARLIER sites' phis and the uphis), td_copy_tail + td_sub_val +
  td_install_tail + td_emit_copies (the two substituted copies —
  phis skipped, defs and operands substituted, P's `br T' replaced
  by T's substituted terminator, the bounds re-read fresh per
  side), td_rewire_uses + td_rewire_use (every outside use -> its
  route's phi), td_commit (os.taildups first, recompute_preds
  last).
- **the two dedups** (both provably identical, in the impl
  contract): td_prepend_phis — the uphi and born-phi prepends
  shared one exact insert-and-shift statement sequence (the copy
  install's shift stays separate: it REPLACES a terminator,
  signed compares, end-only shift for the target block); td_arg_of
  — the (r,di) -> phi-dst lookup the born phis' args and the
  rewire's assignment both computed.
- **the landmine honored**: the allocation ORDER is observable (the
  register-audit table keys on vreg ids) — body locals, then
  uphis U0-then-U1 for the needed defs only, then born phis
  site-by-site in bvals order; every order preserved to the
  character and documented.

9 of the 28 functions are at most 10 lines; the rest are integral
steps (the route step's four cases, the content walk's
load-bearing if-chain, the mark_need stack walk, the phi emitters,
the replace-and-shift install).

### the tooling: scripts/fuzz_parity.sh (the differential fuzz, committed)

The ckpt-26 move (parity as a committed script) extended to the
fuzz half of the protocol: scripts/fuzz_parity.sh OLD NEW FROM TO
runs both binaries over a seed window and compares the outputs
modulo the timing lines (the [fuzz k/n] progress lines and the
wall summary). Both committed windows verified through it at this
checkpoint.

## The verification (all green)

- parity: 146 runs byte-identical vs the pre-refactor binary
  (47 programs x 3 variants, stdin-aware, + 3 debugger tours +
  --demo + the -O3 stats report) via scripts/parity_diff.sh
- audit: examples/audit/RESULTS.md regenerated — git clean
  (identical table: 710 vregs / 137 spills)
- differential fuzz: 16131-16280 and 14231-14430, old vs new —
  identical modulo timing lines, zero mismatches, via
  scripts/fuzz_parity.sh (the stats line — including the taildups
  counter — compared verbatim)
- selftest 207/207; ctest 44/44; units 6328/6328; check-format
  clean (clang-format 16.0.6 — the tree's formatting version; see
  OPT_CHECKLIST.md's protocol note); header order 102/102 +
  umbrella; includes 431/431

## Layout deltas

- new: src/minijz/opt/scalar/taildup_impl.hpp (719 lines, the
  support layer + the full theory banner + the byte-identical
  contract)
- rewritten: src/minijz/opt/scalar/taildup.hpp (483 -> 94: the
  theory intro + the 24-line driver)
- new: scripts/fuzz_parity.sh (the differential-fuzz gate)
- OPT_CHECKLIST.md: taildup moved to Done; the queue's next up is
  mem2reg (~415); the protocol names fuzz_parity.sh + the
  clang-format-16 note
- README.md: this checkpoint's ledger entry (section 14)
