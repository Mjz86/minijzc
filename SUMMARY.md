# SUMMARY — checkpoint 35: tce_merge decomposed (round eleven)

One commit this session, under the user's standing directives: the
per-round dedup pass (CONTINUE.md protocol), the OPT_CHECKLIST queue,
and the new one — BOTH .git directories ride in the zip from now on.

## 1. tce_merge decomposed (tailrec.hpp 745 -> 365 lines)

The queue's next monolith after unswitch: `tce_merge`, 357 lines,
got the round-ten treatment — a 35-line driver stating the phase
list plus `tce_merge_impl.hpp`, the support layer:

* **The shared tce vocabulary** moved with the machinery: the
  member/site caps, `is_tail_call`, and the budget-init /
  site-check / trap emitters (tce_self includes the impl header and
  keeps speaking them).
* **`TmCtx`**, the per-merge context: k/M/ks, Fm under
  construction, the sites, the member vmaps, the block layout
  (bmap_base, the latch chain Mb, T, latch), and the pre-created
  vregs (selHv/aHv/bHv, siteSEL/siteBL/siteEQ, Mp/Mbv).
* **The six phases, in exactly the monolith's order** (the vreg
  allocation order is observable — the audit table keys on vreg
  ids): `tm_collect_sites` (the gates + the intra-SCC tail-site
  census; a non-tail intra edge bails — mixed SCCs stay recursive),
  `tm_alloc` (Fm's shell, the block layout, the pre-creation in the
  monolith's nesting: selHv/aHv/bHv before the member vmaps,
  siteBL/siteEQ site-major, Mp/Mbv latch-major), `tm_emit_header`
  (P0, the H phis, the sel dispatch chain with bug #2's false-arm
  fix), `tm_emit_members` (the bodies through the vmap via
  `remap_vreg_fields`; a member's tail Ret becomes
  [sel'-const; arg pads; budget check; cbr T : chain-next]),
  `tm_emit_latches` (the DESCENDING-j latch chain — ids from the
  layout pass, positions from emission, decoupled by design; the
  trap; Fm's commit), `tm_rewrite_callers` (the sel-const + pad
  splice before every external call).

**The UsCtx lesson applied proactively**: TmCtx's id fields default
to NONE exactly like the monolith's locals did (`bInitV = NONE`,
the NONE-filled vectors) — documented in the struct's contract.
**No landmine fired**: parity held on the first try, both fuzz
windows included. The two preserved asymmetries on the impl
header's landmine ledger: siteSEL is NOT pre-created (allocated at
the member's Ret emission; the latch phis read it after every
member is emitted), and site args are PADDED in place during member
emission so the latch chain can index [sel', a_0..a_{M-1}]
uniformly.

## 2. The round's dedup pass — the scan is at its floor

Window 8/10/12 over src/ (the tool: /home/z/my-project/scripts/
dup_scan.py, NOT in the repo): every remaining hit is an on-record
rejection or a shared-helper call site. Two hits triaged and
rejected this round on the ckpt-33 seed-decl-prologue precedent
(the seed CALL is already the shared primitive; the decl/guard
lines around it are the call cost):

* argpromo/fnspec's 8-line driver prologue (the arity gate +
  `ipa_collect_call_sites` call — the shared helper's call site,
  both passes).
* divmagic/ifconv's nb/nv guard + kc/kv seeding prologue
  (`seed_consts`' out-param call cost; half the sites seed into
  context members a pair-returning helper cannot serve).

The moved tce_merge code performs no hand-rolled enumerations:
`count_operand_uses`, `remap_vreg_fields`, `is_tail_call`, and the
tce_emit_* emitters are all it uses. Post-split scan output: byte
for byte identical to pre-split — no new windows.

## 3. Verification

Parity 146/146 byte-identical, fuzz windows 1-300 + 20000-20300
identical modulo timing, selftest 208/208, units 6482/6482, header
order 118/118 + umbrella (the new impl header), includes 538/538,
audit table regenerated identically, kernel fuzz 30000 all 60
components 0 fails, e2e fuzz 800 clean, stress ALL PASS, bugrepros
10/10.

The String clone: pristine `main`, `origin/main` at ce6ea95; the
local branch `graph-opt-kernels` (tip 2cb6c7d) unchanged. The
queue after this (OPT_CHECKLIST.md): argpromo (~415), unswitch_shape
(433) — the per-round dedup protocol and both standing requirements
in force. The zip protocol change: BOTH .git dirs (the main repo's
and mjz_lib's) now ride in the archive, never excluded.
