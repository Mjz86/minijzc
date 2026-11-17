# generic/ — the mjz_lib additions, localized

Every change this compiler repo carries FOR mjz_lib (the String repo,
vendored as a clone at `../mjz_lib`) lives in THIS folder, written as
if it was in mjz_lib: the same MIT headers, the same
`MJZ_SRC_GRAPH_*_FILE_` / opt-family guards, the same `MJZ_EXPORT` +
`namespace mjz::graph_ns` / `mjz::opt_ns`, the same THE MODEL / THE
FIXPOINT / THE ROWS contract style. The vendored clone itself matches
upstream `main` byte-for-byte (nothing under `mjz_lib/` is modified),
so a fresh `git clone https://github.com/Mjz86/String mjz_lib` always
yields a working tree, and the author can merge this folder whenever
the design is agreed — every change is localized here.

## What is here

- `mjz_lib/opt/` — the optimization-kernel family (22 headers: ai,
  alias, base, cfg, dse, equi, escape, interval, inv, ipa, layout,
  liveness, mark, mem, pcopy, ren, route, sccp, shape, trivphi, vn,
  plus the `alllib.hpp` aggregate). Lifted from the compiler's passes
  and made generic: the back end (regalloc's liveness / interference
  / coloring chain) and the opt drivers call them through `opt_ns::`.
- `mjz_lib/graph/` — the new graph-algorithm family (biconnect, cut,
  distances, euler, flow, matching, mst, phis, rmas, ssa) plus
  `inverse.hpp` — see "the two deltas" below.
- `merge-patches/` — the four changes that a plain file copy cannot
  express, because they MODIFY files upstream already ships.

The same content also rides the String clone's local branch
`graph-opt-kernels` (tip `2cb6c7d`) for diff-based review; this
folder is the canonical in-tree carrier.

## The two deltas vs that branch

Every file is a verbatim copy of the branch's file except:

1. `graph/phis.hpp` gains `#include "inverse.hpp"` — on the branch
   the universe transpose lives in the reworked `csr.hpp`; here
   `csr.hpp` is upstream's (the three historical members only), so
   phis.hpp names its provider directly. Post-merge the include
   resolves search-rule-#1 either way.
2. `graph/inverse.hpp` is new AS A FILE: the thirteen NET-NEW members
   of the inverse family (the four payload cores, the square
   forward/both siblings, the universe octet), extracted verbatim
   from the branch's `csr.hpp` rework. The three historical members
   stay in upstream `csr.hpp` with names and byte behavior intact —
   nothing is shadowed, so there is no ODR hazard between this folder
   and the pristine clone.

## Merging upstream (for the author)

Route A (mechanical): copy `generic/mjz_lib/*` into String's
`mjz_lib/`, then apply patches `0001` (`alllib.hpp`'s opt-family
hook), `0002` (`graph/algo.hpp`'s family includes) and `0003`
(`byte_str/implace_string.hpp`'s missing `template` keyword — a
conformance fix g++ 14 requires). Patch `0004` is the OPTIONAL
design alternative: it folds the whole sixteen-member inverse family
into `csr.hpp` (re-implementing the three historical members over the
shared cores). If you take `0004`, do NOT copy `graph/inverse.hpp`
and drop phis.hpp's include of it — the family needs exactly one
carrier, not both.

## The in-tree plumbing (stays here, does not ride along)

- `src/minijz/graph_lib.hpp` imports the families right after the
  String bridge — the compiler keeps its single mjz_lib import point,
  and no consumer changed.
- The generic headers include their String dependencies in upstream
  form (`#include "csr.hpp"`, `#include "../graph/csr.hpp"`) — quoted
  includes that resolve from the includer's own directory once
  merged. In-tree they resolve through two extra include paths (both
  clone layouts covered) added to every CMake target and to the
  header-order gate: `mjz_lib/mjz_lib/graph` and `mjz_lib/graph`.
- `scripts/include_portability_test.sh` deliberately scans only
  `src/`, `tests/` and `main.cpp`: inside `generic/` those same
  quoted includes are upstream-form (rule-#1-true post-merge); the
  include paths above are the in-tree stand-in for the merge.

## The standing quality gates (CONTINUE.md, checkpoint 31)

Every component here carries its contract comment, its fuzzer entry
in the fuzz-kernel table (`apps/fuzz_kernels.hpp`, each kernel
differentially fuzzed against an independent reference
implementation), and rides the full battery before any commit; the
code-bloat / deduplication pass ran over these families before the
folder was cut (CONTINUE.md keeps the ledger and the rejected dedups
on record).
