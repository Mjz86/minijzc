# SUMMARY — checkpoint 32: the generic localization + the param survey

Two commits, one session. The user's directive: pull every mjz_lib
change into a generic folder INSIDE the compiler repo, written as if
it was in mjz_lib, such that the String repo (the vendored clone)
matches upstream — all changes localized for the author to merge —
then check every component for code-reuse opportunities (kernels
taking the necessary things as params, "for example a dominator
interval param", rather than calculating them inline).

## 1. generic/ is the single carrier (commit 1)

All 32 kernel headers moved VERBATIM (md5-verified) off the String
clone's `graph-opt-kernels` branch into `generic/mjz_lib/` — the opt
family (22 headers, aggregated by `opt/alllib.hpp`) and the graph
family (10 headers). The vendored clone reverted to PRISTINE upstream
`main` (`git diff origin/main` empty); the four changes that touch
files upstream already ships ride as patches in
`generic/merge-patches/`:

* `0001` — `alllib.hpp`'s opt-family `__has_include` hook,
* `0002` — `graph/algo.hpp`'s ten new-family includes,
* `0003` — `byte_str/implace_string.hpp`'s missing `template`
  keyword (a conformance fix g++ 14 warns about on the pristine
  clone; the compiler builds fine either way),
* `0004` — the OPTIONAL csr inverse-family fold (the design
  alternative to inverse.hpp; exactly one carrier, never both).

`generic/README.md` documents the layout, the merge route, and the
in-tree plumbing. The compiler imports both families through
`src/minijz/graph_lib.hpp` — the single mjz_lib import point, no
consumer changed. The generic headers keep their upstream-form quoted
includes (`"csr.hpp"`, `"../graph/csr.hpp"`), so CMake and the
header-order gate carry the clone's graph module dir on the include
path (both clone layouts); `include_portability_test.sh` still scans
only `src/`+`tests/` — inside `generic/` those includes are
rule-#1-true post-merge, the -I is the in-tree stand-in.

## 2. graph/inverse.hpp — the split carrier

The branch's csr.hpp rework deleted the three historical inverse
members and re-implemented them (byte behavior intact) over four
shared cores, adding thirteen siblings. Shadowing upstream csr.hpp
from generic/ would have been an order-dependent ODR trap, so the
thirteen NET-NEW members (four payload cores, the square
forward/both siblings, the universe octet) live in
`generic/mjz_lib/graph/inverse.hpp`, verbatim from the rework; the
three historical stay in upstream csr.hpp. `phis.hpp` gained
`#include "inverse.hpp"` — the only content delta besides the new
file (on the branch the universe transpose lived in the reworked
csr.hpp).

## 3. The param survey (commit 2)

Verdict first: the opt family was ALREADY param-driven by design —
vn/mem/ssa take `dominator_tree`, inv takes the `dom_begin/dom_end`
interval pair (licm passes An's `A.ib/A.ie` today — literally the
user's "dominator interval param"), layout takes cfg's reachable
mask, phis takes dominance frontiers — and the compiler's An layer
is 100% kernel-built (`make_dominator_forest_given_dominators`,
`calculate_dominance_frontiers_given_dominators`; zero hand-rolled
orders, masks, or dominators anywhere in src/).

The ONE genuine inline site: rmas's
`calculate_minimum_feedback_arc_set_reducible` computed immediate
dominators internally (plus a preds forest) every call. It gained a
`..._given_dominators` sibling (the family's `*_given_*` naming) that
takes the caller's idoms; the 3-arg form builds the table inline —
same preds scan, same upstream dom kernel — and forwards. Callers
that already hold idoms (the compiler's An layer, any future
loop-family consumer) skip the rebuild.

Considered and REJECTED (per standing requirement 1, reasons on
record in CONTINUE.md): rmas's internal `make_basic_forest(net.succs)`
conversions (flow-kernel input format over internal data), cut's
symmetrization forest (built from its own symmetrized edge list),
matching's bipartite transpose (the Hopcroft-Karp working set),
`calculate_minimum_equivalent_arc_set`'s internal SCC condensation
(consumed immediately, no second consumer).

## 4. Verification

Commit 1 (localization): parity 146/146 byte-identical vs the
pre-move baseline (rebuilt from clean HEAD first — the sandbox had
recycled); selftest 208/208, units 6482/6482, header order 116/116 +
umbrella, includes 527/527, kernel fuzz 3000 seeds all components 0
fails, e2e fuzz 600, stress ALL PASS. Commit 2 (the param split):
parity 146/146 again; fz_rmasp now SELF-DIFFERENTIALS the two forms
every seed (the given-dominators call vs the standalone call vs the
exact 2^V sweep — 35408 checks, 0 fails, was 23536); the rest of the
battery green.

The String clone: pristine `main`, `origin/main` at ce6ea95; the
local branch `graph-opt-kernels` (tip 2cb6c7d) keeps the same content
as commits for diff-based review. The queue after this
(OPT_CHECKLIST.md) is unchanged: unswitch, argpromo, tailrec's
tce_merge, unswitch_shape — rotate_impl.hpp the template, both
standing requirements in force.
