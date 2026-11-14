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
  base/                  pass_cfg (PassCfg + the 33-name toggle table +
                        the runtime-only pass-snapshot hook), native_run
                        (find_cc/slurp/run_capture/write_file — with the
                        Windows portability helpers mjz_devnull/mjz_timeout)
  front/                 lexer, ast, parser, diag (located diagnostics)
  ir/                    instr (IR core), pred_norm, phi_norm (the
                        phis-first invariant), builder, destruct,
                        stats (SsaStats/DestrStats)
  opt/analysis/          an (dominators/loops/SSA), alias, intervals, trapelim
  opt/mem2reg.hpp        array memory -> SSA registers + dead arrays
  opt/scalar/            fold, copyprop, canon (incl. notcomb), cse, gvn
                        (value numbering: consts + phi congruence + bins),
                        unreach, splice, dce, ifconv, taildup (checkpoint
                        20: small 2-pred tails duplicated into their arms)
  opt/loop/              licm, unroll (the slim driver) + unroll_impl
                        (checkpoint 25: the scan tables, the shape
                        matchers, the trip simulator, the safety scan,
                        the emitters + rewire tail), lcf (the slim
                        driver) + lcf_impl (checkpoint 24: the scan
                        tables, the per-phase shape matchers, the plan
                        gates, the emitters + rewire tail), lcf_sim,
                        lcf_rt, unswitch (+ unswitch_shape: the shared
                        shape/matcher/clone layer, unswitch_bif: the
                        body-if scan + commit), rotate (checkpoint 22:
                        loop rotation proper)
  opt/linear/            linform, reassoc, divmagic
  opt/interprocedural/   callgraph (Tarjan + inliner), scctco, scctco_fold,
                        tailrec (tce_self/tce_merge), cdfold (the slim
                        driver) + cdfold_impl (checkpoint 23: the CdAff
                        algebra, the resolver, the per-phase matchers, the
                        two-buffer emitter — every function small)
  opt/driver.hpp         the pass manager (tiered fixpoint, -O0..-O3)
  opt/optimize_api.hpp   the light DECLARATION of optimize()/optimize_light()
  back/                  vm (bytecode: opcodes + emit_bytecode) +
                        vm_step (steppable VmCtx + vm_run) +
                        interp_ssa (the SSA interpreter) — the vm tripart,
                        bytecode, emit_plan (EmitPlan), stack_alloc,
                        emit_c (+ emit_c_impl), emit_x64 (+ emit_x64_impl:
                        codegen) + regalloc (the chordal allocator) +
                        regaudit (+ regaudit_impl: the --reg-audit table)
  runtime/astrun.hpp     the AST oracle (reference semantics)
  prog/                  program (LIGHT types + the compile() boundary),
                        compile_impl (the pipeline body), printers (IR/
                        runners pretty-printers), runners (umbrella)
  apps/                  cli_parse (LIGHT options), cli_run (reports/matrix/
                        repl), app_api (the extern app boundary),
                        app_main (the shared main body), demo, fuzz_gen,
                        fuzz_run (parallel differential fuzzer), shrink,
                        debugger (the slim session loop + dispatch;
                        checkpoint 23 finished the split: dbg_sess the
                        session state + watch/stop engines, dbg_cmds the
                        command families, dbg_opt the pass explorer,
                        dbg_vm the machine half, dbg_support the
                        machines/breakpoint-scan/naming/snapshot layer)
  tests/cx_tours.hpp    the constexpr/compile-time tour battery
src/minijz.hpp         umbrella (single-TU / constexpr build)
src/tu/*.cpp           the multi-cpp translation units (9 REAL TUs)
tests/                  constexpr_check.cpp (cx proofs), unit_tests.cpp
examples/ demos/        showcase programs (+ general.in input tape), BUGFIXES.md;
                        examples/audit/ = 20 natural programs + RESULTS.md
                        (the register-allocation audit)
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
# cmake --preset windows-llvm  ..
#cmake --build build-win -j  
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
  `--preset vs-clang` (VS 2022 generator + the clang-cl toolset),
  `--preset vs-msvc` (VS 2022 + plain cl.exe), `--preset windows-gcc`
  (MinGW Makefiles + g++, for MSYS2 / w64devkit). They carry a host
  condition, so `cmake --list-presets` only offers them on Windows.
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
   toolset, for machines with VS installed), `cmake --preset vs-msvc`
   (VS 2022 + plain cl.exe), `cmake --preset windows-gcc` (MinGW + g++).
3. Manual equivalent of the first recipe:
   `cmake -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=clang++ -S . -B build`.
4. Or: install VS Build Tools and configure from an "x64 Native Tools"
   prompt so `nmake`/`CL` exist and even the default generator works.

PowerShell note: chain steps with `;` (`&&` needs PowerShell 7+).
clang-cl is a first-class config now: every quoted include resolves in
its includer's own directory (see warnings.txt), so the old
`-Wmicrosoft-include` noise is gone — only genuinely portable includes
remain (`include_portability` is a ctest `regr` gate).

### Checkpoint 16: the build itself is now Windows-clean

The "it doesn't compile on Windows" report had four distinct causes,
all fixed:

1. **MSVC dialect**: the constexpr-proof budgets now spell themselves
   per compiler (`/constexpr:depth8192 /constexpr:steps4000000000` for
   cl vs `-fconstexpr-*` for the GNU drivers); `/utf-8` is added for
   MSVC (the comments are UTF-8); the cx ctest checks build as
   object-library targets under the VS generators (so cl gets its
   INCLUDE/LIB environment set up by the generator instead of a raw
   `cl` that cannot even find `<stdint.h>`); and the clang++
   auto-preference now stays out of the way of Visual Studio generators
   (which pass MSVC-style flags that the GNU clang++ driver rejects).
2. **libc++ `std::atomic_ref`**: upstream mjz_lib references it
   unconditionally, and libc++ never shipped it (it was removed from
   the final C++20 text) — so EVERY libc++ toolchain (LLVM-MinGW, MSYS2
   CLANG64, `clang++ -stdlib=libc++`) died inside the import. The
   committed bridge `src/mjz_lib/alllib_import.hpp` now carries a
   gated single-threaded stand-in (sound here: the compiler never
   shares a refcounted object across threads) that switches itself off
   if libc++ ever grows the real one.
3. **MSVC-hostile builtins**: `__builtin_popcount`/
   `__builtin_clzll` became the C++20 `<bit>` `std::popcount`/
   `std::countl_zero` (identical semantics, every compiler).
4. **Shell dialect** in the native-parity harness: `command -v` →
   `where`, `/dev/null` → `nul`, `timeout 60 ...` (a coreutils killer)
   → dropped on Windows (cmd's `timeout` is a SLEEP), `mkdir -p` →
   `std::filesystem::create_directories`. The five bash-based ctest
   gates register only when bash exists (Git-Bash/WSL/MSYS all work),
   so a bash-less Windows box gets a clean `ctest` instead of five
   failures.

Verified by cross-compiling in CI style: all 9 TUs + the header-only
`main.cpp` + `tests/unit_tests.cpp` compile AND LINK into a PE32+
x86-64 `minijz.exe` for `x86_64-windows-gnu` (the LLVM-MinGW/CLANG64
class of toolchains — the strictest portability case, libc++
included). The MSVC path follows the documented cl option syntax; both
new presets (`vs-msvc`, `windows-gcc`) cover the remaining toolchain
families.

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

## `--reg-audit` — the register-allocation table

Prints the x86-64 register-allocation audit after the compile report:
one row per surviving fn — reachable blocks, vregs, the values housed
in the five callee-saved registers (rbx/r12-r15), the spill slots, the
reload/store spill traffic, the registers pushed. The plan is the SAME
chordal coloring the codegen gets (one allocator, two consumers), the
traffic counts are the exact `movq -N(%rbp),..` counts of the emitted
asm, and the twenty natural programs in `examples/audit/` (with their
committed `RESULTS.md`, regenerable via `scripts/regen_audit_results.py`)
exercise it across the pressure classes. Checkpoint 22 refreshed the
table after loop rotation: 710 vregs, 573 housed, 137 spill slots,
424 spill traffic — the rotation's edge copies are visible in the audit
(the FIR filter went spill-free to 8 slots; the section-8 ledger has
the reading).

## `--debugger` — the interactive facility

An always-answering session over the compiled program; no timeouts, no
waiting — every command dumps state instantly. Commands come from stdin,
so it is scriptable (`printf 'vm step 10\nvm state\nquit\n' | ./minijz
f.mjz --debugger`) and there is a scripted session in the ctest `regr`
gates. The tape comes from `--stdin=FILE` (the debugger reads COMMANDS
from stdin).

```
help                    command list
src [N]                 numbered source (the line numbers feed `vm line`)
fns / stats             function table, pipeline statistics
ir [FN] [ssa|opt|dst|all]   dump IR at any stage
cfg [FN]                CFG: blocks/preds/succs (main: + mjz_lib analysis)
plan [FN]               emission plan (order, next, dead, fused)
bc [FN]                 bytecode listing
bkpts                   the program's language breakpoints (`breakpoint`
                        statements): pc, fn, source line, observed names
vars [FN]               named slots with live values — what survived
                        optimization (observed values included)
watch NAME / unwatch    show a variable at every stop; `vm continue`
                        ALSO stops when a watched value changes
opt [FN]                pass explorer: re-run the pipeline on a copy with
                        a snapshot after EVERY pass; prints the timeline
opt show [N]            full IR of snapshot N
opt diff [A [B]]        unified IR diff between snapshots (names or
                        indices; default first-vs-last)
opt break PASS          a breakpoint BETWEEN passes: PASS's snapshot +
                        its diff against the previous one — and ARMED:
                        the next `opt [FN]` PAUSES the timeline after it
opt continue            disarm the pass breakpoint + collect in full
opt list                the 33-name pass toggle table, on/off
check                   all five runners + comparison
vm reset/step/run/continue/until/break/line/unbreak/breaks/state/dis
                        the steppable machine: single-op tracing WITH
                        source-line annotations, pc breakpoints, SOURCE
                        LINE breakpoints (`vm line L`), aligned
                        disassembly, and `vm state` = EVERYTHING (pc+fn,
                        frames+slots, operand stack, arena, input
                        position, output)
quit
```

`vm continue` stops automatically at every language breakpoint and
prints the observed values (scalars as numbers, array handles as arena
dumps); the stop is BEFORE the op executes, and `continue` resumes
through it (classic debugger semantics). `opt` snapshots cover the
intra-fn pipeline; the interprocedural passes run program-wide around
it.

The VM half runs on the `VmCtx` stepper (`back/vm.hpp`); `vm_run()` is a
thin loop over `vm_step_op()` with identical semantics (the constexpr
proofs still run the VM at compile time).

## The `breakpoint` statement — debugging optimized builds

`breakpoint;` or `breakpoint x, y, arr;` — an observation point. It is
a NO-OP for program output in every runner (oracle, all IR stages, the
bytecode VM, emitted C and asm: full native parity holds — the fuzzer
sprinkles breakpoints into every new-seed program since the
`MJZ_BKPT_SEED_FLOOR` window). But the observed values are USES:

* DCE treats the point as a root — it survives `-O3` (loop unrolling
  duplicates it per iteration; each stop shows that iteration's values);
* `mem2reg` never lifts an observed array to registers — the allocation
  stays in the arena so the debugger can dump its contents (and the C
  backend keeps it out of stack-array form for the same honesty);
* `deadargs` counts an observed parameter as a hard use — it stays in
  the signature;
* const-call/pure-call-CSE treat a function containing one as impure —
  the call (and its observation points) never fold away.

The optimizer may still rewrite WHICH vreg holds the value (copy-prop
into temps, constants folded); the debugger resolves names through the
pre-opt SSA statement, and shows the value the optimized program
actually has at the stop — that is the point: even optimized builds
remain debuggable. `--opt-diff[=FN]` gives the same pass-diff view at
the CLI level.

## Located diagnostics (compile errors + runtime traps)

Compile errors render with line:col, the source line, and a caret, and
quote the offending token (`front/diag.hpp`):

```
prog.mjz:3:9: error: use of undeclared variable 'zzz'
    3 |   print zzz + 1;
      |       ^~~
prog.mjz:2:7: error: call to 'f' passes 1 argument but takes 2
```

Runtime traps from the interpreters carry the trapping instruction's
source offset and a category — `TRAP (division by zero at 7:3)`,
`TRAP (index out of bounds at 2:8)` — while the parity contract (the
exact `TRAP\n` text native builds print) stays byte-identical.

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
  thread and the redundant branch vanishes)**, and (checkpoint 16)
  **global value numbering (`gvn` — one dominator-scoped value-number
  table unifying duplicate Const defs, congruent merge phis of the same
  block (same incoming map), and pure-Bin recomputation with cse's exact
  rules; mutually-referential phi pairs are left to phi-degen/sccp on
  purpose)**, and (checkpoint 17) **trivial loop unswitching (`unswitch`,
  toggle #30 — a loop whose header branches on an SSA-proven
  loop-invariant condition is versioned on it: the preheader branches,
  the false path jumps straight to the exit, and the true path is a
  fresh-vreg clone of the loop with the test gone; internal `break`
  edges are kept and every exit block's phis rekeyed, and uses of header
  phis on the false path go through fresh merge phis at the exit)**
  plus **loop canonicalization (`lsimp`, toggle #31, and the licm header
  scan — the while-lowering computes the loop condition IN the header,
  so `while (f > 0)` with f never written inside leaves an invariant
  test there; licm now scans the header too (duplicating in-loop Const
  operands into the preheader) which both saves a compare per iteration
  and is the classic unswitch enabler; lsimp dissolves trivial
  loop-carried phis (x' = x) to their entry values)**. The same
  checkpoint fixed two LATENT bugs the new fuzz bait exposed: unroll and
  lcf computed trip counts with continue-style polarity only, so loops
  whose header test is an EXIT condition (a constant while-test folded
  away, splice merged the `break` block up) got zero-trip counts; and
  licm treated `nt` (trap-impossible PROOFS) as a hoist license even
  though the proof is control-flow-sensitive (`z != 0 && 42 / z` is only
  safe UNDER the guard) — now only a constant divisor outside {0, -1}
  hoists. And (checkpoint 18) **compound-condition loop unswitching**
  (the same `unswitch` toggle, extended): the while-lowering spreads
  `while (a && b)` / `while (a || b)` over a diamond — the header tests
  a, the elbow tests b, the 1/0 blocks feed the merge whose phi+cbr IS
  the loop's exit edge. When BOTH leaf tests are invariant, the whole
  guard chain is versioned as one (`&&`: pre: cbr a -> g2: cbr b ->
  clone / fx: br exit; `||`: the true arms short-circuit into the loop
  through a dedicated entry block), the true clone bypasses the entire
  diamond straight into the body, and the value branch's exit edge
  becomes the zero-iteration edge (exit phis keyed to it take the header
  phis' ENTRY values, keyed to the new false-side block). The diamond
  blocks must be pure (Const/Copy/Bin, nt on Div/Mod) — skipped side
  effects on the zero-iteration path would be a miscompile, and the
  invariance checks already reject anything that computes the conditions
  in-loop. Checkpoint 18's code-quality move: `opt/loop/lcf.hpp`
  (previously ~1150 lines) now splits into the driver + `lcf_sim.hpp`
  (the general any-condition simulator) + `lcf_rt.hpp` (the runtime-trip
  guarded closed form) — and the const-call evaluator, which
  historically squatted in `loop/lcf_rt.hpp` under a stale header,
  moved to its real home `interprocedural/constcall.hpp`. And
  (checkpoint 19) **body-if loop unswitching** (the classic LLVM
  "trivial" unswitch of an invariant INNER branch — still the same
  `unswitch` toggle): the head test is a REAL while-condition, but a
  body branch `bif: cbr(INV) A/B` with INV SSA-proven invariant, both
  arms in-loop and each entered only through bif, versions the loop —
  `pre: cbr(INV) h / h'`, the original loop becoming the INV-true
  version (`bif: br A`, B dies) and a full fresh-vreg clone the false
  one (`bif': br B'`, A' dies). BOTH versions keep running the loop —
  sound because the original picked the SAME arm every iteration (INV
  is an SSA value, it cannot change mid-loop) — so no purity gates
  beyond INV's invariance are needed. Every exit edge (the header's
  exit arm INCLUDED — no zero-iteration edge is replaced) gets a
  VERSION-MERGE block: preds {exit source, its clone}, one phi per
  loop-defined value read at or through the exit target
  (phi(v, fresh v)), then br to the target; that holds the <=2-preds
  invariant the 2-arg phi representation needs. Outside reads of
  loop-defined values are routed by a DELIVERED walk: pure break arms
  read the VM directly, single-pred chains pass the value up, and a
  2-pred merge whose values arrive per-pred (the post-loop print when
  a loop has both a header exit and a guarded break — the SSA builder
  left those as PLAIN reads because the header phi dominates the whole
  post-loop region) gets a fresh phi born at the merge. The dead arms
  and their merge phis degenerate under unreach + copyprop in the same
  tier round, the way the checkpoint-17/18 abandoned loop bodies do.
  The new bait exposed (and this checkpoint fixes) a LATENT bug in the
  LCF general simulator: it evaluated the header test with
  continue-style polarity only (exit on FALSE), so a break-reshaped
  loop (the versioned true loop, after entry-threading + splice, can
  carry the `break` condition AS its header test — exit on TRUE) folded
  to a zero-trip closed form through the WRONG exit edge; the
  simulator now takes the loop's own polarity, the way the analytic
  trip counter has since checkpoint 17. Checkpoint 19's code-quality
  move: the shape matchers + the clone machinery the three unswitch
  paths share (the invariant gate, the exit inventory, bmap/fresh
  allocation, the append-only full-loop clone emission) extract into
  `opt/loop/unswitch_shape.hpp`, and the body-if scan + commit into
  `opt/loop/unswitch_bif.hpp` — unswitch.hpp becomes the slim driver.
  Checkpoint 20 lands the wishlist's named next step, **tail
  duplication** (toggle #32, tier 1, `opt/scalar/taildup.hpp` — the
  degenerate guarded unswitch the checkpoint-19 notes pointed at): a
  SMALL block T shared by exactly two predecessors, each ending in a
  DEDICATED `br T`, is duplicated into both of them — the merge block
  and its jump disappear from every path. T's phis do not duplicate,
  they MOVE: each continuation U (preds == {T} exactly, so after the
  duplication U's preds are exactly the two arms) gets one phi per
  escaping value; routing is per-USE (a value can escape through BOTH
  continuations at once — one arm's region reads it through U0, the
  other's through U1), and a 2-pred re-merge whose arms arrive from
  different suppliers gets a phi BORN there — the checkpoint-19
  delivered-walk discipline, one nesting level per re-merge (the
  in-loop if/else whose merged value feeds the header phi through
  the post-arm join). The gates: T is not a loop header (a header
  whose body duplicated into its own latch would be loop ROTATION
  proper — this tree's loop consumers all match the canonical
  test-first shape, so the merge whose continuation IS the header —
  the latch merge — stays unduplicated by design); T's body <= 12
  instrs of Const/Copy/Bin/Print (a Print printed once per path
  before prints once per path after; a may-trap Div/Mod is fine for
  the same reason); terminator Br/Cbr/Exit/Ret (the fn tail
  duplicated into both arms — no routing); conditional preds stay
  unduplicated (they would need edge blocks this IR does not have).
  The correlated-merge jackpot: `if (c) x=1; else x=9; y=x+5`
  duplicates y=x+5 into both arms where tier-0 folding pre-resolves
  it to 6 and 14 — the merge then selects between two constants.
  One duplication per call; the fixpoint driver re-analyzes. The
  checkpoint ALSO fixes a LATENT miscompile the new probes exposed
  (reproducible on checkpoint 19, missed by the fuzzer because its
  loops carry per-iteration prints the LCF purity gate rejects):
  `lcf_rt`'s guard read the bound vreg from the preheader when its
  defining Const still lived INSIDE the loop header — licm only
  duplicates such a Const out when it hoists an instruction reading
  it, and the header compare is never hoisted (its phi operand is
  loop-carried) — a use-before-def that destruction's register
  allocator then coalesced into garbage (`let i = input(); while
  (i < 40) { i = i + 1; s = s + 2; }`: oracle 817, compiled 3); the
  guard now materializes a fresh local copy of an in-loop Const bound
  (the same move licm's dup_const makes — a Const's value is
  position-independent). And the recurrence chains: `linform` grows
  an ANALYSIS mode — the lcf recurrence scan only READS the linear
  form (the whole loop is deleted when the closed form commits), so
  a shared def absorbs there even though reassociation must keep
  its absorbed defs single-use — which lets the MULTI-USE induction
  update absorb and turns `while (i < B) { s = s + i; i = i + 1; }`
  into the sum-of-series recurrence s' = s + i + 1: the b != 0
  evolutions the analytic T(n) series AND the runtime Tv
  construction were carrying since checkpoint 18 but could never
  fire (the single-use restriction made every induction-dependent
  recurrence a non-phi leaf term, and the scan rejected it).
* still open (the standing wishlist): LCSSA
  (needs undef semantics this IR does not have — a loop-exit phi's
  zero-trip edge has no defined value), demanded/known bits,
  MemCPyOpt, loop fusion/fission/distribution, function
  outlining, Attributor-style inference, GlobalOpt. Notes from the
  unswitch/taildup/rotation checkpoints: body-if unswitching is DONE
  (checkpoint 19, above), tail duplication is DONE (checkpoint
  20, above — the degenerate guarded unswitch) and loop rotation
  proper is DONE (checkpoint 22, below — the latch merge the taildup
  gates pointed at, consumed exactly as predicted); the natural next
  step from there is the partial/guarded unswitch family proper
  (version on a loop-VARYING branch whose taken arm is cheap to
  duplicate); compound unswitch
  handles exactly two leaf tests — deeper boolean trees
  (`a && b && c`) stay unversioned.

## 4. make the compiler debuggable (breakpoints, diagnostics, pass diffs) — DONE

The checkpoint-15 additions, per the request:

* **the `breakpoint` statement** (`breakpoint;` / `breakpoint x, arr;`):
  an observation point that never prints anything but observes the listed
  values so the debugger can stop and step there — **even optimized
  builds** (observed values are USES: DCE roots, arrays stay un-lifted,
  params stay in signatures; see the dedicated README section above).
  Guarded in the optimizer (dce/mem2reg/deadargs hard-use, impure for
  constcall/callcse, conservative skips in jthread elbows and the LCF/
  cdfold simulators) and proven by `cx_check_7` at COMPILE TIME plus the
  breakpoint-sprinkling fuzz window (`MJZ_BKPT_SEED_FLOOR`).
* **better error messages**: located compile diagnostics (line:col +
  caret + token quoting via `front/diag.hpp`), the call-arity error now
  names the function, both counts, AND the call site; runtime traps from
  the interpreters carry kind + source offset ("division by zero at
  7:3") with the native-parity `TRAP\n` contract byte-identical.
* **the debugger grew both halves**: program side — automatic stops at
  language breakpoints with observed values, `bkpts`/`vars`/`watch`,
  source-LINE breakpoints (`vm line L`), line-annotated stepping and
  disassembly, numbered `src`; optimization side — the pass explorer
  (`opt` timeline with per-pass snapshots, `opt show`, `opt diff`'s
  unified IR diff between ANY two passes, `opt break PASS` as a
  breakpoint BETWEEN passes — checkpoint 18: it ARMS, and the next
  `opt [FN]` collection actually PAUSES there (the timeline truncates
  after that pass, its diff is shown, `opt continue` collects in full),
  `opt list`), plus the batch `--opt-diff` CLI flag — the diff shower
  between passes, driven by a runtime-only snapshot hook in
  `PassCfg`/`dbg_pass` that never touches the constexpr path.
  (checkpoint 19 adds the scripted `bifunswitch_debugger_session`
  ctest: `opt break unswitch` on the body-if tour demo pauses the
  timeline exactly where the versioning happens, `opt show` dumps the
  version-merge blocks, and the five-way check proves the versioned
  program end to end. Checkpoint 20 adds the `taildup_debugger_session`
  twin: `opt break taildup` on the taildup tour demo pauses after the
  FIRST duplication — the loop body's if/else merge, arms carrying the
  copied tail and the born phi keying the latch — `opt continue`
  collects in full (the fn-tail merge fires in round 2), and the
  five-way check proves the duplicated program end to end.)

## 5. checkpoint 16: Windows builds, GVN, code-quality splits, wider fuzz — DONE

* **the cmake now compiles on Windows** (the report's headline): four
  distinct root causes fixed — the MSVC flag dialect (constexpr budgets,
  /utf-8, object-library cx checks, no clang++ hijack under VS
  generators), the libc++ `std::atomic_ref` hole in upstream mjz_lib
  (gated stand-in in the committed bridge), MSVC-hostile builtins
  (-> C++20 `<bit>`), and the shell dialect of the native-parity
  harness (`where`/`nul`/no coreutils `timeout`/`std::filesystem`
  mkdir). New presets `vs-msvc` and `windows-gcc` join windows-llvm /
  windows-ninja / vs-clang. Verified end to end by cross-compiling all
  9 TUs + the header-only form + unit tests into a PE32+ Windows exe
  (see the Windows toolchains section's checkpoint-16 subsection).
* **GVN proper** (`gvn`, toggle #29): dominator-scoped value numbering
  unifying duplicate Consts, congruent merge phis, and pure-Bin
  recomputation; 14 new unit checks; fuzz rotation case 16 disables it.
* **code quality**: the 1214-line `apps/debugger.hpp` split into
  `dbg_support.hpp` (machines/breakpoint scan/naming/snapshots) +
  `debugger.hpp` (the `DbgSess` session + one small static handler per
  command family + a thin tokenize-dispatch loop); the 675-line
  `run_debugger` is gone. Behavior byte-identical (both scripted
  debugger ctest sessions pass unchanged).
* **fuzzers expanded**: shadow-pair merges planted in every seed >=
  12331 (`MJZ_GVN_SEED_FLOOR`; phi-congruence bait — 91 of the first
  150 new-window programs carry one), rotation now 17-way, the
  random-config mode draws from the 29-name table, and the new windows
  rotation 12331+ / random-cfg 10801+ ran clean (see the battery note
  in the commit message).

## 6. checkpoint 17: loop unswitching, loop canonicalization, debugger watchpoints — DONE

* **trivial loop unswitching** (`unswitch`, toggle #30, tier 2): versioning
  loops on SSA-proven invariant header conditions, `break` edges included
  (exit phis rekeyed to the clones, fresh merge phis carry header-phi
  entry values on the false path). The enabler was the next bullet; the
  bait window is seeds >= 12731 (`MJZ_UNSWITCH_SEED_FLOOR`: invariant-flag
  while loops whose termination rides on a guarded break — 128 of the
  first 300 new-window programs carry one, 55 loops versioned).
* **loop canonicalization**: licm now scans loop HEADERS (the while-lowering
  computes the condition there) and duplicates in-loop Const operands while
  hoisting — the invariant test lands in the preheader, one compare saved
  per iteration. `lsimp` (toggle #31) dissolves trivial loop-carried phis.
* **two latent bugs fixed** (found by the new bait within minutes, both
  reproducible on checkpoint 16): unroll/lcf trip-count polarity for
  break-reshaped headers (constant while-tests fold away, splice merges
  the `break` block up as the header test — the old code read those as
  zero-trip loops), and licm hoisting guard-protected `nt` Div/Mod out of
  their guards (nt proofs are control-flow-sensitive; only constant
  divisors outside {0,-1} hoist now).
* **debugger**: `watch NAME` became a value-change watchpoint (`vm
  continue` stops and prints old -> new when a watched slot changes;
  switching fns re-baselines silently), and `--opt-diff=FN:PASS` filters
  the batch pass-diff viewer to one pass (e.g. `--opt-diff=0:unswitch`).
* **code quality**: `opt/loop/loop_shape.hpp` (the loop-recognition layer
  shared by unroll/lcf/unswitch — scan tables, innermost-first headers,
  the canonical pre/latch/body/exit shape) and `ir/phi_norm.hpp`
  (normalize_phis moved out of lcf.hpp to where its consumers live).
* **fuzz**: the toggle table grew to 31 names (the random-config mode
  draws from it for new seeds; the fixed rotation adds unswitch/lsimp
  off-cases 17/18 for seeds >= the floor only, so every committed
  fixture keeps its exact old config); the rotation window 12731-13030
  and the random-cfg window 12731+ ran clean, native parity included.
  (checkpoint 20 adds case 19 — taildup off, seeds >= 14231; checkpoint
  22 adds case 20 — rotation off, seeds >= 15531 — AND strengthens the
  discipline: the pass joins the fuzz config only from its floor up, so
  the committed fixtures of every older window keep regenerating
  byte-identically — verified this checkpoint on both fz14231+ and
  fz14831+. The table is 33 names now.)

## 7. checkpoint 21: backend modularity, the register-allocation audit — DONE

The backend becomes modular (the checkpoint's two biggest files split,
byte-identical, the full battery + an asm/bytecode corpus diff
protecting every step) and grows a second consumer for the register
allocator — the audit the checkpoint's twenty natural example programs
exercise:

* **`back/vm.hpp` (~1044) → the vm tripart**: `vm.hpp` (the bytecode
  layer — the `Bc` opcodes, bc_name/bc_rd_u32/bc_put_u32/bc_size,
  `emit_bytecode`) now includes `back/vm_step.hpp` (the steppable
  machine: `VmCtx` + `vm_init` + `vm_step_op` + `vm_run` — the
  debugger's live view) and `back/interp_ssa.hpp` (`run_prog`, the SSA
  interpreter with its explicit frame stack). The parent-includes-
  children convention preserves the historical single-file definition
  order exactly, so every includer keeps working unchanged.
* **`back/emit_x64_impl.hpp` (~888) → regalloc + codegen**:
  `back/regalloc.hpp` carries the whole allocator (`RaPlan` + `ra_plan`
  — block liveness, the SSA interference graph, the mjz_lib chordal
  coloring, the 5-register assignment and spill-slot ordinals);
  emit_x64_impl.hpp keeps the per-fn codegen + the program entry and
  includes regalloc at the same position, so behavior is unchanged.
  The backend stack now reads as a pipeline of light plan/impl pairs:
  emit_plan (the emission plan) / regalloc (the allocator) / emit_x64
  (the codegen) / emit_c / vm + vm_step + interp_ssa / regaudit (the
  audit below).
* **`--reg-audit` (the register-allocation audit)**: `back/regaudit.hpp`
  (light declarations + `RegAuditRow`/`RegAuditOut`, the emit_x64.hpp
  pattern) + `back/regaudit_impl.hpp` (compiled once — via the light
  header in the header-only build, via tu/backends.cpp in the multi-TU
  build). The audit re-runs `make_emit_plan` + `ra_plan` — the SAME
  plan the codegen gets, one allocator two consumers — and reports per
  fn: reachable blocks, vregs with a home, the ones HOUSED in the five
  callee-saved registers (rbx/r12-r15), the SPILL SLOTS, and the spill
  traffic — reloads (one per textual use of a spilled vreg, the
  codegen's own dead-skip discipline so dead instructions don't count)
  and stores (one per live def, one per spilled incoming param). The
  accounting is exact: verified by grep against the emitted asm — the
  audit's `movq -N(%rbp),..` counts match the .s file instruction for
  instruction. Printed under `--quiet` too (an explicit flag is an
  explicit ask); the row names print with their OWN length (a
  string_view has no NUL — the %.*s lesson the codegen learned with
  std::string).
* **twenty natural audit programs** (`examples/audit/aa01..aa20` +
  `.in` tapes + `RESULTS.md`): series, Horner, eight-accumulator
  pressure cookers, one-pass statistics, bubble sort, 3x3 matmul, FIR
  filter, Fletcher checksums, gcd/lcm pairs, fib/tribo, three-seed
  Collatz, trial division, digit ciphers, ledger state machines,
  calendar arithmetic, unit-conversion chains, Game-of-Life neighbor
  census, interest schedules, wide RECURSIVE calls (the inliner eats
  constant calls — recursion is what keeps calls real), and deep
  recursion. Every program is input()-driven where the optimizer would
  otherwise fold it to a constant chain — the honest register-allocation
  view of natural code — and the set deliberately spans the pressure
  classes: zero-spill (aa02/07/08/12/17/20) through 22 spills (the
  three-seed Collatz). RESULTS.md is the committed audit table (644
  vregs, 537 housed, 107 spill slots, 200+145 spill traffic across the
  twenty) with the reading guide. The `reg_audit_examples` ctest runs
  all twenty through `--check-all --reg-audit`: a runner mismatch exits
  nonzero, and a missing audit table is itself a failure.
* **fuzz**: the rotation window 14831-15130 (native parity included, 20
  fixtures committed as fuzz_native/fz14831..14851) and 15131-15430,
  the random-cfg window 14231-14430, and the bait window 14231-14530
  re-verified — all clean.

## 8. checkpoint 22: loop rotation proper — DONE

The wishlist's named next step (the taildup gates pointed at the latch
merge since checkpoint 20) lands as `rotate` (toggle #33, tier 2, the
LAST loop consumer — `opt/loop/rotate.hpp`):

* **the transformation**: the canonical test-first loop becomes
  test-last with a one-shot guard. The whole non-phi h-body MOVES
  (never just copies) to the two edges — a guard copy in the
  preheader (the 1st header run) and an edge test in the latch (the
  (i+1)-th header run) — so every side effect keeps its exact count
  and order: k trips means k+1 header runs in both forms, and the
  guard copy IS the 1st while the latch copy of iteration i is the
  (i+1)-th (Prints, Inputs, Calls, Div/Mod traps and Bkpt
  observations all fire identically; nothing sits between a latch
  end and the next header top). The header keeps its phis with keys
  AND args unchanged — pre still enters h and the latch still
  branches back, so nothing re-keys. Loop-internal readers of a
  moved def get a fresh header phi carrying (guard copy, latch
  copy) — exactly the (pre, latch) pattern the original phis use;
  the after-loop region reads them through fresh exit phis carrying
  (entry value, back-edge value) — the same pair each header phi
  merges, relocated past the loop (the exit phi of an h-phi
  literally re-uses the header phi's arg pair). One control
  transfer fewer per iteration (the header cbr + the latch br fuse
  into the latch cbr).
* **the soundness envelope** (the strict unroll/lcf family):
  canonical shape, unique pre + latch (no self-loops), dedicated
  `br h` preheader, the ONLY exit edge is the header's (no breaks)
  with preds(X) == {h}, h-body <= 8 instrs, outside uses of
  loop-defined values only through the HDEF set (h-phis + h-body
  defs) and only in the post-loop region — the guard reads only
  values dominating the preheader (the checkpoint-20 lcf-rt
  use-before-def lesson is structurally impossible here).
* **the checkpoint's own regression, found by the selftest**: the
  `while(1) + break` showcase folds so the increment lives IN the
  header — an h-phi whose latch arg IS a moved h-body def. Three
  phi paths needed routing through the moved def's fresh header
  phi (the phi's own latch arg, the latch copy's phi read, the
  exit phi's latch-side arg) — plus the classic copy-dst rewrite
  the checkpoint-20 landmine list already warned about (the copies
  kept the original dst while the cbrs read the fresh vregs: both
  defined the same vreg, the tests read garbage). The unit series
  pins the shape (c) with the exact program.
* **the register-audit interaction** (what the checkpoint-21 audit
  was built for): rotation trades register pressure for the
  per-iteration branch. The twenty natural programs' table was
  REGENERATED (`scripts/regen_audit_results.py`, now committed) —
  644 vregs/537 housed/107 spill slots/345 traffic became
  710/573/137/424: the guard + latch-edge copies and the routing
  phis add live values at the loop edges (the FIR filter went from
  spill-free to 8 slots — the measured price of one fewer control
  transfer per iteration; the horner/checksum/trial-division/Life/
  deep-recursion programs stay spill-free).
* **fuzz**: the rotation window 15531-15830 (143 rotations, 121
  rotation-bait loops — runtime-clamped start + impure body, the
  vocabulary that survives every earlier consumer; native parity
  included, 20 fixtures committed as fuzz_native/fz15531..15550)
  and 15831-16130 (167 rotations), the re-verified windows
  14831-15130 and 14231-14530 (both byte-stable: the below-floor
  gate), random-cfg 14431-14730 and 15531-15830 — all clean.

## 9. checkpoint 23: the code-quality continuation (function decomposition + the debugger/cdfold file splits) — DONE

The CONTINUE backlog's item 4, consumed: "break the large functions
(each ~10 lines unless it genuinely needs more room), break the big
files" — applied to the two next-biggest files the backlog named.
Pure refactor, zero behavior change, byte-identical outputs.

* **cdfold** (`opt/interprocedural/cdfold.hpp` 809 lines with ONE
  642-line function -> a 33-line slim driver + `cdfold_impl.hpp`,
  the support layer): the CdAff algebra (cd_aff_eq/cd_term) and the
  bounded resolver (cd_resolve, decomposed into cd_resolve_bin /
  cd_resolve_phi / cd_aff_mul / cd_aff_addsub); a shared cd_def_site
  replaces TEN inline copies of the last-def scan — with the scan
  bound passed EXPLICITLY, because the difference is load-bearing
  (the resolver sees the CURRENT block count, the shape matchers the
  PASS-ENTRY one; the closed-form blocks earlier folds append are
  deliberately invisible to the matchers); the 642-line matcher
  became 20+ small constexpr functions, one per phase, in the old
  order (cd_membership, cd_loop_edges, cd_collect_hphis,
  cd_classify_latches, cd_find_selector, cd_find_countdown,
  cd_classify_adds, cd_classify_exits with cd_cbr_np_zero /
  cd_cbr_bh_zero_trap over a shared cd_eq_zero_vreg, cd_check_xblocks
  with cd_x_ret/cd_x_br/cd_x_uses_ok, cd_sel_sound with entry/toggle
  proofs, cd_body_pure); the fold became the CdGen two-buffer emitter
  (cd_emit_guard/parity/toggle/triangle/add_one, cd_rewrite_uses,
  cd_patch_pre, cd_cf_terminator, cd_splice_pre, cd_append_cf,
  cd_sever_loop) orchestrated by cd_apply — the vreg ALLOCATION ORDER
  (the nt()/konst() call sequence) is preserved exactly, so the
  emitted IR is byte-identical. The dead state the original
  accumulated (kval, Xt/Xf, the const*const re-resolve) dropped.
* **debugger** (`apps/debugger.hpp` 970 lines -> 171, plus four new
  focused files): `dbg_sess.hpp` the DbgSess session state with the
  watchpoint engine (watch_step 68 lines -> watch_slot_of /
  watch_last / watch_rebaseline / watch_report / watch_refresh) and
  the stop engine (run_bounded 61 -> stop_at + stop_report + a slim
  loop; the manual-bp message keeps its two shapes — with/without the
  ops count — via the ops argument); `dbg_cmds.hpp` the eleven
  command families (the ir dump lambda, the vars enrichment, the
  bkpt observation builder, the watch add/del paths — each a named
  function); `dbg_opt.hpp` the pass explorer (dbg_cmd_opt 158 ->
  dbg_snap_idx + show/diff/break/continue/list/collect, each small;
  the pause report its own function); `dbg_vm.hpp` the machine half
  (dbg_cmd_vm 143 -> per-subcommand functions; the traced single
  step — trace line, stack top, breakpoint observation — is
  dbg_vm_step_one + dbg_vm_show_bkpt); debugger.hpp keeps the help
  text, the banner, tokenize/dispatch, and a 25-line session loop
  (run_debugger was 89).
* **Verification**: the strongest available for a no-behavior-change
  claim — a scripted `--debugger` session exercising EVERY command
  path (src/fns/ir/cfg/stats/plan/bc/bkpts/vars/watch, opt
  break/continue/show/diff/list, vm reset/step/run/break/line/
  unbreak/breaks/until/dis/state, check) diffs byte-identical
  against the pre-refactor binary; `--check-all` runtime parity
  identical; the register-audit table regenerates byte-identically
  (RESULTS.md untouched by regen_audit_results.py); selftest 207/207,
  ctest 44/44, units 6328/6328, check-format clean; the new headers
  are self-contained (header_order 97/97 + umbrella, up from 92) and
  every quoted include portable (410/410).
* **Measured shape**: 457 -> 527 functions (70 new small ones);
  functions <= 10 lines 301 -> 332; the 71+ bucket 62 -> 58 (the
  642/158/143/89/68-line monsters gone; the biggest function in the
  touched files is now dbg_vm_state at 80, pre-existing in the
  support layer). The next-biggest remaining targets, in backlog
  order: `opt/loop/lcf.hpp`'s opt_lcf (~640), `opt/loop/unroll.hpp`
  (~470), `opt/scalar/fold.hpp` (~450) — the same treatment applies.

## 10. checkpoint 24: lcf decomposed (the code-quality continuation, round two) — DONE

The backlog's #1 named target, consumed: `opt/loop/lcf.hpp`'s
opt_lcf — at 640 lines the biggest function left in the tree — got
the full cdfold treatment (support layer + slim driver, one small
constexpr function per phase, scan bounds and emission order
preserved exactly). Pure refactor, zero behavior change.

* **the split**: `lcf.hpp` 718 -> 64 lines; the driver is a 24-line
  opt_lcf (scan once, walk the headers nest-first, and per loop:
  shape -> general-simulator-or-plan -> emit-and-rewire). The new
  `lcf_impl.hpp` (980 lines) holds LcfScan/LcfLoop (the pass-wide
  and per-loop state), lcf_pow/lcf_geo (the series algebra — lcf_pow
  has no callers, kept as lcf_geo's documented companion), the five
  shape matchers (lcf_loop_edges/lcf_inloop/lcf_unique_entry,
  lcf_single_exit/lcf_block_exit_ok, lcf_arms, lcf_gather_hphis/
  lcf_one_hphi, lcf_find_ind/lcf_try_ind_phi/lcf_step_match/
  lcf_cond_uses_ind), the plan gates (lcf_bound/lcf_bound_outside,
  lcf_trip_count/lcf_exits_now, lcf_zero_trip, lcf_outside_uses_ok,
  lcf_pre_avoids_exit, lcf_body_pure, lcf_maytrap_scan,
  lcf_analyze/lcf_one_evo/lcf_lin_to_evo, lcf_rt_ready), the two
  dispatchers (lcf_sim_call, lcf_rt_call — LcfGo {Skip, Folded,
  Emit} is the plan's tri-state), the emitters (lcf_splice,
  lcf_emit_closed/lcf_emit_one_closed/lcf_push_closed/
  lcf_push_scaled, the N-iteration simulator lcf_sim_n ->
  lcf_sim_iterations/lcf_sim_header_pass/lcf_sim_chain_pass/
  lcf_latch_transfer/lcf_eval1/lcf_emit_sim_batch) and the rewire
  tail (lcf_rewire_preheader, lcf_rewrite_uses, lcf_rekey_exit_phis).
* **the landmines, now documented in one place** (lcf_impl.hpp's
  header): every block scan stays bounded by the PASS-ENTRY nb (no
  fn.nblocks() substitutions); the fallback simulator's operand
  check reads only a/b2/c2, never I.args (the original's asymmetry,
  kept not "fixed"); LcfRtIn.C stays unset exactly as the old inline
  dispatcher left it; the analytic scan stops at the first failing
  phi but still records that phi's evo entry; the vreg allocation
  order of the emitters (fv, then c/t/c2 per shape) is preserved to
  the call. One behavioral trap caught in review before it could
  ship: the original `continue`s the WHOLE header loop when the test
  value is not header-defined — the general simulator is NOT tried
  — so lcf_find_ind returns bool (gate failed = skip loop) instead
  of leaving ind==NONE (gate passed, no induction = simulator).
  And loop_shape.hpp's look-alike helpers (loop_scan, in_loop_flags,
  header_phis, loop_shape_of) are deliberately NOT reused: their
  semantics differ (kc marks the LAST def, no ndefs==1; exactly one
  internal pred required where lcf tolerates several back-edges; no
  exit polarity) — lcf keeps its own scans, with a comment saying
  why.
* **Verification**: 93 CLI parity runs (--check-all / -O0 / bogus
  flag-error variants) + 30 stdin-aware runs (audit tapes) + the
  full 206-check --demo suite (every lcf_* demo: additive, geometric,
  runtime_init, rt_dynamic_bound, rt_le, rt_edge_max, sim_fallback,
  nested) diffed byte-identical against the pre-refactor binary; the
  register-audit table regenerates byte-identically (710 vregs /
  137 spills / 424 traffic, the committed baseline untouched);
  differential fuzz on the 16131-16280 (rotation) and 14231-14430
  (taildup+rt-lcf) windows identical modulo timing lines, zero
  mismatches; selftest 207/207, ctest 44/44, units 6328/6328,
  check-format clean; the new header is self-contained (header order
  98/98 + umbrella, up from 97) and every quoted include portable
  (414/414, up from 410).
* **Measured shape** (top-level brace-matched functions in
  src/minijz/**/*.hpp): 253 -> 317 (+64; 66 new small functions,
  the old 3-file trio merged into the driver); <= 10 lines
  76 -> 101, 11-20 50 -> 84, 21-40 42 -> 48; the 71+ bucket 69 -> 68
  — the 640-line opt_lcf is gone and nothing in the lcf pair
  exceeds 25 lines (lcf_geo, lcf_push_closed; the driver is 24).
  The next-biggest remaining targets, in backlog order:
  `opt/loop/unroll.hpp`'s opt_unroll (~470), `opt/scalar/fold.hpp`'s
  opt_constprop (~450), `opt/loop/unswitch_bif.hpp`'s
  unswitch_body_if (~432), taildup (~419), mem2reg (~415),
  rotate (~405) — the same treatment applies.

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

## 11. checkpoint 25: unroll decomposed + the algorithm/theory documentation wave — DONE

The backlog's #1 named target, consumed: `opt/loop/unroll.hpp`'s
opt_unroll — at 470 lines the biggest function left in the tree —
got the full cdfold/lcf treatment (support layer + slim driver, one
small constexpr function per phase, scan bounds and emission order
preserved exactly). Pure refactor, zero behavior change. Plus the
standing documentation request: every algorithm and data structure
in the SSA and optimization machinery now carries a theory-level
header comment.

* **the split**: `unroll.hpp` 498 -> 62 lines; the driver is a
  16-line opt_unroll (scan once, walk the headers nest-first, per
  loop: shape -> trip+growth -> safety -> commit; one unroll per
  call). The new `unroll_impl.hpp` (782 lines) holds UnrScan/
  UnrLoop/UnrMaps, the scan (unr_defs: LAST def and LAST Const win,
  like lcf's; unr_header_list), the shape matchers (unr_loop_edges
  with the LAST-is-the-latch rule, unr_header_term, unr_single_exit,
  unr_arms with the break-style polarity, unr_gather_hphis,
  unr_find_ind -> unr_try_ind -> unr_step_match +
  unr_test_uses_ind), the plan (unr_trip: the wrapping-arithmetic
  simulation, ivk pushed BEFORE the exit check so ivk[K] is the
  value left in i; unr_growth_ok's 400 cap), the safety scan
  (unr_outside_uses_ok with the X-block phi relaxation), the commit
  (unr_zero_trip, unr_build_maps with the k-major allocation order,
  unr_fill_entry, unr_mapv, unr_emit_header_copy / body_copies ->
  emit_body_block -> unr_emit_term/unr_edge, and the rewire tail
  unr_rewire_preheader / unr_rekey_exit_phis / unr_rewrite_uses).
  33 small functions, the largest 28 lines; the byte-identical
  contract (the landmines) is documented at unroll_impl.hpp's head:
  PASS-ENTRY bounds, the succ0-without-NONE-guard asymmetry at
  unr_copy_succs (the original's, kept not "fixed"), os.unrolls++
  before the K==0 shortcut, K > limit kept as belt and braces,
  loop_shape.hpp's look-alikes not reused (the lcf precedent).
* **the documentation wave**: 55 files, ~1,570 inserted comment
  lines, zero code churn. By hand (the SSA/optimization core, 16
  files): an.hpp (dominators/intervals/natural loops/frontiers +
  Cytron phi placement + renaming-stack theory), ir/instr.hpp (the
  representation theory: flat code vector, two-tier naming,
  pred-keyd phis, the nt marker), ir/builder (the pre-SSA emission
  model), ir/phi_norm, ir/destruct (parallel-copy / lost-copy /
  critical-edge theory), opt/mem2reg (the slot machinery's data
  structures), back/interp_ssa (edge-parallel phi staging + the
  runner-parity chain), sccp (lattice/Tarski/sparse-delta theory),
  gvn (the scoped-table mechanism), licm (the purity/zero-trip
  soundness argument), opt/driver (phase ordering: why a fixpoint,
  the tier-2 order unswitch -> unroll -> lcf -> rotate LAST),
  divmagic (Granlund-Montgomery magic numbers), callgraph (SCC
  scheduling + the splice), regalloc (liveness -> interference ->
  chordal coloring mechanics), config.hpp (the wrapping/trap
  semantic contract), fold (local vs conditional folding). Via six
  parallel documentation passes (40 files): scalar tier-2 (cse,
  copyprop, dce, canon, splice, ldelim, unreach), the loop tier-2
  family (lsimp, unswitch x3, lcf x4, loop_shape), the
  interprocedural tier-2 (icf, argpromo, deadargs, constcall,
  callcse, fnspec + tailrec/scctco/scctco_fold/cdfold), linear +
  analysis (linform, reassoc, alias, intervals, trapelim), and the
  back end (vm, vm_step, stack_alloc, bytecode, emit_plan, emit_c
  x2, emit_x64 x2). rotate.hpp already carried the tree's best
  theory header (the rotation diagram + the k+1 argument) and was
  left as-is.
* **the verification**: 93 CLI parity runs (stdin-aware, three flag
  variants) + the debugger tours, all diffed byte-identical against
  the pre-refactor binary; the register-audit table regenerated
  byte-identically; differential fuzz old-vs-new on 16131-16280 and
  14231-14430 — identical modulo timing lines, zero mismatches;
  selftest 207/207, ctest 44/44, units 6328/6328, check-format
  clean, header order 99/99 + umbrella, includes 419/419.

## 12. checkpoint 26: fold decomposed (the code-quality continuation, round four) — DONE

The backlog's #1 named target, consumed: `opt/scalar/fold.hpp`'s
opt_constprop — at 450 lines the biggest function left in the tree
(the whole 497-line file was one function plus banners) — got the
cdfold/lcf/unroll treatment. Pure refactor, zero behavior change:
support layer + slim driver, one small constexpr function per
phase, scan order, cascade order and counter increments preserved
exactly. Plus the standing recovery ask: the decomposition queue
now lives in OPT_CHECKLIST.md at the repo root (persisted after a
lost-checklist incident), and the parity differential is now a
committed script (scripts/parity_diff.sh) instead of ad-hoc bash.

* **the split**: `fold.hpp` 497 -> 67 lines; the driver is an
  11-line opt_constprop (build the context once, then the 12-round
  fixpoint: a quiet round breaks, any live round records the
  change). The new `fold_impl.hpp` (629 lines) holds FoldCtx (the
  def-index map + the per-round known-const tables) and 28 small
  constexpr functions: the def-index scan (LAST def wins, the
  guard defensive), the per-round table rebuild (Consts MADE by
  round k are visible to round k+1 — the fixpoint), the
  compare-domain machinery (fold_cmp_def re-checking D.k == K::Bin
  at use time, fold_inv_cmp's predicate inversion, the keep/zero/
  not fact trio gated by fold_bin_cmpfacts), the two-def identity
  (0-(0-x) = x, with the I.b2 != I.a self-match guard), the nine
  algebraic families (add/sub, mul, div/mod, and, or, xor, shift,
  mulhi — annihilators before identities, ka before kb), the
  same-vreg facts (x-x/x^x/x!=x -> 0, x==x/x<=x/x>=x -> 1; Add/
  Mul/Div/Mod deliberately NOT folded — x/x traps), the full-const
  fold, the per-kind handlers (Copy/Phi/Depth/Cbr — the Cbr one
  rewrites fn.succ in the terminator's own field order), the
  dispatch switch (ka/kb looked up before the switch, the
  original's shape), the round, and the context factory.
* **the one dedup**: the former 20-case inline wrapped-arithmetic
  switch is now instr.hpp's SHARED w_bin — provably the same table
  (same w_* per op, same {cmp,false} results; the textual
  difference, w_bin's unreachable trailing {0,true} vs the
  original's value-initialized Rv{}, is dead in both since the B
  enum has exactly the 18 covered values). Documented in
  fold_impl.hpp's contract header, cdfold's dedup precedent.
* **the landmine honored**: the 12-round structure is observable —
  consts made during a round only enter the tables at the NEXT
  rebuild, and the pass's return value drives the pipeline's
  os.iters. The round structure (rebuild, scan, quiet-round exit,
  the cap) is preserved to the character; the theory note in
  fold.hpp's banner names it.
* **the verification**: 146 parity runs (47 programs x three flag
  variants, stdin-aware, + three scripted debugger tours + --demo
  + the -O3 pass-statistics report), all diffed byte-identical
  against the pre-refactor binary; the register-audit table
  regenerated byte-identically (710 vregs / 137 spills / 424
  traffic); differential fuzz old-vs-new on 16131-16280 and
  14231-14430 — identical modulo timing lines, zero mismatches;
  selftest 207/207, ctest 44/44, units 6328/6328, check-format
  clean, header order 100/100 + umbrella (fold_impl.hpp is the
  100th), includes 422/422.
