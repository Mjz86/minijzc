# minijzc

sloppy! test compiler — uses C++20 and https://github.com/Mjz86/String
(`mjz_lib/` inside it) to build an optimizing compiler for a simple lang.

The bug-hunt of 2026-11 fixed six optimizer miscompiles (see
`git log --grep=fix(bug`); the differential fuzzer and the cx proofs now
guard them continuously.

## Layout

```
src/minijz/           the compiler as a header-only constexpr library
  config.hpp            fundamental types, checked arithmetic (w_*), the
                        runner contract (RunOut), CHECK bookkeeping
  fwd.hpp               forward declarations (declaration-only contexts)
  graph_lib.hpp         the mjz_lib import, ISOLATED: only the headers that
                        actually need the graph/big-number library include it
  base/                  pass_cfg (PassCfg + the 28-name toggle table),
                        native_run (find_cc/slurp/run_capture/write_file)
  front/                 lexer, ast, parser
  ir/                    instr (IR core), pred_norm, builder, destruct,
                        stats (SsaStats/DestrStats)
  opt/analysis/          an (dominators/loops/SSA), alias, intervals, trapelim
  opt/mem2reg.hpp        array memory -> SSA registers + dead arrays
  opt/scalar/            fold, copyprop, canon (incl. notcomb), cse, unreach,
                        splice, dce, ifconv
  opt/loop/              licm, unroll, lcf, lcf_rt
  opt/linear/            linform, reassoc, divmagic
  opt/interprocedural/   callgraph (Tarjan + inliner), scctco, scctco_fold,
                        tailrec (tce_self/tce_merge), cdfold
  opt/driver.hpp         the pass manager (tiered fixpoint, -O0..-O3)
  opt/optimize_api.hpp   the light DECLARATION of optimize()/optimize_light()
  back/                  vm (interpreter + steppable VmCtx), bytecode,
                        emit_plan (EmitPlan), stack_alloc,
                        emit_c (+ emit_c_impl), emit_x64 (+ emit_x64_impl:
                        chordal regalloc via mjz_lib)
  runtime/astrun.hpp     the AST oracle (reference semantics)
  prog/                  program (LIGHT types + the compile() boundary),
                        compile_impl (the pipeline body), printers (IR/
                        runners pretty-printers), runners (umbrella)
  apps/                  cli_parse (LIGHT options), cli_run (reports/matrix/
                        repl), app_api (the extern app boundary),
                        app_main (the shared main body), demo, fuzz_gen,
                        fuzz_run (parallel differential fuzzer), shrink,
                        debugger (the interactive session)
  tests/cx_tours.hpp    the constexpr/compile-time tour battery
src/minijz.hpp         umbrella (single-TU / constexpr build)
src/tu/*.cpp           the multi-cpp translation units (9 REAL TUs)
tests/                  constexpr_check.cpp (cx proofs), unit_tests.cpp
examples/ demos/        showcase programs (+ general.in input tape), BUGFIXES.md
bugrepro/               historical repros for bugs 1-6, examples/BUGFIXES.md
cmake/                  run_showcases.cmake
```

Every header includes exactly what it uses — **include order does not
matter** (`scripts/header_order_test.sh` proves it over all 64 headers +
umbrella as a ctest). `constexpr` everywhere (no globals on the
compile-time paths); runtime-only helpers are `static`/`inline` and never
reached from constant expressions.

### The two build shapes (and why the multi-TU one is now real)

The library stays header-only so the constexpr proofs can run the whole
pipeline at compile time. The multi-cpp build splits the SAME bodies
along declaration/definition boundaries so each heavy composite is
compiled exactly once:

| TU                    | compiles                                | time  |
|-----------------------|-----------------------------------------|-------|
| `tu/main.cpp`         | options parsing only (light headers)    | ~1 s  |
| `tu/cli_app.cpp`      | the shared main body + reports + repl   | ~3 s  |
| `tu/debugger.cpp`     | the interactive debugger                | ~3 s  |
| `tu/demo.cpp`         | the demo suite                          | ~3 s  |
| `tu/shrink.cpp`       | the delta-debugger                      | ~3 s  |
| `tu/cxtours.cpp`      | the constexpr-tour battery              | ~6 s  |
| `tu/fuzz.cpp`         | generator + parallel fuzz driver        | ~4 s  |
| `tu/prog.cpp`         | parse/build/SSA/**every pass**/destruct | ~15 s |
| `tu/backends.cpp`     | C + x86-64 emitters (mjz_lib coloring)  | ~6 s  |

(the historical 8-TU layout re-compiled the whole header-only world in
every TU — the thin entry alone dropped from ~21 s to ~1 s.)

## Build & test (CMake)

```sh
git clone https://github.com/Mjz86/String mjz_lib   # inside this repo
cmake -S . -B build && cmake --build build -j        # default: multi-cpp
./build/minijz --selftest        # demos + fuzz(200) + native gate
ctest --test-dir build -L cx -j4 --output-on-failure  # the 6 constexpr proofs
ctest --test-dir build -L fuzz --output-on-failure    # differential fuzz
ctest --test-dir build -R unit --output-on-failure    # (or ./build/minijz_unit_tests)
ctest --test-dir build -L regr --output-on-failure    # bug repros + stress + header order + include portability + a scripted debugger session
```

The String clone works under BOTH upstream layouts (the historical
root-level `alllib_import.hpp` and the current nested
`mjz_lib/mjz_lib/` one): the committed `src/mjz_lib/alllib_import.hpp`
bridge adapts via `__has_include`, so a fresh clone needs zero extra
steps — no symlinks, no configure-time generation.

**Compiler preference:** if nothing is pinned (`-DCMAKE_CXX_COMPILER`,
`$CXX`, toolchain), CMake looks for `clang++` first and uses it when
present (README note, now implemented); otherwise the platform default.

Options and presets:

* `cmake --preset clang` — force clang++; `--preset gcc` — force g++;
  `--preset fancy` — the full-fat clang++ line from the README discussion
  (C++26, `-O3 -g -march=native`, the whole `-W` battery,
  `-fconstexpr-steps=55555555`); `--preset header-only` — single TU;
  `--preset fuzz-wide` — a 2000-seed sharded ctest window.
* Windows (see the dedicated section below): `--preset windows-llvm`
  (MinGW Makefiles + clang++), `--preset windows-ninja` (Ninja + clang++),
  `--preset vs-clang` (VS 2022 generator + the clang-cl toolset). They
  carry a host condition, so `cmake --list-presets` only offers them on
  Windows.
* `-DMINIJZ_MULTI_CPP=OFF` — single translation unit (the header-only
  constexpr form).
* `-DMINIJZ_EXT_WARNINGS=ON` — the extended warning battery (per-compiler
  mapping: `-Wshadow -Wsign-conversion -Wcast-qual ...`, `-ferror-limit`
  vs `-fmax-errors`); `-DMINIJZ_NATIVE_TUNING=ON` adds `-march=native`;
  `-DMINIJZ_WERROR=ON` upgrades to errors.
* `-DMINIJZ_CXX_STANDARD=26` — build as C++26 (default 20).
* `-DFUZZ_SEEDS=start:count` — the differential-fuzz seed window for
  ctest; `-DFUZZ_SHARDS=N` splits it into N cases; `-DFUZZ_JOBS=N` runs
  each ctest fuzz case with N parallel workers. Different windows
  surface different bugs: `cmake -B build -DFUZZ_SEEDS=4001:4000`.
* `-DFUZZ_RANDOM_CONFIGS=OFF` — drop the random-pass-config fuzz cases.
* `cmake --build build --target format` / `check-format` — run / verify
  clang-format over the sources (formatting is now tree-wide consistent;
  the config is `.clang-format`, LLVM base, includes never reordered).
* `cmake --build build --target showcases` — runs general/showcase and the
  demos at -O0..-O3 into `build/showcases/`, a **git subdirectory of the
  build folder**: every run is committed, so
  `cmake --build build --target showcase-diff` (or `git log` / `git diff`
  there) shows the difference between versions of the compiler.
  (A rebuild with unchanged outputs adds no commit — that is the point.)

g++ and clang++ spell the constexpr budget differently; CMake maps it
(`-fconstexpr-depth`/`-fconstexpr-ops-limit` vs `-fconstexpr-steps`).

Plain g++ still works, both modes:

```sh
g++ -std=c++20 -O2 -I. -Isrc -I mjz_lib main.cpp -o minijz           # single TU
g++ -std=c++20 -O2 -DMJZ_MULTI_CPP -I. -Isrc -I mjz_lib -c src/tu/*.cpp && g++ *.o -o minijz
```

## Windows toolchains (the "nmake" trap, fixed)

The original failure (kept for the record):

```
cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++
CMake Error at CMakeLists.txt:20 (project):
  Running 'nmake' '-?' failed with: no such file or directory
```

Cause: from a plain PowerShell with no Visual Studio environment,
CMake's default generator on Windows is `NMake Makefiles`; with no VS
installed there is no `nmake`, and `project()` dies before anything
useful can happen (the follow-up `cmake --build` then complains that
"NMake does not support parallel builds" — same root cause).

What is in place now:

1. **Early interception** — CMakeLists.txt checks, BEFORE `project()`,
   whether the generator is NMake and `nmake` is missing, and fails
   with a message that lists the fixes below instead of the cryptic
   `'nmake' '-?' failed` one.
2. **Ready-made presets** (host-conditioned, Windows-only):
   `cmake --preset windows-llvm` (MinGW Makefiles + clang++ — no MSVC
   needed), `cmake --preset windows-ninja` (Ninja + clang++),
   `cmake --preset vs-clang` (VS 2022 generator + the clang-cl
   toolset, for machines with VS installed).
3. Manual equivalent of the first recipe:
   `cmake -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=clang++ -S . -B build`.
4. Or: install VS Build Tools and configure from an "x64 Native Tools"
   prompt so `nmake`/`CL` exist and even the default generator works.

PowerShell note: chain steps with `;` (`&&` needs PowerShell 7+).
clang-cl is a first-class config now: every quoted include resolves in
its includer's own directory (see warnings.txt), so the old
`-Wmicrosoft-include` noise is gone — only genuinely portable includes
remain (`include_portability` is a ctest `regr` gate).

## The compile-time proof

Each `MJZ_CX_CHECK=k` is its own compiler invocation (each proof retains
its full constant-expression state until end-of-TU; each holds up to
~4 GiB, so they run serially):

```sh
for k in 1 2 3 4 5 6; do
  g++ -std=c++20 -O0 -I. -Isrc -I mjz_lib -DMJZ_CX_CHECK=$k \
      -fconstexpr-depth=8192 -fconstexpr-ops-limit=4000000000 \
      -c tests/constexpr_check.cpp -o /dev/null && echo "cx check $k: OK"
done
```

1. arena arrays + lib loop analyses
2. recursion / inline / const-call / tail-call
3. unrolling / closed forms / divmagic / input()
4. branchy unrolling + short-circuit
5. the bug-3 shape (`fa(8)` through the default scctco engine)
6. comparison negation at the equality points

The tailrec/tce_merge engine cannot run inside a static proof within a
4 GiB retained-state budget; it is covered by the runtime battery
(`cx_pipeline()` via `--selftest`), `tests/unit_tests.cpp`, and the
fuzzer's pass rotations.

## Fuzzing, shrinking — and parallel fuzz

```sh
./minijz --fuzz=2000 --fuzz-from=1          # 5-way differential (rotating -O configs)
./minijz --fuzz=2000 --fuzz-cfg=random      # + a random pass config per seed
./minijz --fuzz=100 --fuzz-native=8         # + C/asm builds at -O0/-O2
./minijz --fuzz=1000 --fuzz-jobs=4          # 4 parallel workers (default: auto)
./minijz --fuzz=2 --fuzz-from=1210 --fuzz_dump   # dump the last 2 programs + tapes
```

* `--fuzz-jobs=N` — parallel workers over an atomic seed queue (default:
  the selftest stays serial so its output is byte-stable; a plain fuzz
  run uses the core count, capped at 4). Results are seed-deterministic;
  only report interleaving can vary.
* **Live progress** on stderr: `[fuzz 250/1000] 31s elapsed, eta 94s,
  mismatches 0` — a big run is never silent again.
* **Slow-seed notes** (any seed over 5 s) and a **stall watchdog** that
  names the in-flight seeds if a chunk stops progressing, with the
  `--fuzz-from=<seed>` bisect hint — the pathological seed is identified
  instead of looking like a hang (`--fuzz=1000` now reports continuously
  and halts; it used to look stuck while grinding silently).
* `--fuzz_dump` replaces the old `MJZ_FUZZ_DUMP` environment variable.
* On mismatch it AUTO-SAVES `fuzz_fail_<seed>.mjz` + `.flags` (the exact
  repro command line) + `.in` (the tape) → shrink:

```sh
./minijz --shrink=fuzz_fail_<seed>.mjz --stdin=fuzz_fail_<seed>.in <flags>
```

  delta-debugs it (all five runners + native C/asm parity must keep
  failing) and writes `FILE.small.mjz`.

## `--check-all` / runners

Every run verifies oracle (AST) vs SSA vs optimized vs destroyed vs VM,
and `--fuzz-native` additionally builds the C and asm backends at -O0/-O2
and compares byte-for-byte.

## `--debugger` — the interactive facility

An always-answering session over the compiled program; no timeouts, no
waiting — every command dumps state instantly. Commands come from stdin,
so it is scriptable (`printf 'vm step 10\nvm state\nquit\n' | ./minijz
f.mjz --debugger`) and there is a scripted session in the ctest `regr`
gates. The tape comes from `--stdin=FILE` (the debugger reads COMMANDS
from stdin).

```
help                    command list
src / fns / stats       source, function table, pipeline statistics
ir [FN] [ssa|opt|dst|all]   dump IR at any stage
cfg [FN]                CFG: blocks/preds/succs (main: + mjz_lib analysis)
plan [FN]               emission plan (order, next, dead, fused)
bc [FN]                 bytecode listing; opt [FN] re-runs the optimizer
                        LIVE, pass by pass, IR after every changing pass
check                   all five runners + comparison
vm reset/step/run/until/break/unbreak/breaks/state/dis
                        the steppable machine: single-op tracing, pc
                        breakpoints, aligned disassembly, and `vm state`
                        = EVERYTHING (pc+fn, frames+slots, operand stack,
                        arena, input position, output)
quit
```

The VM half runs on the `VmCtx` stepper (`back/vm.hpp`); `vm_run()` is a
thin loop over `vm_step_op()` with identical semantics (the constexpr
proofs still run the VM at compile time).

## After you change something

Run the battery (all green = trustworthy):

```sh
g++ -std=c++20 -O2 -I. -Isrc -I mjz_lib main.cpp -o minijz && ./minijz --selftest
cmake --build build -j2 && ctest --test-dir build -L "cx|fuzz|regr" --output-on-failure
```

---

# The task list (status ledger)

The three tasks below came in with the branchA README ("ADDED TASKS");
this section tracks their state.

## 1. fix cmake failure — DONE

The pasted failure (default NMake generator on a machine with no Visual
Studio → `'nmake' '-?' failed`) is handled by the "Windows toolchains"
section above: an early pre-`project()` interception with a friendly
message, the host-conditioned presets `windows-llvm` / `windows-ninja` /
`vs-clang`, and the documented manual recipes.

## 2. fix the warnings — DONE

`warnings.txt` (216 clang-cl warnings) is now the triage ledger: the 215
`-Wmicrosoft-include` hits were the quoted-include portability problem,
fixed tree-wide (every quoted include resolves in its includer's own
directory — enforced by the `include_portability` ctest gate); the one
real `-Wdangling-gsl` (a temporary's `.c_str()` outliving the
full-expression in the debugger's frame listing) is fixed at the source.
See `warnings.txt` for the full story and the regeneration recipe.

## 3. add the following optimization passes (or, if present, extend and widen them)

Status of the list as implemented in this tree (names in backticks are
the ctest/CLI toggle names):

* already covered: DCE/ADCE (`dce`, mark-and-sweep from side-effect
  roots — trap-aware), dead-block + unreachable-block elimination
  (`unreach`), CFG simplification / block merging / jump-chain splicing
  (`splice`), branch folding (`fold` + `unreach`), if-conversion
  (`ifconv`), loop deletion-via-closed-forms (`lcf` + `cdfold`),
  constant folding/propagation (`fold`), SCCP + branch pruning
  (`sccp`), algebraic simplification / instruction combining
  (`fold` + `canon`), reassociation (`reassoc`), strength reduction
  (`divmagic`), CSE (`cse`, dominator-scoped), value / correlated value
  propagation + range analysis + constraint elimination (`trapelim`
  range proofs + `sccp`), load/store forwarding + redundant load /
  store elimination (`ldelim`), mem2reg / SROA / DSE (`mem2reg`),
  inlining (`inline`), interprocedural constant propagation + IPSCCP
  (`constcall`), pure-call CSE (`callcse`), LICM (`licm`), loop
  unrolling (`unroll`), tail-call elimination (`tailrec` + `scctco`),
  IR canonicalization (`canon`), dead-PHI elimination (`dce` +
  `fold`), **dead-argument elimination (`deadargs`, interprocedural —
  params unused everywhere incl. mutual-recursion chains are dropped
  from signatures and every call site)**, **identical code folding
  (`icf`, interprocedural — functions with structurally equivalent
  normalized bodies merge by partition refinement over callee
  fingerprints with self-recursion wildcards; mutually recursive
  twins collapse into one self-recursive function and every caller is
  retargeted)**, **function specialization (`fnspec`, interprocedural —
  a parameter holding the same `Const` at every non-recursive call
  site buys a specialized clone with the constant bound at entry and
  the argument dropped from the clone's signature)**, **argument
  promotion (`argpromo`, interprocedural — a read-only constant-index
  array parameter becomes scalar element parameters; the callee's Loads
  become plain param uses and each promoted call site materializes the
  provably in-bounds element loads right before the call, so arrays that
  stop escaping lift and constant element stores fold the whole call at
  compile time)**, **jump threading (`jthread` — a branch edge into an
  "elbow" block (phis + one comparison + a branch) whose comparison is
  decidable PER EDGE (the range hull over all preds is not) redirects
  straight to the taken successor; both edges of a correlated merge
  thread and the redundant branch vanishes)**.
* still open (the standing wishlist): tail
duplication, loop rotation/simplification/canonicalization proper,
  LCSSA, GVN (beyond the dominator-scoped CSE + ldelim + callcse
  combination), demanded/known bits, MemCpyOpt,
  loop fusion/fission/distribution, loop unswitching/versioning/predication,
  function outlining, Attributor-style inference, GlobalOpt.

The original wishlist (kept verbatim for reference):

Mostly architecture-independent optimizations

Control-flow optimizations

• Dead Code Elimination — DCE

• Aggressive Dead Code Elimination — ADCE

• Dead Basic Block Elimination

• Dead Global Elimination — GlobalDCE

• Control-flow graph simplification — SimplifyCFG

• Jump threading

• Branch folding

• Unreachable-block elimination

• If-conversion in simple cases

• Switch lowering simplification

• Tail duplication

• Loop deletion

• Loop rotation

• Loop simplification

• Loop canonicalization

• Loop-closed SSA construction — LCSSA

Scalar and expression optimizations

• Constant folding

• Constant propagation

• Sparse Conditional Constant Propagation — SCCP

• Interprocedural SCCP — IPSCCP

• Algebraic simplification

• Instruction combining — InstCombine

• Reassociation

• Strength reduction

• Common subexpression elimination

• Global Value Numbering — GVN

• Early CSE — EarlyCSE

• Partial redundancy elimination

• Value propagation

• Correlated value propagation

• Constraint elimination

• Range analysis

• Undefined-behavior exploitation

• Simplification using known bits

• Simplification using demanded bits

Memory-related optimizations

• Memory-to-register promotion — mem2reg

• Scalar Replacement of Aggregates — SROA

• Dead Store Elimination — DSE

• Memory optimization — MemCpyOpt

• Load/store forwarding

• Redundant load elimination

• Store-to-load forwarding

• Redundant store elimination

• Alias-analysis-assisted optimization

• Stack allocation promotion

• Local stack-slot merging

• Lifetime-marker optimization

• Null-pointer check elimination in valid cases

Function and interprocedural optimizations

• Function inlining

• Always-inline processing

• Function outlining

• Function specialization

• Argument promotion

• Return-value promotion

• Interprocedural constant propagation

• Interprocedural dead argument elimination

• Global variable optimization — GlobalOpt

• Function attribute inference — Attributor

• 

Inference of:

• 

readonly

• readnone

• noreturn

• nounwind

• willreturn

• nonnull

• alignment information

• 

memory effects

• 

Dead argument elimination

• Global constant propagation

• Constant merging

• Identical code folding, depending on configuration

Loop optimizations

These are generally target-independent in terms of correctness:

• Loop-invariant code motion — LICM

• Induction-variable simplification — IndVarSimplify

• Scalar evolution analysis — ScalarEvolution

• Loop unswitching

• Loop deletion

• Loop fusion

• Loop fission

• Loop distribution

• Loop unrolling

• Loop peeling

• Loop versioning

• Loop predication

• Loop idiom recognition

• Strength reduction inside loops

• Range-check elimination

• Bounds-check simplification

• Loop access analysis

However, unrolling, peeling, fusion, and similar transformations usually use target-specific cost models.

Code-size and IR cleanup

• CFG cleanup

• Basic-block merging

• Instruction simplification

• Redundant cast elimination

• Dead PHI elimination

• Unused global elimination

• Attribute-based simplification

• IR canonicalization

• Debug-information-aware cleanup

• Constant and string merging


(The resume manual for the current checkpoint lives in CONTINUE.md at
the delivered zip root, not in the README — the embedded checkpoint-09
copy was stale: it described main @ 4a0bf9c, before the TU split was
merged into branchA.)
