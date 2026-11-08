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


# REMOVE EACH NOTE AFTER THE FIX
NOTE:
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


and :
 clang++   main.cpp  -std=c++26 -O3 -march=native  -g -fconstexpr-steps=12712420 -o build/a.exe
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:6:
.\src\minijz/graph_lib.hpp:12:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\../mjz_lib/alllib_import.hpp [-Wmicrosoft-include]
   12 | #include "../mjz_lib/alllib_import.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:8:
.\src\minijz/base/pass_cfg.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/config.hpp [-Wmicrosoft-include]
    6 | #include "minijz/config.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:9:
.\src\minijz/front/lexer.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/config.hpp [-Wmicrosoft-include]
    5 | #include "minijz/config.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:10:
.\src\minijz/front/ast.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/config.hpp [-Wmicrosoft-include]
    5 | #include "minijz/config.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:11:
.\src\minijz/front/parser.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/front/ast.hpp [-Wmicrosoft-include]
    5 | #include "minijz/front/ast.hpp"
      |          ^
.\src\minijz/front/parser.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/front/lexer.hpp [-Wmicrosoft-include]
    6 | #include "minijz/front/lexer.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:12:
.\src\minijz/ir/instr.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/config.hpp [-Wmicrosoft-include]
    5 | #include "minijz/config.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:13:
.\src\minijz/ir/pred_norm.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:14:
.\src\minijz/ir/builder.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/front/ast.hpp [-Wmicrosoft-include]
    5 | #include "minijz/front/ast.hpp"
      |          ^
.\src\minijz/ir/builder.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/ir/builder.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/pred_norm.hpp [-Wmicrosoft-include]
    7 | #include "minijz/ir/pred_norm.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:15:
.\src\minijz/ir/destruct.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    6 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/ir/destruct.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/analysis/an.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:15:
In file included from .\src\minijz/ir/destruct.hpp:7:
.\src\minijz/opt/analysis/an.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/graph_lib.hpp [-Wmicrosoft-include]
    6 | #include "minijz/graph_lib.hpp"
      |          ^
.\src\minijz/opt/analysis/an.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    7 | #include "minijz/ir/instr.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:17:
.\src\minijz/opt/analysis/alias.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    5 | #include "minijz/ir/instr.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:18:
.\src\minijz/opt/analysis/intervals.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    5 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/analysis/intervals.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/alias.hpp [-Wmicrosoft-include]
    6 | #include "minijz/opt/analysis/alias.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:19:
.\src\minijz/opt/analysis/trapelim.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/analysis/trapelim.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/analysis/trapelim.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/alias.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/analysis/alias.hpp"
      |          ^
.\src\minijz/opt/analysis/trapelim.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/intervals.hpp [-Wmicrosoft-include]
    8 | #include "minijz/opt/analysis/intervals.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:20:
.\src\minijz/opt/mem2reg.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    6 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/mem2reg.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    7 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/mem2reg.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    8 | #include "minijz/opt/analysis/an.hpp"
      |          ^
.\src\minijz/opt/mem2reg.hpp:9:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/alias.hpp [-Wmicrosoft-include]
    9 | #include "minijz/opt/analysis/alias.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:21:
.\src\minijz/opt/scalar/fold.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/scalar/fold.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/scalar/fold.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/analysis/an.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:22:
.\src\minijz/opt/scalar/copyprop.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/scalar/copyprop.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/scalar/copyprop.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/analysis/an.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:23:
.\src\minijz/opt/scalar/canon.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    6 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/scalar/canon.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    7 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/scalar/canon.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    8 | #include "minijz/opt/analysis/an.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:24:
.\src\minijz/opt/scalar/cse.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/scalar/cse.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/scalar/cse.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/analysis/an.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:25:
.\src\minijz/opt/scalar/unreach.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/scalar/unreach.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/scalar/unreach.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/analysis/an.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:26:
.\src\minijz/opt/scalar/splice.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/scalar/splice.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/scalar/splice.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/analysis/an.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:27:
.\src\minijz/opt/scalar/dce.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/scalar/dce.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/scalar/dce.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/analysis/an.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:28:
.\src\minijz/opt/scalar/ifconv.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/scalar/ifconv.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/scalar/ifconv.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/intervals.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/analysis/intervals.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:29:
.\src\minijz/opt/loop/licm.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/loop/licm.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/loop/licm.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/analysis/an.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:30:
.\src\minijz/opt/loop/unroll.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/loop/unroll.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/loop/unroll.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/analysis/an.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:31:
.\src\minijz/opt/loop/lcf.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    6 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/loop/lcf.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    7 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/loop/lcf.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    8 | #include "minijz/opt/analysis/an.hpp"
      |          ^
.\src\minijz/opt/loop/lcf.hpp:9:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/linear/linform.hpp [-Wmicrosoft-include]
    9 | #include "minijz/opt/linear/linform.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:31:
In file included from .\src\minijz/opt/loop/lcf.hpp:9:
.\src\minijz/opt/linear/linform.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    5 | #include "minijz/ir/instr.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:32:
.\src\minijz/opt/loop/lcf_rt.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    6 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/loop/lcf_rt.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    7 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/loop/lcf_rt.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    8 | #include "minijz/opt/analysis/an.hpp"
      |          ^
.\src\minijz/opt/loop/lcf_rt.hpp:9:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/linear/linform.hpp [-Wmicrosoft-include]
    9 | #include "minijz/opt/linear/linform.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:34:
.\src\minijz/opt/linear/reassoc.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/linear/reassoc.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/linear/reassoc.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/linear/linform.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/linear/linform.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:35:
.\src\minijz/opt/linear/divmagic.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/linear/divmagic.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/linear/divmagic.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/graph_lib.hpp [-Wmicrosoft-include]
    7 | #include "minijz/graph_lib.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:36:
.\src\minijz/opt/interprocedural/callgraph.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/graph_lib.hpp [-Wmicrosoft-include]
    6 | #include "minijz/graph_lib.hpp"
      |          ^
.\src\minijz/opt/interprocedural/callgraph.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    7 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/interprocedural/callgraph.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    8 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/interprocedural/callgraph.hpp:9:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    9 | #include "minijz/opt/analysis/an.hpp"
      |          ^
.\src\minijz/opt/interprocedural/callgraph.hpp:10:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/scalar/dce.hpp [-Wmicrosoft-include]
   10 | #include "minijz/opt/scalar/dce.hpp"
      |          ^
.\src\minijz/opt/interprocedural/callgraph.hpp:11:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/loop/lcf_rt.hpp [-Wmicrosoft-include]
   11 | #include "minijz/opt/loop/lcf_rt.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:37:
.\src\minijz/opt/interprocedural/scctco.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    6 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/interprocedural/scctco.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    7 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/interprocedural/scctco.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/interprocedural/callgraph.hpp [-Wmicrosoft-include]
    8 | #include "minijz/opt/interprocedural/callgraph.hpp"
      |          ^
.\src\minijz/opt/interprocedural/scctco.hpp:9:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/interprocedural/scctco_fold.hpp [-Wmicrosoft-include]
    9 | #include "minijz/opt/interprocedural/scctco_fold.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:37:
In file included from .\src\minijz/opt/interprocedural/scctco.hpp:9:
.\src\minijz/opt/interprocedural/scctco_fold.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    5 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/interprocedural/scctco_fold.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/interprocedural/scctco_fold.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/interprocedural/callgraph.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/interprocedural/callgraph.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:39:
.\src\minijz/opt/interprocedural/tailrec.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    6 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/interprocedural/tailrec.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    7 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/interprocedural/tailrec.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    8 | #include "minijz/opt/analysis/an.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:40:
.\src\minijz/opt/interprocedural/cdfold.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    6 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/interprocedural/cdfold.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    7 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/interprocedural/cdfold.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    8 | #include "minijz/opt/analysis/an.hpp"
      |          ^
.\src\minijz/opt/interprocedural/cdfold.hpp:9:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/linear/linform.hpp [-Wmicrosoft-include]
    9 | #include "minijz/opt/linear/linform.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:41:
.\src\minijz/opt/driver.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    5 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    6 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/an.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/analysis/an.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/intervals.hpp [-Wmicrosoft-include]
    8 | #include "minijz/opt/analysis/intervals.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:9:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/analysis/trapelim.hpp [-Wmicrosoft-include]
    9 | #include "minijz/opt/analysis/trapelim.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:10:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/mem2reg.hpp [-Wmicrosoft-include]
   10 | #include "minijz/opt/mem2reg.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:11:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/scalar/fold.hpp [-Wmicrosoft-include]
   11 | #include "minijz/opt/scalar/fold.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:12:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/scalar/copyprop.hpp [-Wmicrosoft-include]
   12 | #include "minijz/opt/scalar/copyprop.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:13:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/scalar/canon.hpp [-Wmicrosoft-include]
   13 | #include "minijz/opt/scalar/canon.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:14:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/scalar/cse.hpp [-Wmicrosoft-include]
   14 | #include "minijz/opt/scalar/cse.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:15:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/scalar/unreach.hpp [-Wmicrosoft-include]
   15 | #include "minijz/opt/scalar/unreach.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:16:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/scalar/splice.hpp [-Wmicrosoft-include]
   16 | #include "minijz/opt/scalar/splice.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:17:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/scalar/dce.hpp [-Wmicrosoft-include]
   17 | #include "minijz/opt/scalar/dce.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:18:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/scalar/ifconv.hpp [-Wmicrosoft-include]
   18 | #include "minijz/opt/scalar/ifconv.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:19:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/loop/licm.hpp [-Wmicrosoft-include]
   19 | #include "minijz/opt/loop/licm.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:20:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/loop/unroll.hpp [-Wmicrosoft-include]
   20 | #include "minijz/opt/loop/unroll.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:21:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/loop/lcf.hpp [-Wmicrosoft-include]
   21 | #include "minijz/opt/loop/lcf.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:22:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/loop/lcf_rt.hpp [-Wmicrosoft-include]
   22 | #include "minijz/opt/loop/lcf_rt.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:23:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/linear/reassoc.hpp [-Wmicrosoft-include]
   23 | #include "minijz/opt/linear/reassoc.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:24:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/linear/divmagic.hpp [-Wmicrosoft-include]
   24 | #include "minijz/opt/linear/divmagic.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:25:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/interprocedural/callgraph.hpp [-Wmicrosoft-include]
   25 | #include "minijz/opt/interprocedural/callgraph.hpp"
      |          ^
.\src\minijz/opt/driver.hpp:26:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/interprocedural/cdfold.hpp [-Wmicrosoft-include]
   26 | #include "minijz/opt/interprocedural/cdfold.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:42:
.\src\minijz/back/stack_alloc.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/instr.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:43:
.\src\minijz/back/vm.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    5 | #include "minijz/ir/instr.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:44:
.\src\minijz/back/emit_c.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    5 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/back/emit_c.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/back/stack_alloc.hpp [-Wmicrosoft-include]
    6 | #include "minijz/back/stack_alloc.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:45:
.\src\minijz/back/emit_x64.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/graph_lib.hpp [-Wmicrosoft-include]
    6 | #include "minijz/graph_lib.hpp"
      |          ^
.\src\minijz/back/emit_x64.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    7 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/back/emit_x64.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    8 | #include "minijz/ir/instr.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:46:
.\src\minijz/back/bytecode.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    5 | #include "minijz/ir/instr.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:47:
.\src\minijz/base/native_run.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/config.hpp [-Wmicrosoft-include]
    7 | #include "minijz/config.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:48:
.\src\minijz/runtime/astrun.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/front/ast.hpp [-Wmicrosoft-include]
    5 | #include "minijz/front/ast.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:49:
.\src\minijz/prog/program.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/front/parser.hpp [-Wmicrosoft-include]
    5 | #include "minijz/front/parser.hpp"
      |          ^
.\src\minijz/prog/program.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/builder.hpp [-Wmicrosoft-include]
    6 | #include "minijz/ir/builder.hpp"
      |          ^
.\src\minijz/prog/program.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/opt/driver.hpp [-Wmicrosoft-include]
    7 | #include "minijz/opt/driver.hpp"
      |          ^
.\src\minijz/prog/program.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/destruct.hpp [-Wmicrosoft-include]
    8 | #include "minijz/ir/destruct.hpp"
      |          ^
.\src\minijz/prog/program.hpp:9:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/back/stack_alloc.hpp [-Wmicrosoft-include]
    9 | #include "minijz/back/stack_alloc.hpp"
      |          ^
.\src\minijz/prog/program.hpp:10:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/runtime/astrun.hpp [-Wmicrosoft-include]
   10 | #include "minijz/runtime/astrun.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:50:
.\src\minijz/prog/runners.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/ir/instr.hpp [-Wmicrosoft-include]
    5 | #include "minijz/ir/instr.hpp"
      |          ^
.\src\minijz/prog/runners.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/back/vm.hpp [-Wmicrosoft-include]
    6 | #include "minijz/back/vm.hpp"
      |          ^
.\src\minijz/prog/runners.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/back/emit_c.hpp [-Wmicrosoft-include]
    7 | #include "minijz/back/emit_c.hpp"
      |          ^
.\src\minijz/prog/runners.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/back/emit_x64.hpp [-Wmicrosoft-include]
    8 | #include "minijz/back/emit_x64.hpp"
      |          ^
.\src\minijz/prog/runners.hpp:9:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/back/bytecode.hpp [-Wmicrosoft-include]
    9 | #include "minijz/back/bytecode.hpp"
      |          ^
.\src\minijz/prog/runners.hpp:10:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/program.hpp [-Wmicrosoft-include]
   10 | #include "minijz/prog/program.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:51:
.\src\minijz/apps/demo.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/program.hpp [-Wmicrosoft-include]
    5 | #include "minijz/prog/program.hpp"
      |          ^
.\src\minijz/apps/demo.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/runners.hpp [-Wmicrosoft-include]
    6 | #include "minijz/prog/runners.hpp"
      |          ^
.\src\minijz/apps/demo.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/runtime/astrun.hpp [-Wmicrosoft-include]
    7 | #include "minijz/runtime/astrun.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:52:
.\src\minijz/apps/fuzz_gen.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/program.hpp [-Wmicrosoft-include]
    5 | #include "minijz/prog/program.hpp"
      |          ^
.\src\minijz/apps/fuzz_gen.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/runners.hpp [-Wmicrosoft-include]
    6 | #include "minijz/prog/runners.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:53:
.\src\minijz/tests/cx_tours.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/program.hpp [-Wmicrosoft-include]
    6 | #include "minijz/prog/program.hpp"
      |          ^
.\src\minijz/tests/cx_tours.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/runners.hpp [-Wmicrosoft-include]
    7 | #include "minijz/prog/runners.hpp"
      |          ^
.\src\minijz/tests/cx_tours.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/apps/fuzz_gen.hpp [-Wmicrosoft-include]
    8 | #include "minijz/apps/fuzz_gen.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:54:
.\src\minijz/apps/fuzz_run.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/pass_cfg.hpp [-Wmicrosoft-include]
    6 | #include "minijz/base/pass_cfg.hpp"
      |          ^
.\src\minijz/apps/fuzz_run.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/program.hpp [-Wmicrosoft-include]
    7 | #include "minijz/prog/program.hpp"
      |          ^
.\src\minijz/apps/fuzz_run.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/runners.hpp [-Wmicrosoft-include]
    8 | #include "minijz/prog/runners.hpp"
      |          ^
.\src\minijz/apps/fuzz_run.hpp:9:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/runtime/astrun.hpp [-Wmicrosoft-include]
    9 | #include "minijz/runtime/astrun.hpp"
      |          ^
.\src\minijz/apps/fuzz_run.hpp:10:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/back/bytecode.hpp [-Wmicrosoft-include]
   10 | #include "minijz/back/bytecode.hpp"
      |          ^
.\src\minijz/apps/fuzz_run.hpp:11:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/native_run.hpp [-Wmicrosoft-include]
   11 | #include "minijz/base/native_run.hpp"
      |          ^
.\src\minijz/apps/fuzz_run.hpp:12:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/apps/fuzz_gen.hpp [-Wmicrosoft-include]
   12 | #include "minijz/apps/fuzz_gen.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:55:
.\src\minijz/apps/cli.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/program.hpp [-Wmicrosoft-include]
    5 | #include "minijz/prog/program.hpp"
      |          ^
.\src\minijz/apps/cli.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/runners.hpp [-Wmicrosoft-include]
    6 | #include "minijz/prog/runners.hpp"
      |          ^
.\src\minijz/apps/cli.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/back/bytecode.hpp [-Wmicrosoft-include]
    7 | #include "minijz/back/bytecode.hpp"
      |          ^
.\src\minijz/apps/cli.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/native_run.hpp [-Wmicrosoft-include]
    8 | #include "minijz/base/native_run.hpp"
      |          ^
.\src\minijz/apps/cli.hpp:9:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/apps/demo.hpp [-Wmicrosoft-include]
    9 | #include "minijz/apps/demo.hpp"
      |          ^
.\src\minijz/apps/cli.hpp:10:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/apps/fuzz_run.hpp [-Wmicrosoft-include]
   10 | #include "minijz/apps/fuzz_run.hpp"
      |          ^
.\src\minijz/apps/cli.hpp:11:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/tests/cx_tours.hpp [-Wmicrosoft-include]
   11 | #include "minijz/tests/cx_tours.hpp"
      |          ^
In file included from main.cpp:8:
In file included from .\src/minijz.hpp:56:
.\src\minijz/apps/shrink.hpp:5:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/apps/cli.hpp [-Wmicrosoft-include]
    5 | #include "minijz/apps/cli.hpp"
      |          ^
.\src\minijz/apps/shrink.hpp:6:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/program.hpp [-Wmicrosoft-include]
    6 | #include "minijz/prog/program.hpp"
      |          ^
.\src\minijz/apps/shrink.hpp:7:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/prog/runners.hpp [-Wmicrosoft-include]
    7 | #include "minijz/prog/runners.hpp"
      |          ^
.\src\minijz/apps/shrink.hpp:8:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/runtime/astrun.hpp [-Wmicrosoft-include]
    8 | #include "minijz/runtime/astrun.hpp"
      |          ^
.\src\minijz/apps/shrink.hpp:9:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/back/bytecode.hpp [-Wmicrosoft-include]
    9 | #include "minijz/back/bytecode.hpp"
      |          ^
.\src\minijz/apps/shrink.hpp:10:10: warning: #include resolved using non-portable Microsoft search rules as: .\src\minijz/base/native_run.hpp [-Wmicrosoft-include]
   10 | #include "minijz/base/native_run.hpp"
      |          ^

