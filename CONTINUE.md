# CONTINUE — next steps after checkpoint 19 (body-if unswitch + VM exits + lcf-sim polarity)

Read SUMMARY.md (same zip root) first: branchA @ one commit on top of
checkpoint 18 (`d697225`) — the classic "trivial" unswitch of an
invariant INNER branch (both versions keep running the loop; every
exit — the header's arm included — merges through a version-merge
block; outside reads route through a delivered walk that can BIRTH
merge phis at 2-pred post-loop blocks), the unswitch_shape.hpp /
unswitch_bif.hpp split, the lcf-sim polarity fix the bait exposed,
and the 13631+ fuzz windows. This file is the resume manual.

## Setup (2 min)

```sh
cd minijzc                       # branchA @ the ckpt-19 commit
git log --oneline -4             # ckpt19 ... d697225 unswitch+canon ... 72685f7 ...
git clone https://github.com/Mjz86/String mjz_lib   # inside this repo
# (NO symlinks: src/mjz_lib/alllib_import.hpp adapts to both String
#  layouts via __has_include and carries the libc++ atomic_ref shim.)
export PATH="$HOME/.venv/bin:$PATH"   # cmake 4.4 + clang-format via pip
# (if the sandbox recycled: pip install cmake clang-format)
cmake -S . -B build && cmake --build build -j4
./build/minijz --selftest                    # 207/207
ctest --test-dir build -j4                  # 40/40
./build/minijz_unit_tests                   # 6215/6215
./build/minijz --fuzz=300 --fuzz-from=13631 --fuzz-jobs=4   # the bait window
```

## Try the checkpoint-19 features right away

```sh
# 1) body-if unswitch (watch UNSWITCH and the guard cbr in the diff):
printf 'let bf = input();\nlet cn = 0;\nwhile (cn < 5) {\n  if (bf > 0) { cn = cn + 1; } else { cn = cn + 2; }\n}\nprint cn;\n' > bi.mjz
echo "3" | ./build/minijz bi.mjz                # 5 (the +1 version)
echo "0" | ./build/minijz bi.mjz                # 6 (the +2 version overshoots:
                                               #  the VM-merged exit value)
./build/minijz bi.mjz --opt-diff=0:unswitch    # pre: cbr(INV) h / h', the
                                               # clone, the version-merges

# 2) the scripted debugger session (the committed ctest):
printf 'opt break unswitch\nopt\nopt show 2\nopt continue\nvm run\ncheck\nquit\n' \
  | ./build/minijz demos/bif_tour.mjz --debugger --stdin=demos/bif_tour.in

# 3) the break-arm read + the 2-pred post-loop merge (delivered phis):
printf 'let bf = input();\nlet c = 5;\nlet s = 0;\nwhile (c > 1) {\n  if (c == 2) { print 900 + c; break; }\n  if (bf > 0) { c = c - 1; } else { c = c - 2; }\n  s = s + c;\n}\nprint s * 10 + c;\n' > bb.mjz
echo "3" | ./build/minijz bb.mjz                # 902 92
echo "0" | ./build/minijz bb.mjz                # 41
```

## Standing backlog (priority order)

1. **Keep widening the fuzz window**: rotation 14231+, random-cfg
   13831+ (the bait vocabulary is seed-floored: 12301 bkpt, 12331 gvn,
   12731 unswitch, 13331 compound-cond, 13631 body-if — the next flavor
   needs a NEW floor constant so committed fixtures stay
   byte-identical). The 13931-14230 rotation window already ran clean.
2. **Wishlist remainder** (README task 3): with body-if DONE, the
   natural next steps are **tail duplication** (the degenerate guarded
   unswitch: version on a loop-VARYING branch when the duplicated arm
   is small — the unswitch_bif clone machinery + a cost model is most
   of it) and **loop rotation proper** (the while-lowering already
   computes the condition in the header; rotation would move the
   body-first shape in, feeding unroll/lcf more closed forms). After
   that: LCSSA (needs undef semantics this IR does not have — a
   loop-exit phi's zero-trip edge has no defined value; document it as
   not-constructible rather than open), demanded/known bits (thin:
   the surface language has no bitwise syntax — only
   divmagic-produced shifts/mulhi), MemCPyOpt, loop
   fusion/fission/distribution, function outlining, Attributor-style
   inference, GlobalOpt (no globals — likely not-constructible too).
3. **Code quality continuation** if wanted: next-biggest files are
   `back/vm.hpp` (~1044), `back/emit_x64_impl.hpp` (~888),
   `apps/debugger.hpp` (~970), `opt/interprocedural/cdfold.hpp
   (~815) — the support-layer + slim-driver treatment (the lcf and
   unswitch splits) works well; the full battery + all scripted
   debugger sessions protect byte-identical behavior. unswitch.hpp is
   down to ~430; unswitch_shape/bif carry the rest.
4. **A real `vs-msvc` run on actual Windows/MSVC** is still the one
   unverified step (no MSVC in the sandbox; this session also lacked
   clang++ — native parity ran on g++; see the ckpt-16 SUMMARY "Known
   limits" for what was cross-verified back then).

## Protocol (unchanged)

README task-list ledger update → full battery green (selftest,
ctest, units, format, parity spot-checks, fuzz windows clean) →
commit → `download/20-*.zip` with fresh SUMMARY.md + CONTINUE.md →
archive with ANCHORED excludes (`./build`, `./build-ho`, `./mjz_lib`,
`*.exe`, `*.out.tmp`, `*.mbc` — never a bare `--exclude=mjz_lib`, it
eats the tracked `src/mjz_lib/`) → do NOT push origin (sandbox has no
credentials).

## Landmines hit this checkpoint (worth remembering)

- **The versioned true loop can RESHAPE under your feet.** After the
  body-if unswitch, entry-threading + splice carried the `break`
  condition up as the true version's header test (exit-on-TRUE), and
  the lcf SIMULATOR (not the analytic counter — that was fixed in
  checkpoint 17) folded it as zero-trip through the wrong exit edge.
  Any pass that walks loop headers must ask which ARM leaves, not
  assume continue-style.
- **Post-loop plain reads are legal SSA.** A loop's exit target with
  preds {header, break arm} reads the header phi PLAINLY (no phi —
  the header dominates the whole post-loop region), so versioning
  must BORN a phi there: one arg per post-pred (the VM for the
  in-loop pred, the delivered value for the outside pred). The
  reverse-reachability collection decides which VMs must carry which
  values — one merge per vreg covers all its readers.
- **Validate before ANY commit edit.** The delivered walk is
  cycle-guarded and source-checked up front; a walk that could fail
  mid-commit would leave broken IR behind (the fn is already cloned).
- **The fuzz bait must be self-contained when it early-returns.**
  The r==6 statement generator closes the loop brace in its common
  tail; a bait that emits its own `}` and post-loop print must return
  BEFORE that tail, or every seed >= the floor fails to parse.
