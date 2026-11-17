# CONTINUE — next steps after checkpoint 31 (the dedup campaign)

Read SUMMARY.md (same zip root) first. The main repo is 16 commits
ahead of origin/main; mjz_lib rides branch `graph-opt-kernels` (six
commits, diffable against main). This file is the resume manual.

## What this checkpoint shipped: the code-bloat cleanup

The user's finding: "many places have the EXACT same algorithm
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

## STANDING REQUIREMENTS (added by the user's directive, ckpt 31)

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

1. **OPT_CHECKLIST queue** (re-measured after the dedup): unswitch
   (~389), argpromo (~387, the twins already gone), tailrec's
   tce_merge (~385), unswitch_shape (429). rotate_impl.hpp is the
   freshest template; REUSE FIRST (the requirement above).
2. **mjz_lib extraction candidates found by the scan**: the
   kc/kv seeders and the bump counters are IR glue (stay in
   instr.hpp); but if a SECOND compiler ever grows, they are the
   first kernels to lift. The loop-family gates
   (loop_single_exit_target et al.) are candidates for
   mjz_lib/opt/shape.hpp once a second consumer exists.
3. **The LLVM-gap backlog** (OPT_CHECKLIST section A/B): PRE/LCM,
   loop-access/dependence, SCEV unification; DSE's chains are DONE.
   Machine layer: block placement (layout kernels are the
   foundation), peephole, spill splitting, shrink-wrapping.
4. **Widen the fuzz window**: post-dedup windows 1-600 + 20000-20500
   verified this checkpoint. The committed fuzz_parity.sh windows
   for refactors must be re-baselined after any new pass fires.

## Build in a recycled sandbox (the short version)

1) cmake is gone again? recreate the shim (the cache points at it):
   mkdir -p scripts/shim, then write scripts/shim/cmake as a /bin/sh
   script whose whole body is: if [ "$1" = "-E" ] && [ "$2" =
   "cmake_link_script" ]; then while IFS= read -r line; do if [ -n
   "$line" ]; then eval "$line" || exit $?; fi; done < "$3"; fi;
   exit 0  — then chmod +x it and point build/CMakeCache.txt's
   CMAKE_COMMAND:INTERNAL at it.
2) rebuild (touch the TUs: the shim skips cmake_depends):
   bash scripts/rebuild.sh
3) the battery:
   ./build/minijz --selftest          # 208
   ./build/minijz_unit_tests          # 6476
   ./build/minijz --demo              # 206
   ./build/minijz --fuzz-kernels=30000   # all 60 components
   ./build/minijz --fuzz=800              # e2e window
   bash scripts/stress.sh             # via MINIJZ_BIN=./build/minijz
   bugrepros: run each *.mjz under --check-all

## The gates that protect the tree

bash scripts/include_portability_test.sh    # 515/515
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
