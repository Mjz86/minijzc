# minijzc

sloppy! test compiler — uses C++20 and https://github.com/Mjz86/String
(`mjz_lib/` inside it) to build an optimizing compiler for a simple lang.

The bug-hunt of 2026-11 fixed four optimizer miscompiles (see
`git log --grep=fix(bug`); the differential fuzzer and the cx proofs now
guard them continuously.

## Layout

```
src/minijz/           the compiler as a header-only constexpr library
  config.hpp            fundamental types, checked arithmetic (w_*), the
                        runner contract (RunOut), the pipeline road map
  graph_lib.hpp         the mjz_lib import, ISOLATED: only the headers that
                        actually need the graph/big-number library include it
  base/                  pass_cfg (PassCfg + the 20-name toggle table),
                        native_run (find_cc/slurp/run_capture/write_file)
  front/                 lexer, ast, parser
  ir/                    instr (IR core), pred_norm, builder, destruct
  opt/analysis/          an (dominators/loops/SSA), alias, intervals, trapelim
  opt/mem2reg.hpp        array memory -> SSA registers + dead arrays
  opt/scalar/            fold, copyprop, canon (incl. notcomb), cse, unreach,
                        splice, dce, ifconv
  opt/loop/              licm, unroll, lcf, lcf_rt
  opt/linear/            linform, reassoc, divmagic
  opt/interprocedural/   callgraph (Tarjan + inliner), scctco, scctco_fold,
                        tailrec (tce_self/tce_merge), cdfold
  opt/driver.hpp         the pass manager (tiered fixpoint, -O0..-O3)
  back/                  vm, emit_c, emit_x64 (chordal regalloc), bytecode,
                        stack_alloc
  runtime/astrun.hpp     the AST oracle (reference semantics)
  prog/                  program (compile()), runners (printers/text_of)
  apps/                  demo, fuzz_gen, fuzz_run, cli, shrink
  tests/cx_tours.hpp    the constexpr/compile-time tour battery
src/minijz.hpp         umbrella (single-TU / constexpr build)
src/tu/*.cpp           the multi-cpp translation units
tests/                  constexpr_check.cpp (cx proofs), unit_tests.cpp
examples/ demos/        showcase programs (+ general.in input tape), BUGFIXES.md
bugrepro/               historical repros for bugs 1-5, see examples/BUGFIXES.md
cmake/                  run_showcases.cmake
```

Every header includes exactly what it uses — **include order does not
matter**. `constexpr` everywhere (no globals on the compile-time paths);
runtime-only helpers are `static` and never reached from constant
expressions.

## Build & test (CMake)

```sh
git clone https://github.com/Mjz86/String mjz_lib   # inside this repo
cmake -S . -B build && cmake --build build -j        # default: multi-cpp
./build/minijz --selftest        # demos + fuzz(200) + native gate
ctest --test-dir build -L cx -j4 --output-on-failure  # the 6 constexpr proofs
ctest --test-dir build -L fuzz --output-on-failure    # differential fuzz
ctest --test-dir build -R unit --output-on-failure    # (or ./build/minijz_unit_tests)
```

Options (see `CMakeLists.txt`):

* `-DMINIJZ_MULTI_CPP=OFF` — single translation unit instead of 8 linked
  TUs (the header-only constexpr form; slightly slower to build, needed
  nowhere but the smallest machines).
* `-DFUZZ_SEEDS=start:count` — the differential-fuzz seed window for
  ctest; `-DFUZZ_SHARDS=N` splits it into N cases. Different windows
  surface different bugs: `cmake -B build -DFUZZ_SEEDS=4001:4000`.
* `-DFUZZ_RANDOM_CONFIGS=OFF` — drop the random-pass-config fuzz cases.
* `cmake --build build --target showcases` — runs general/showcase and the
  demos at -O0..-O3 into `build/showcases/`, a **git subdirectory of the
  build folder**: every run is committed, so
  `cmake --build build --target showcase-diff` (or `git log` / `git diff`
  there) shows the difference between versions of the compiler.
  (A rebuild with unchanged outputs adds no commit — that is the point.)

Plain g++ still works, both modes:

```sh
g++ -std=c++20 -O2 -I. -Isrc -I mjz_lib main.cpp -o minijz           # single TU
g++ -std=c++20 -O2 -Isrc -I. -I mjz_lib -c src/tu/*.cpp ...           # = CMake
```

## The compile-time proof

Each `MJZ_CX_CHECK=k` is its own g++ invocation (g++ retains each constant
expression's evaluation state until end-of-TU; each proof holds up to
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

## Fuzzing & shrinking

```sh
./minijz --fuzz=2000 --fuzz-from=1          # 5-way differential (rotating -O configs)
./minijz --fuzz=2000 --fuzz-cfg=random      # + a random pass config per seed
./minijz --fuzz=100 --fuzz-native=8         # + C/asm builds at -O0/-O2
MJZ_FUZZ_DUMP=1 ./minijz --fuzz=2 --fuzz-from=1210 2>&1 | grep -A30 "SEED 1211"
```

A mismatch auto-saves `fuzz_fail_<seed>.mjz` + `.flags` (the exact repro
command line) + `.in` (the tape). Then:

```sh
./minijz --shrink=fuzz_fail_<seed>.mjz --stdin=fuzz_fail_<seed>.in <flags>
```

delta-debugs it (all five runners + native C/asm parity must keep failing)
and writes `FILE.small.mjz`.

## `--check-all` / runners

Every run verifies oracle (AST) vs SSA vs optimized vs destroyed vs VM,
and `--fuzz-native` additionally builds the C and asm backends at -O0/-O2
and compares byte-for-byte.


#NOTE:
next time we will do clang format so the formatting is consistent





NOTE:

PS C:\Users\Mehdi\Documents\code_projects\generic_string_class\minijzc> cmake -S . -B build && cmake --build build -j   -DMINIJZ_MULTI_CPP                            
At line:1 char:21
+ cmake -S . -B build && cmake --build build -j   -DMINIJZ_MULTI_CPP
+                     ~~
The token '&&' is not a valid statement separator in this version.
    + CategoryInfo          : ParserError: (:) [], ParentContainsErrorRecordException
    + FullyQualifiedErrorId : InvalidEndOfLine
 
PS C:\Users\Mehdi\Documents\code_projects\generic_string_class\minijzc> cmake -S . -B build ; cmake --build build -j   -DMINIJZ_MULTI_CPP 
-- Building for: NMake Makefiles
CMake Error at CMakeLists.txt:2 (project):
  Running

   'nmake' '-?'

  failed with:

   no such file or directory


CMake Error: CMAKE_CXX_COMPILER not set, after EnableLanguage
-- Configuring incomplete, errors occurred!
Unknown argument -DMINIJZ_MULTI_CPP
Usage: cmake --build <dir>             [options] [-- [native-options]]
       cmake --build --preset <preset> [options] [-- [native-options]]
Options:
  <dir>          = Project binary directory to be built.
  --preset <preset>, --preset=<preset>
                 = Specify a build preset.
  --list-presets[=<type>]
                 = List available build presets.
  --parallel [<jobs>], -j [<jobs>]
                 = Build in parallel using the given number of jobs. 
                   If <jobs> is omitted the native build tool's 
                   default number is used.
                   The CMAKE_BUILD_PARALLEL_LEVEL environment variable
                   specifies a default parallel level when this option
                   is not given.
  -t <tgt>..., --target <tgt>...
                 = Build <tgt> instead of default targets.
  --config <cfg> = For multi-configuration tools, choose <cfg>.
  --clean-first  = Build target 'clean' first, then build.
                   (To clean only, use --target 'clean'.)
  --resolve-package-references={on|only|off}
                 = Restore/resolve package references during build.
  -v, --verbose  = Enable verbose output - if supported - including
                   the build commands to be executed. 
  --             = Pass remaining options to the native tool.


  make cmake detect and use clang , also i think some clang and gcc flags differ , for example i use somthing like if i want to be fancy :
clang++ -std=c++26 -O3   -g -fprebuilt-module-path=build  -fconstexpr-steps=55555555 -fexperimental-library  -march=native   -Wall -Wextra  -Walloca -Wcast-align -Wcast-qual -Wctor-dtor-privacy -Wdeprecated-copy-dtor -Wdouble-promotion -Wenum-conversion -Wfloat-equal -Wformat-signedness -Wformat=2 -Wmismatched-tags  -Wmultichar -Wnon-virtual-dtor -Woverloaded-virtual -Wpointer-arith -Wrange-loop-construct -Wshadow -Wuninitialized -Wvla -Wwrite-strings -Wsign-conversion -Wdelete-non-virtual-dtor  -ferror-limit=30       -lstdc++  

