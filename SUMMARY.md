# SUMMARY — checkpoint 31: the dedup campaign (the code-bloat cleanup)

One strand, one session, three commits: the user's finding — "many
places have the exact same algorithm implemented inline as the
algorithm that's in other files" — was made mechanical (a
normalized 8-line-window scan across src/), confirmed (twin
clusters: cse/gvn 39 windows, argpromo/fnspec 21, the emitters 26,
the kc/kv const-table seeding in TWELVE files, the bump-lambda use
counter in six), and eliminated with byte-identical parity at every
step. The mjz_lib branch (graph-opt-kernels, six commits) is
unchanged this round — the twins were COMPILER-side glue around
kernels that already existed; the dedup made the glue shared too.

## 1. instr.hpp: the IR knowledge hub

The two kc/kv seeding disciplines, the clone permutation, and the
use counter are IR knowledge, not pass policy — they moved next to
w_bin (the ckpt-26 precedent for shared instr knowledge):

* `seed_consts` — ANY Const def keys the vreg, last def wins (the
  SSA-context rule: canon, dse, ifconv, ldelim + the vn wrappers).
* `seed_single_def_consts` — only vregs with EXACTLY ONE def (the
  post-destruction phi-slot freeze rule: a slot whose edge-1 def is
  `c3 = 5` and edge-2 def is `c3 = t` must not be inlined as 5).
  Was hand-rolled in emit_plan, stack_alloc, divmagic, reassoc,
  lcf, mem2reg.
* `remap_vreg_fields` + `remap_stk_arrays` — the five-field
  permutation rewrite of the cloning passes (argpromo, fnspec,
  deadargs). Phi's stale base-var tag and Call's fn index are
  intentionally not remapped (the deadargs policy, documented once).
* `bump_uses_of` + `count_operand_uses` — the textual-use counter
  (a/b2/c2/args, NONE-guarded) that emit_x64, tailrec (twice), lcf,
  canon, reassoc and the fusion table all hand-rolled. sccp's
  site-INDEX variant is deliberately not this and stays local.

## 2. emit_plan.hpp: the shared backend policy

* `make_fuse_table` — the branch-fusion gate (raw use counts, last
  def index, def counts, the single-use-single-def compare scan).
  make_emit_plan's phase 5 AND astrun's assign_slots now call the
  SAME function: the hand-kept mirror whose drift once let another
  vreg clobber a fused operand's slot (fuzz seed 33) is structurally
  impossible. The allocation profile is unchanged (three nv vectors
  + one code vector) — the constexpr pipeline's 4 GiB budget is
  undisturbed.
* `mark_referenced_blocks` — the label-reference mask (fallthrough
  elision + the constant-condition fold), identical policy in the C
  and asm emitters, now one function.
* `append_source_range_banner` — the lo/hi source-line banner; the
  C/asm spellings differ only in the comment syntax, passed as the
  (open, close) pair.

## 3. The new support headers

* `opt/scalar/vn_sites.hpp` — cse/gvn's verbatim glue: vn_u (the
  NONE->none_v bridge), vn_fill_bin_site (the pure-Bin
  classification: dst gate, nt-gated div/mod, the commutative
  family, the operand rows), vn_rewrite_completed_map (the second
  pass: resolve chains over every operand field, Phi args included —
  sound because ren[v]=w implies w's def dominates every edge-use
  of v). The passes differ only in WHICH entry kinds they admit
  (cse: Bins; gvn: Bins + consts + complete phis).
* `opt/interprocedural/ipa_sites.hpp` — the call-site census
  (f, i, g over fns[0..n0)), the Const-behind-a-vreg lookup, and
  the arity gate (argpromo accepts >=1 site, fnspec >=2 — the gate
  is the caller's, the scan is shared).

## 4. The loop family + the small twins

* loop_shape.hpp: `innermost_headers_by_nest` (was THREE copies:
  loop_scan's inline block, lcf_header_list, unr_header_list — the
  pair-swap order is behavior), `loop_single_exit_target` (the
  only-exit-departs-the-header gate), `loop_zero_trip_exit` (the
  `br X` terminator rewrite). The unroll decomposition of ckpt 25
  had kept these local to stay byte-identical; the dedup moved them
  with the same guarantee.
* lcf_eval1 moved to lcf_sim.hpp (the simulator's identical eval
  lambda deleted); the builder's And/Or short-circuit diamonds
  merged (only the first cbr's polarity differs; block/vreg
  allocation order preserved exactly); emit_c's two in-file xorbase
  copies hoisted to one definition.

## 5. What was NOT shared (the reasons on record, CONTINUE.md)

assign_slots' liveness/interference/coloring: the kernel
interference adds UNCONDITIONAL pairwise entry-def edges (seed
804's fix) which would change the degree sort and thus slot ids;
the kernel liveness would need per-instr site-table vectors inside
the CONSTEXPR pipeline (the 4 GiB budget). The fuse gate — the
sound half — IS shared. divmagic/reassoc's rebuild tails are
look-alikes around different inner logic (over-abstraction).
bc_defs/instr is the deliberate bytecode/IR mirror. sccp's uses[]
pushes site indices, not counts.

## 6. The standing requirements (the user's directive)

CONTINUE.md now carries two standing rules: (1) the code-bloat
cleanup / deduplication pass runs BEFORE every finalized commit
(diff-scan the touched files; reuse first; proved-identical
semantics or it stays local, difference documented); (2) code
quality and fuzz testing for EVERY component (contract comment,
fuzzer entry or differential window, full battery).

## 7. Verification

Each of the three commits: 146 parity runs byte-identical vs the
pre-campaign baseline; differential fuzz identical modulo timing
(wave 1: 1-600 + 20000-20500; wave 2: 1-500 + 20000-20400; wave 3:
1-300); kernel fuzz clean; selftest 208/208, units 6476/6476,
header order 116/116 + umbrella, includes 515/515, audit table
identical, stress ALL PASS. Net ~-250 code lines; ~350 lines of
duplication converted to shared single-definition code; the dup
scan's biggest clusters are gone (39->6, 21->0, 26->6, 12->2
definitions, 6->1).

The queue after this (OPT_CHECKLIST.md): unswitch (~389), argpromo
(~387, twins already gone), tailrec's tce_merge (~385),
unswitch_shape (429) — rotate_impl.hpp is the template, and the
standing dedup requirement now applies to every decomposition.
