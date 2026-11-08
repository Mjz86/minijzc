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
