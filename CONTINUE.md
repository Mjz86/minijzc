# CONTINUE — next steps after checkpoint 34 (unswitch decomposed)

Read SUMMARY.md (same zip root) first. The main repo is 24 commits
ahead of origin/main; the vendored String clone sits PRISTINE on
upstream `main` (nothing under `mjz_lib/` is modified — the user's
directive), and every mjz_lib addition this repo carries lives in
`generic/` written as-if-in-mjz_lib (merge route: `generic/README.md`;
the local branch `graph-opt-kernels`, tip 2cb6c7d, keeps the same
content as commits for diff review). This file is the resume manual.

## THE PER-ROUND PROTOCOL (the user's directive, ckpt 33)

EVERY round that updates the codebase runs the dedup pass and records
it here — not just before commits:
1. **Scan**: `python3 /home/z/my-project/scripts/dup_scan.py 8` (and
   10/12 for the big twins) over src/. Copy the tool in if the sandbox
   recycled; it is 60 lines of python.
2. **Triage every hit**: read BOTH sites in full. A twin is removable
   ONLY if provably identical on every reachable input (guard
   equivalence incl. NONE handling, allocation order, filter
   strictness, traversal order when observable). Look-alikes around
   different inner logic are REJECTED with the reason recorded below.
3. **Reuse first**: before writing new code, check instr.hpp (the IR
   knowledge hub: enumerations, counters, seeders, remap) and the
   mjz_lib kernel inventory for what the new code needs.
4. **Record**: the round's accepted + rejected dedups land in this
   file's ledger (the section below) with the verification numbers.

## What checkpoint 34 shipped: unswitch decomposed (round ten)

opt_unswitch 381 lines -> 35-line driver + unswitch_impl.hpp (UsCtx
+ us_shape_ok/us_exits_ok/us_false_region/us_outside_uses_ok + the
commit tail us_guard_blocks/us_emit_guards/us_merge_phis/us_rewire).
THE LESSON of the round (record for every future decomposition): the
first draft zero-initialized UsCtx's guard-block ids; the monolith
initialized them to NONE per header. The && compound shape never
allocates t1, so `if (U.t1 != NONE)` fell through and emitted the
||-only `t1: br H' into BLOCK 0 — seed 20029's -O3 build looped
forever. The 146-run parity did NOT catch it (the audit programs
never hit compound-&&); the committed differential fuzz window did, on
the first try. Two rules: (1) a context struct replacing
function locals must copy the locals' INITIALIZERS exactly
(NONE-defaulted ids, not value-defaulted); (2) the wide fuzz windows
are part of every decomposition's gate, not optional.

## What checkpoint 33 shipped: the dedup campaign, round two

The window-8/10/12 scan after ckpt 31/32 found the stragglers; ten
waves removed the proved-identical twins, byte-identical at every
step:

1. **`for_each_operand` + `rewrite_operands`** (ir/instr.hpp): the
   fixed a/b2/c2/args operand traversal, once. Eleven hand-rolled
   mark/chk/one/rw enumerations collapsed (rotate_impl, unswitch x2,
   lcf_impl, lcf_rt, lcf_sim, regalloc, astrun, vn_sites' rewrite,
   bump_uses_of's own body). reassoc's bump copy (a ckpt-31 miss) and
   jthread's use census became plain `bump_uses_of` calls.
2. **`vn_run`** (opt/scalar/vn_sites.hpp): the cse/gvn driver. Both
   passes were verbatim twins outside the site-fill policy and the
   stats counter; cse is a one-line policy now, gvn states its three
   entry kinds.
3. **remap_vreg_fields adoption**: callgraph + tailrec's inline
   five-field permutation copies (their mapv maps NONE->NONE, so the
   shared guards are vacuous — proved).
4. **`any_fn_in_closure`** (opt/interprocedural/constcall.hpp): the
   call-graph closure worklist; closure_has_depth/fn_pure_closure
   are predicate one-liners.
5. **In-file twins**: vm_step's eleven trap-stop copies -> trap_out;
   astrun's And/Or mirror arms -> one arm + polarity; emit_c's
   Load/Store bounds emission -> emit_arr_bounds; diag's line scan
   -> line_of; parser's if/while '( expr )' -> paren_expr.

Verification: parity 146 byte-identical, fuzz 1-300 + 20000-20300
identical modulo timing, selftest 208, units 6482, header order 116,
includes 529, stress ALL PASS. Net -60 lines.

## What checkpoint 32 shipped: the generic localization + the params

1. **generic/ is the single carrier.** All 32 kernel headers moved
   VERBATIM (md5-verified) into `generic/mjz_lib/{opt,graph}/`; the
   clone reverted to pristine upstream `main`; the four
   upstream-file changes ride as patches in `generic/merge-patches/`
   (alllib hook, algo includes, the implace_string `template`
   conformance fix, the OPTIONAL csr family fold — never both that
   and graph/inverse.hpp). inverse.hpp itself is the split carrier
   for the thirteen net-new inverse-family members (the three
   historical stay in upstream csr.hpp; shadowing would be an
   order-dependent ODR trap). graph_lib.hpp imports both families
   (single import point); CMake + the header-order gate carry the
   clone's graph module dir for the upstream-form quoted includes.
2. **The param-driven survey** (the user's example: "a dominator
   interval param"). The opt family was ALREADY param-driven (vn/
   mem/ssa take dominator_tree; inv takes dom_begin/dom_end — licm
   passes An's A.ib/A.ie; layout takes cfg's reachable mask; phis
   takes DFs), and An is 100% kernel-built. The one genuine inline
   site — rmas's reducible FAS computing idoms internally — gained
   the `..._given_dominators` sibling; the 3-arg form builds and
   forwards; fz_rmasp self-differentials the two forms every seed
   (35408 checks, 0 fails).
3. **Rejected on record**: rmas's internal make_basic_forest(net.
   succs) conversions (flow input format over internal data), cut's
   symmetrization forest, matching's bipartite transpose (the
   Hopcroft-Karp working set), min-equivalent's internal SCC
   condensation (no second consumer).

## What checkpoint 31 shipped: the code-bloat cleanup

The user's finding: "many places have the exact same algorithm
implemented inline as the algorithm that's in other files." A
mechanical scan (normalized 8-line windows across src/) confirmed
it; three commits removed the twins with byte-identical parity at
every step (146 runs, fuzz windows, full battery):

1. **instr.hpp became the IR knowledge hub**: seed_consts +
   seed_single_def_consts (TWELVE hand-rolled kc/kv seedings
   collapsed), remap_vreg_fields + remap_stk_arrays (the clone
   permutation rewrite, was in argpromo/fnspec/deadargs),
   bump_uses_of + count_operand_uses (the bump lambda, was in six
   files).
2. **emit_plan.hpp**: make_fuse_table — the branch-fusion gate now
   called by BOTH make_emit_plan and astrun's assign_slots (the
   mirror that fuzz seed 33 proved dangerous can never drift again);
   mark_referenced_blocks + append_source_range_banner (the two text
   backends' label mask + source banner).
3. **vn_sites.hpp** (opt/scalar/): cse/gvn's verbatim glue — vn_u,
   vn_fill_bin_site, vn_rewrite_completed_map. The passes differ
   only in which entry kinds they admit.
4. **ipa_sites.hpp** (opt/interprocedural/): the call-site census,
   the Const-behind-a-vreg lookup, the arity gate (argpromo/fnspec).
5. **loop_shape.hpp**: innermost_headers_by_nest,
   loop_single_exit_target, loop_zero_trip_exit — the lcf/unroll
   twins (and loop_scan's header list, a third copy).
6. lcf_eval1 moved to lcf_sim.hpp (both users include it);
   emit_c's two in-file xorbase copies hoisted to one; the builder's
   And/Or diamonds merged (only the first cbr's polarity differs).

Net: ~-250 code lines, ~350 lines of duplication converted to
shared, single-definition code. The scan tool lives at
/home/z/my-project/scripts/dup_scan.py (NOT in the repo — copy it in
if the sandbox recycles; it is 60 lines of python, window size 8).

## STANDING REQUIREMENTS (added by the user's directive, ckpt 31;
## sharpened into the per-round protocol above, ckpt 33)

1. **The code-bloat cleanup / deduplication pass runs BEFORE every
   finalized commit.** Before committing ANY change: diff-scan the
   touched files for inline twins (the dup_scan.py pattern) and for
   hand-rolled copies of mjz_lib kernels / instr.hpp helpers; reuse
   FIRST, then write new code. Byte-identical parity is the gate —
   a shared function must be provably identical to every twin it
   replaces (NONE-vs-<nv guard equivalence, allocation order,
   filter strictness), or it stays local with the difference
   documented.
2. **Code quality and fuzz testing for EVERY component.** No new
   kernel, pass, or helper ships without: (a) the THE MODEL / THE
   FIXPOINT / THE ROWS-style contract comment, (b) its own fuzzer
   entry when it is a kernel (KZ_FNS + KZ_NAMES + fuzz_base counter
   arrays — all three, the static_assert catches drift), or a
   differential fuzz window when it is a pass, (c) the full battery
   green (selftest, units, header order, include portability,
   parity, stress, audit regen).

## Rejected dedups (the reasons on record)

- **intervals.hpp's raw use census** (ckpt 33): it deliberately
  EXCLUDES Phi edge args — is_cmp_def counts scalar comparison uses
  only, so bump_uses_of (which counts args) would change the
  single-use gate. Stays local.
- **emit_c's cmp_txt vs instr.hpp's bname** (ckpt 33): a 6-case
  emission subset whose `default: "!="` is load-bearing for the
  negation path vs the full diagnostic table. Different contracts;
  coupling them would silently change C output the day one table
  gains a spelling. Stays local.
- **deadargs/icf's Call-walk openings** (ckpt 33): 5-line traversal
  skeletons around different inner logic — the divmagic/reassoc
  precedent (a callback walker would be over-abstraction).
- **the seed_consts decl prologue** (ckpt 33): the seed CALL is
  already the shared primitive; the two decl lines are the out-param
  call cost, and half the sites seed into context members
  (S.kc/X.kc/P.kc) a pair-returning helper cannot serve.
- assign_slots' liveness/interference/coloring stay local in
  astrun.hpp: the kernel interference adds UNCONDITIONAL pairwise
  entry-def edges (seed 804's fix) which would change the degree
  sort and thus the slot ids; and the site tables would allocate
  per-instr vectors inside the CONSTEXPR pipeline (the 4 GiB
  evaluation budget). The fuse gate (make_fuse_table) IS shared —
  that was the sound half.
- divmagic/reassoc's rebuild tails (nc/nbl compaction) are
  look-alikes around DIFFERENT inner logic; a generic
  rebuild-with-callback would be over-abstraction. Left as is.
- bc_defs.hpp vs instr.hpp opcode tables: the bytecode/IR mirror is
  by design (two layers, one mapping).
- sccp's uses[] pushes site INDICES, not counts — not the bump
  counter; stays local.

## Where the next work is (priority order)

1. **OPT_CHECKLIST queue** (re-measured after ckpt 34): tailrec's
   tce_merge (~370, 745-line file, the mapv/remap twins already
   shared), argpromo (~408), unswitch_shape (433, the shared matcher
   + clone machinery). REUSE FIRST (the per-round protocol above);
   the UsCtx-lessons apply (NONE-defaulted ids).
2. **mjz_lib extraction candidates**: the loop-family gates
   (loop_single_exit_target et al.) are candidates for
   generic/mjz_lib/opt/shape.hpp once a second consumer exists
   (rmas's given_dominators form is ready for the same consumers).
3. **The LLVM-gap backlog** (OPT_CHECKLIST section A/B): PRE/LCM,
   loop-access/dependence, SCEV unification; DSE's chains are DONE.
   Machine layer: block placement (layout kernels are the
   foundation), peephole, spill splitting, shrink-wrapping.
4. **Widen the fuzz window**: post-unswitch windows 1-300 +
   20000-20300 verified this checkpoint (20029 proved their worth).
   The committed fuzz_parity.sh windows for refactors must be
   re-baselined after any new pass fires.

## Build in a recycled sandbox (the short version)

1) cmake is gone again? recreate the shim (the cache points at it):
   mkdir -p scripts/shim, then write scripts/shim/cmake as a /bin/sh
   script whose whole body is: if [ "$1" = "-E" ] && [ "$2" =
   "cmake_link_script" ]; then while IFS= read -r line; do if [ -n
   "$line" ]; then eval "$line" || exit $?; fi; done < "$3"; fi;
   exit 0  — then chmod +x it and point build/CMakeCache.txt's
   CMAKE_COMMAND:INTERNAL at it.
1b) CMakeLists include dirs changed while cmake is the shim? inject
   into the baked build tree (ckpt 32 added the graph module dirs):
   append the new -I paths to CXX_INCLUDES in
   build/CMakeFiles/minijz.dir/flags.make AND
   build/CMakeFiles/minijz_unit_tests.dir/flags.make, and sed the
   matching -I token in build/CTestTestfile.cmake's cx_check lines.
2) rebuild (touch the TUs: the shim skips cmake_depends):
   bash scripts/rebuild.sh
3) the battery:
   ./build/minijz --selftest          # 208
   ./build/minijz_unit_tests          # 6482
   ./build/minijz --demo              # 206
   ./build/minijz --fuzz-kernels=30000   # all 60 components
   ./build/minijz --fuzz=800              # e2e window
   bash scripts/stress.sh             # via MINIJZ_BIN=./build/minijz
   bugrepros: run each *.mjz under --check-all

## The gates that protect the tree

bash scripts/include_portability_test.sh    # 527/527
bash scripts/header_order_test.sh           # 116 + umbrella
python3 scripts/regen_audit_results.py      # identical unless a new
                                           # pass fires on the audit
                                           # programs
bash scripts/parity_diff.sh /tmp/minijz.baseline ./build/minijz
                                           # 146 runs, byte-identical
                                           # (rebuild the baseline
                                           # from a clean HEAD first)
clang-format --dry-run --Werror <file>      # 16.0.6, tree-wide clean
                                           # (NOT available in this
                                           # sandbox this session)

## Protocol (updated ckpt 31)

README task-list ledger update -> THE DEDUP PASS over the touched
files (standing requirement 1) -> full battery green (standing
requirement 2) -> commit -> `download/NN-*.zip` with fresh
SUMMARY.md + CONTINUE.md -> archive excluding `./build`,
`./build-ho`, `*.exe`, `*.out.tmp`, `*.mbc`, and `./fuzz_native`
artifacts (the mjz_lib .git RIDES IN the zip: the branch tracking
ships) -> do NOT push origin.

TRAPS: background processes die with the shell session (run
synchronously, never `| tail` a build); the `continue`-vs-fall-
through discipline is behavior; the allocation order of
new_anon_vreg calls is observable (the audit table keys on vreg
ids); formatting is clang-format 16.0.6 exactly; the kernel fuzzer's
component table is ORDER-COUPLED (KZ_NAMES == KZ_FNS == fuzz_base's
counter arrays); sharing a look-alike between passes REQUIRES proved
identical semantics — the lcf precedent, now the standing rule.
