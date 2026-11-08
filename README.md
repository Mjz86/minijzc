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
  base/                  pass_cfg (PassCfg + the 20-name toggle table),
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
ctest --test-dir build -L regr --output-on-failure    # bug repros + stress + header order + a scripted debugger session
```

**Compiler preference:** if nothing is pinned (`-DCMAKE_CXX_COMPILER`,
`$CXX`, toolchain), CMake looks for `clang++` first and uses it when
present (README note, now implemented); otherwise the platform default.

Options and presets:

* `cmake --preset clang` — force clang++; `--preset gcc` — force g++;
  `--preset fancy` — the full-fat clang++ line from the README discussion
  (C++26, `-O3 -g -march=native`, the whole `-W` battery,
  `-fconstexpr-steps=55555555`); `--preset header-only` — single TU;
  `--preset fuzz-wide` — a 2000-seed sharded ctest window.
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

Windows note: with no MSVC installed, generate for MinGW/Ninja and let
the clang preference find LLVM — `cmake -G "MinGW Makefiles"
-DCMAKE_CXX_COMPILER=clang++ -S . -B build` (PowerShell: use `;` between
steps, `&&` needs PS7). clang-cl's `-Wmicrosoft-include` notes on quoted
includes are benign here: every include resolves through `-I src`
portably; the search-rule difference only affects WHICH rule finds it
first.

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



# fix cmake failure 
 cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++
CMake Error at CMakeLists.txt:20 (project):
  Running

   'nmake' '-?'

  failed with:

   no such file or directory


-- Configuring incomplete, errors occurred!
PS C:\Users\Mehdi\Documents\code_projects\generic_string_class\minijzc> cmake --build build -j
Warning: NMake does not support parallel builds. Ignoring parallel build command line option.
no such file or directory
CMake Error: Generator: build tool execution failed, command was: nmake -f Makefile /nologo
PS C:\Users\Mehdi\Documents\code_projects\generic_string_class\minijzc> 

# add the following optimization passes ( or if present , extend and widen them):
Do the full 

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

# CONTINUE — next steps after checkpoint 09

Read SUMMARY.md (same zip root) first: it records the state (main @
4a0bf9c — parallel fuzz, interactive debugger, real TU boundary, clang-
format, cmake-clang — battery all green). This file is the resume manual.

## Setup (2 min)

```sh
cd minijzc                       # main @ 4a0bf9c
git log --oneline -6             # 4a0bf9c debugger gate ... ee02d79 parallel fuzz
export PATH="$HOME/.venv/bin:$PATH"   # cmake 4.4 via pip; clang-format too
cmake -S . -B build && cmake --build build -j2
./build/minijz --selftest                    # 206/206
ctest --test-dir build -L regr -j2           # 22 gates (includes debugger session)
./build/minijz --fuzz=550 --fuzz-from=9501 --fuzz-jobs=4   # parallel chunk
```

## Standing backlog (priority order)

1. **Keep widening the fuzz window** (rotation 9501+, random 8501+ — that
   is how all 6 bugs were found). Parallel now: `--fuzz-jobs=4` makes a
   2000-seed chunk ~4× faster; the watchdog names a stuck seed instead
   of hanging silently.
2. **Native parity in bulk**: more `--fuzz-native` windows (corpus: fz1-6,
   fz1500s, fz3401-40, fz10201-04, fz11001-36 tracked so far).
3. **Shave the prog TU** (14.8 s, the only heavy one): move
   `optimize_prog` + the pass bodies into their own TU behind a light
   decl (the pattern is established — see compile_impl/optimize_api);
   estimated prog → ~9 s + opt ~9 s in parallel.
4. Optional: split the remaining big headers along the established
   umbrella pattern (lcf.hpp 1021, cdfold 692, tailrec 662 →
   tce_self/tce_merge are independent, scctco 648, callgraph 546).

## The protocol (user-mandated — NEVER skip)

After EACH bug fix / significant change, in order:
1. `git commit` in minijzc (message: root cause in one line)
2. zip the working dir → /home/z/my-project/download/ (ONLY that dir is
   user-visible) with SUMMARY.md + CONTINUE.md updated to the new state
   at the zip root
3. STOP and wait for user confirmation.
Zips so far: fix1..fix6, final_complete, a144e9e snapshot, 07-3branch-
merge, 08-bug6-regr-gates, **09-fuzz-debugger-tusplit** (this one).
Do NOT delete the `snap-v2` ref (B2's history).

## Verify battery (run ALL before claiming any change; all must be green)

```sh
g++ -std=c++20 -O2 -I. -Isrc -I mjz_lib main.cpp -o minijz && ./minijz --selftest
cmake -S . -B build && cmake --build build -j2          # 9 real TUs
./build/minijz --selftest && ./build/minijz_unit_tests  # 206 + 6100
ctest --test-dir build -L cx --output-on-failure        # 6 proofs (~100 s)
ctest --test-dir build -L fuzz -j2                      # 4 gates
ctest --test-dir build -L regr -j2                      # 22 gates
./build/minijz examples/general.mjz --stdin=examples/general.in -O3
cmake -S . -B build-ho -DMINIJZ_MULTI_CPP=OFF && cmake --build build-ho -j2 \
  && ./build-ho/minijz --selftest                       # single-TU mode: 206/206
printf 'vm step 5\nvm state\ncheck\nquit\n' | ./build/minijz demos/tco_parity.mjz --debugger
```

## Gotchas (consolidated — the hard-won list, updated for this session)

- mjz_lib graph fns need `ulen` (unsigned long) vectors with ulen(-1)
  sentinels; uint32 truncation silently corrupts unreachable-node handling.
- g++ OOMs under -j parallelism: build -j2 max in the sandbox; the
  9-TU build is sized for that (biggest TU = prog @ ~15 s).
- Each cx proof MUST be its own TU (CMake already arranges this; clang
  uses -fconstexpr-steps instead of g++'s -fconstexpr-ops-limit — mapped).
- Do not chase: TRAP-vs-TIMEOUT, divmagic negative divisors (cleared),
  tailrec-in-constexpr (4 GiB limit, runtime only), SCC-shaped constexpr
  OOM (pre-existing, scoped out on purpose).
- Fresh seed ranges find fresh bugs. `--fuzz-from=S` rotates -O configs.
- Background processes DIE when a tool call ends: fuzz chunks must run
  in the FOREGROUND (`--fuzz-jobs` splits the work inside one process).
- For pass-level tracing: `--debug=2` still dumps per-pass; better: the
  interactive `--debugger` (`opt` re-runs the pipeline live; `vm state`
  shows the whole machine). The fuzz loop reports progress on stderr.
- `git status` "M" walls are 755 permission bits (core.fileMode=false);
  content is clean. Untracked `fuzz_native/*.exe` are regenerable.
- Prefer repro files over `-e "<inline src>"` (quoting bit us once).
- Known-bug history: bug1 tce_self chain order | bug2 tce_merge dispatch
  false-arm | bug3 scctco entry rotation | bug4 notcomb inv Gt/Ge swap |
  bug5 empty-block ghost succ | bug6 copyprop crossed-copy resolve()
  self-loop HANG (all fixed; examples/BUGFIXES.md; ctest label `regr`).
- New-TU rule: a heavy function compiled in exactly ONE TU must be
  non-static + declared in a light header (see app_api.hpp /
  optimize_api.hpp / program.hpp). constexpr/inline entries that are
  only DECLARED in a using TU need an emitted alias or a wrapper
  (see cx_pipeline_rt) — constexpr bodies are emitted only where used.
- clang-format is now the source of truth (`cmake --build build --target
  check-format`); LLVM style, includes NEVER sorted (order is
  documentation).

## If the user asks for a git-level 3-way merge anyway

Already content-merged (fa3816c) and the tree was restructured since —
a git merge of snap-v2 now would conflict broadly. If forced: merge on
main, prefer main's split-tree versions wholesale, then re-run the
battery.
