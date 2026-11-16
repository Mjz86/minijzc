# ALGORITHMS.md — every algorithm in the tree, its reference paper, and what was added to implement it

The compiler's algorithms live in three generic layers, all speaking the
same `uintlen_t` index-space dialect (dense client-chosen ids for blocks /
vars / sites, CSR rows and event streams as the only inputs, pure tables
as the only outputs — no `Instr`, no `Fn`, no `K`/`B` enums ever appear):

| layer | file | what lives there |
| --- | --- | --- |
| `mjz::graph_ns` | `mjz_lib/mjz_lib/graph/` | pure graph theory: dominators, frontiers, SCC, loops, coloring |
| `mjz::ssa_ns` | `src/minijz/ir/ssa_generic.hpp` | SSA construction over any index space |
| `mjz::opt_ns` | `src/minijz/opt/opt_generic.hpp` | the optimization kernels (26 families) |

**The founding rule** (this branch's directive): an ALGORITHM must not
depend on the way the compiler works. Binary phis, the `>= 2 preds`
rule, the `< 2 defs` skip, root policies, trap policies, hoist legality
— those are the COMPILER'S policies and live in the passes (the
wrappers); what lives in the layers is the relation itself, stated for
ANY arity and ANY policy. Every entry below ends with where the
wrapper's policy attaches.

Everything is `constexpr` (`MJZ_CX_FN`), `noexcept`, and unit-tested
standalone in `tests/unit_tests.cpp` (section 17 + the kernel sections)
— the layers could lift into `mjz_lib` unchanged.

---

## 1. graph_ns — the graph theory layer

### 1.1 Dominators — `graph/dom.hpp`
**Paper.** Thomas Lengauer & Robert Endre Tarjan, *A Fast Algorithm for
Finding Dominators in a Flowgraph*, ACM TOPLAS 1(1), 1979, §3–5.

**The paper gives.** The semidominator (`sdom`) recurrence — computed
over a reverse DFS preorder with the two-clause minimum (clause 1: an
incoming tree edge from an earlier node; clause 2: a path through
later-numbered nodes) — and the dominator derivation
`idom = sdom`-anchored walk up the DFS tree, with union-find path
compression making the whole thing near-linear.

**The kernel here.** `calculate_dominators_from_entry(ret, entry, succs,
preds)` — idoms as a flat `node -> node` table over any CSR forest
pair; `calculate_inverse_dominators_from_dominators` and the DFS
interval helpers (`ib/ie`: `d` dominates `s` iff `[ib[d],ie[d]]`
contains `[ib[s],ie[s]]`) round out the family.

**What was added to implement it.** The semi-dominator discovery
ordinal doubles as the DFS preorder numbering (one array, two duties);
the DSU path compression runs over the DFS-tree parent forest with the
compressed ancestors re-checked against the current node's ordinal
(the Lengauer-Tarjan `EVAL` step inlined); unreachable nodes keep
`idom == self` so downstream frontiers can skip them without a
separate reachability pass. Everything is stated over *generic* CSR
rows, not an IR CFG.

### 1.2 Dominance frontiers — `graph/idf.hpp`
**Paper.** Ron Cytron, Jeanne Ferrante, Barry Rosen, Mark Wegman,
Kenneth Zadeck, *Efficiently Computing Static Single Assignment Form
and the Control Dependence Graph*, ACM TOPLAS 13(4), 1991 — Figure 9
(the `DF` computation via `idom` runner walks).

**The kernel here.** `calculate_dominance_frontiers_given_dominators`
and the inverse (`IDF`-facing) variant.

**What was added to implement it.** The runner walk is implemented
with an intrusive singly-linked list threaded through a scratch array
(`current_inverse_frontiers`), so each pred-to-idom climb marks and
chains exactly the frontier members without allocation; the `stop_node
== idom(y)` termination and the root sentinel (`idom[y] == y`) follow
the paper. The generic contract takes any `idom` table + any pred
forest — the compiler passes blocks, `mem2reg` passes slots.

### 1.3 Iterated dominance frontiers — `graph/idf.hpp`
**Paper.** Cytron et al. 1991, Figure 11 (the worklist `DF+`
algorithm: place at `DF(worklist)`, push the placed blocks, repeat
until dry).

**The kernel here.**
`calculate_iterated_dominance_frontiers_given_dominance_frontiers` —
IDF of a seed set, given precomputed DF rows.

**What was added to implement it.** The visited/worklist bits are
packed into one tagged array (the low bit doubles as the "already on
the output" marker), and the output is written in discovery order —
the order the SSA layer's block-ascending enumeration then depends
on. Sized scratch is allocated once per call and reused across every
variable's IDF query (see `graph/phis.hpp`).

### 1.4 Phi placement (the pure relation) — `graph/phis.hpp`
**Paper.** Cytron et al. 1991, §4 (the placement criterion: variable
`v` with def-site set `S` needs a phi at block `Y` iff `Y ∈ DF+(S)`).

**The kernel here.** `calculate_phi_placements(df, def_blocks_of_var)`
— the VAR-major relation: row `v` = the blocks needing a phi for `v`;
`calculate_inv_phi_placements` gives the transposed, block-major
index.

**What was added to implement it.** The relation is stated with ZERO
client assumptions: every def count places per the theory (single-def
vars get their `DF(def)` phis exactly as minimal SSA says — the
compiler's `< 2 defs` filter does NOT live here), pred counts are not
even an input (a one-pred loop header places when the theory says
so), and arity is explicitly a LOWERING concern — this layer only
says WHERE. The transpose needed its own `make_basic_inv_forest_universe`
primitive because the relation is bipartite (var rows, block entries):
the square `make_basic_inv_forest` would size the output by the wrong
universe (this was the review bug — fixed here, with the reasoning in
the file banner).

### 1.5 Strongly connected components — `graph/scc.hpp`
**Paper.** Robert Endre Tarjan, *Depth-First Search and Linear Graph
Algorithms*, SIAM J. Computing 1(2), 1972.

**What was added to implement it.** `calculate_strongly_connected_
components` returns the SCC index per node plus the member lists in
Tarjan's finalization order (an SCC finalizes only after every SCC it
can reach) — the order the IPO pipeline exploits to fold callees
before callers. The compiler's `callgraph.hpp` builds the call graph
(edge per call site) and consumes exactly this.

### 1.6 Natural loops — `graph/loop.hpp`
**Reference.** The natural-loop definition via back edges and
dominance: Frances Allen, *Control Flow Analysis*, SIGPLAN Notices
5(7), 1970 / Matthew Hecht & Jeffrey Ullman, *Characterizations of
Reducible Flow Graphs*, J. ACM 21(3), 1974; the formulation used
here is the standard one (Muchnick, *Advanced Compiler Design and
Implementation*, 1997, §7.5).

**What was added to implement it.** Loop membership rows per header
(`A.loops`, consumed by every restructuring pass), derived from the
dominator table: a block is in the loop of header `h` iff it reaches
`h`'s latch without passing `h`. Nested-loop orderings come off the
same walk (the `nest-descending` header list unroll walks).

### 1.7 Chordal-graph coloring — `graph/color.hpp` (register allocation)
**Papers.** Delbert Fulkerson & Oliver Gross, *Incidence Matrices and
Interval Graphs*, Pacific J. Math 15(3), 1965 (chordal graphs admit
perfect elimination orderings); Robert Tarjan & Mihalis Yannakakis,
*Simple Linear-Time Algorithms to Test Chordality of Graphs...*,
SIAM J. Computing 13(3), 1984 (the maximum-cardinality-search order);
Sebastian Hack, Daniel Grund, Gerhard Goos, *Register Allocation for
Programs in SSA Form*, CC 2006 (SSA interference graphs are chordal,
so greedy in MCS order is OPTIMAL — colors = clique number).

**What was added to implement it.** `back/regalloc.hpp`: liveness →
interference → `color_chordal_graph` (greedy in MCS order) → the
5-register assignment to the callee-saved pool (`rbx, r12..r15`), with
spill slots for the rest. The optimality clause is what lets the
register-audit table claim exactness.

### 1.8 Reachability — `graph/reach.hpp`
Textbook DFS/BFS flood, stated over CSR rows; `calculate_reachable_
mask` is the kernel behind `opt/unreach.hpp` (see 3.6).

---

## 2. ssa_ns — SSA construction (`src/minijz/ir/ssa_generic.hpp`)

**Paper.** Cytron, Ferrante, Rosen, Wegman, Zadeck, *Efficiently
Computing Static Single Assignment Form and the Control Dependence
Graph*, ACM TOPLAS 13(4), 1991 — §4 (placement), §5.2 (`Rename`).

### 2.1 The placement wrapper — `ssa_ns::calculate_phi_placements`
The pure relation (1.4) plus THIS IR's policy, stated as a filter
after the fact: variables with `< 2` def blocks place nothing (the
language's builder guarantees the single def dominates every use that
matters), blocks with `< 2` incoming edge SLOTS place nothing
(duplicate preds count — a `[p,p]` row from `cbr(c,Y,Y)` is 2), and
the output is transposed to block-major in ascending variable order —
the emission order the phi-placeholder machinery depends on. The
policy-free placement is unit-tested to DISAGREE with the wrapper in
exactly these cases (test 16i pins the gap).

### 2.2 Renaming — `rename_ssa`
**The paper gives.** The dominator-tree DFS with per-variable stacks
of definitions: push at defs, rename uses to the stack top, pop on
exit; phis are defs in their block and uses at the END of each
predecessor (the "phi args are renamed on the out-edges of the pred"
rule).

**What was added to implement it** (the deltas over the paper):
- **Def-site stacks, not name counters.** Each variable's stack holds
  DEF SITE ids, and a def's version number (`def_ordinal`) is its
  STACK DEPTH at push time — so versions are dense, comparable, and
  independent of the client's id vocabulary.
- **A flat undo log instead of recursion.** The walk is an explicit
  frame stack; every push appends `(var, old_top)` to one flat log,
  and leaving a block replays to the frame's mark. No destructor
  cascades, no per-block vectors.
- **Phi records are tables, not instructions.** One (pred, arg) entry
  PER PRED-SLOT EDGE of the phi's block — N-ary by construction.
  Phi def sites get RESERVED ids `[site_count, site_count + n_phis)`
  (with `phi_ord_base` mapping block -> its first phi ordinal), and
  `phi_entry_begin` gets an `n_phis + 1` sentinel so the last record
  has an end. Entries are pre-poisoned with `ssa_none_v`; a pred the
  dominator walk never reaches (unreachable) or a duplicate-pred row
  (only the LAST matching slot is written) keeps the poison —
  matching the hand-rolled walk bit for bit.
- **The only IR knowledge is the event stream** `(kind, var, site)`
  per block, in program order. The same kernel renames vregs
  (builder), slots (mem2reg), and could rename anything a client
  numbers.

### 2.3 The reflow plan — `calculate_reflow_plan`
**No paper — this tree's own.** The "instruction removal" half of
SSA-style rebuilds as pure arithmetic: given old per-block site
ranges, a keep mask, a head count and an insert count, produce each
block's new `[begin, end)` and the new total — the flat-code-vector
equivalent of "drop dead stores, prepend phis." `mem2reg` and the
builder's sweep both rebuild through it, so no IR surgery lives in
the kernel.

### 2.4 The merge-chain plan — `calculate_merge_chain_plan`
**Reference: the N-pred lowering problem.** The renaming kernel is
N-ary; the IR's phis are binary. The plan lowers an N-ary phi at
block `b` to a CHAIN of binary phis through fresh merge blocks, as
pure data (which fresh blocks, which pred rekeys, which arg feeds
which hop). This is the compiler-driven half of "generic data,
compiler drives": the kernel states the plan, the pass mints the
blocks.

### 2.5 mem2reg — `opt/mem2reg.hpp`
**Reference.** The promotion of stack slots to SSA values is SSA
construction applied to memory — Cytron et al. 1991 (the same
placement + renaming, over SLOTS as pseudo-variables); the pass shape
(freshly named "mem2reg") follows the LLVM precedent.

**What was added.** Slots are numbered (`slot_base[id] + j`), def
blocks per slot (the Alloc's zero-init + every Store) seed the IDF
placement, a Load/Store/init event stream drives `rename_ssa`, and
the rebuild goes through `calculate_reflow_plan` — dead stores
dropped, slot phis prepended, loads become the reaching def's value.
The promotion gates (single-handle aliasing, constant in-range
indices, the 2048-slot budget) are policy and live in the pass.

---

## 3. opt_ns — the optimization kernels (`src/minijz/opt/opt_generic.hpp`)

### 3.1 mark_ns — dead-code elimination (`mark_ns::calculate_live_sites`)
**Reference.** Mark-sweep reachability over the def/use bipartite
graph — the mark phase of McCarthy's 1960 GC transposed to code
("liveness by marking"), as presented for DCE in Muchnick 1997, §18.

**What was added.** The pure table form: `def_of_var` (SSA: one def
site per var), `args_of_site` (ANY order, duplicates and junk ids
tolerated), `roots` (the client's observability policy). The
fixpoint is order-independent (a site is pushed at most once), which
is why `dce.hpp`'s output stayed byte-identical when its hand-rolled
walk became this call. The same kernel answers every "is it used"
question in the tree. `calculate_trivial_phi_pick` (below) is dce's
phi-collapse companion.

### 3.2 The trivial-phi pick (`calculate_trivial_phi_pick`)
**Reference.** The trivial-phi identification of Briggs, Cooper,
Harvey, Simpson, *Practical Improvements to the Construction and
Destruction of Static Single Assignment Form*, CC 1998.

**What was added.** The N-ary statement: all non-self incoming args
equal (absent args ignored) -> that arg; `phi(a,a)`, `phi(a,self)`,
`phi(a)` pick `a`. More than one distinct value or nothing but
self/absent -> `none_v` (a genuine phi). dce collapses such phis and
feeds copyprop.

### 3.3 vn_ns — value numbering (`value_number`) — cse + gvn
**Papers.** Bowen Alpern, Mark Wegman, Kenneth Zadeck, *Detecting
Equality of Variables in Programs*, POPL 1988 (congruence with
commutative canonicalization); Preston Briggs, Keith Cooper, Tim
Harvey, Taylor Simpson, *Value Numbering*, Software: Practice and
Experience 27(6), 1997 (the DOMINATOR-SCOPED table — an entry is in
scope exactly where its definition dominates); Clifford Click,
*Combining Analyses, Combining Optimizations*, PhD thesis, Rice 1995
(the hashed-value + redundancy-elimination view).

**What was added to implement it.**
- The scoped table with **creation-sequence watermarks**: entering a
  child records four flat marks (table, resolved phi args, phi preds,
  keyed operands); leaving truncates all four together — no per-block
  hash tables, everything is one flat store per payload kind.
- **Operand resolution through the growing `ren` map**, so a hit keys
  by the canonical representative (chains collapse in one walk).
- **Const operands key by VALUE** when the client's const table knows
  them; an ABSENT operand keys as const 0 — the compiler's historical
  table encoding, kept bit-for-bit so cse/gvn output is unchanged.
- **Commutative canonicalization** by a mixed var/value sort tag (the
  same `mixed_sort_key` the `canon` pass orders by — one canonical
  shape per equivalence class is what makes CSE hit).
- **Phi congruence** = same block, same resolved incoming map
  (per-pred), plus the **taint rule** — a repo-specific soundness
  guard: either phi's raw args may not contain the other's dst,
  checked over ALL of the block's phi sites (hit-dropped ones
  included, hence the site rows and not the table).
- One kernel, two passes: `cse` (local-window entries) and `gvn`
  (full scopes) are the same walk with different eligibility tags.

### 3.4 sccp_ns — sparse conditional constant propagation (`sccp_engine_t`)
**Paper.** Mark Wegman & Kenneth Zadeck, *Constant Propagation with
Conditional Branches*, ACM TOPLAS 13(2), 1991.

**What was added to implement it.**
- The ENGINE owns the two worklists (SSA vars + CFG edges) and the
  executability tables; the TRANSFER SEMANTICS stay with the client
  as one callback over scalars (the `std::sort`-comparator pattern:
  the algorithm is generic, the policy — here the evaluation — is
  not). Per site the client declares `value | phi | branch`.
- **Executable-edge tracking by SLOT id** (prefix sums over the raw
  succ rows, `none_v` holes included), which makes the duplicate-pred
  rule fall out: `pred_exec_into(p, target)` is true when ANY of
  `p`'s slots reaching `target` is executable — the N-ary statement
  of "the edge p->t is executable."
- **Phi merging over EXECUTABLE in-edges only** (the paper's
  conditional-branch sensitivity), one meet per entry, monotone
  descent by construction (`lower` only fires on change), so the
  fixpoint is unique regardless of visit order — which is why the
  compiler's rewrite (a pure read of `lat`/`bexec`) is byte-identical
  even though the engine internals are new.
- A step guard: `run()` returns false when it trips; the caller's
  rule is "no rewrite" (the non-termination safety net).

### 3.5 pcopy_ns — parallel-copy sequentialization + edge splits
**Papers.** The parallel-assignment hazards of SSA destruction are
the LOST-COPY and SWAP problems: Cytron et al. 1991, §7 (critical
edges + parallel semantics of phis); Briggs et al. 1998 (the
destruction pipeline this tree mirrors); the modern taxonomy and the
temp-for-cycle solution: Fabrice Rastello & Florent Bouchez Tichadou
(eds.), *SSA-based Compiler Design*, Apress, 2015 (the SSA
destruction chapters). Critical
edge splitting itself is Cytron et al. 1991 (and standard practice).

**What was added.**
- `sequentialize_pcopy`: the greedy filter emits only copies whose
  dst is not a pending SOURCE; whatever survives the filter is
  EXACTLY a permutation cycle, broken by saving ONE source into a
  fresh temp — the temp is minted by a client callback at exactly the
  cycle-break points (the wrapper's vreg numbering stays its own).
  `src == none_v` is the UNDEF arg, materialized as const zero
  (matching the language's zero-initialized storage). Self-copies
  drop first (they would look like unbreakable 1-cycles).
- `calculate_edge_splits`: the critical-edge split as a pure PLAN —
  split `(pred, slot -> target)` iff the pred fans out to `>= 2`
  DISTINCT targets and the target holds phis; fresh block ids are
  assigned sequentially in (pred asc, slot asc) order — the order the
  client's rebuild must apply to mint identical ids.

### 3.6 cfg_ns — reachability + splice (`calculate_reachable_mask`, `calculate_splice_plan`)
**Reference.** Reachability is the textbook flood (see 1.8). Splice —
merging a block into its single successor — is standard CFG cleanup
(cf. Muchnick 1997 §7.3, jump elimination); the phi-free condition is
the SSA-era refinement (a phi block's args key on preds, so it cannot
be absorbed without re-keying).

**What was added.** The splice plan is MATCHINGS ONLY (no chains):
`b` absorbs its unique succ `s` when `s` is not `b`, not the entry,
not already paired, entered ONLY from `b`, and phi-free. The sweep
REKEYS the preds of `s`'s successors as it goes (a later pair's
"entered only from `b'`" check reads the rekeyed rows), so the kernel
keeps its own mutated copy — the client never sees it. The rebuild
(concatenating code ranges, dropping absorbed terminators) is IR
surgery and stays in `splice.hpp`; so do the phi t/f rekeys.

### 3.7 inv_ns — loop-invariant code motion (`find_unique_outside_pred`, `calculate_first_invariant_site`)
**Reference.** The classic invariant test and motion legality:
Muchnick 1997, §14.1; the dominance clause (operands' defs must
DOMINATE the preheader, not merely precede the loop) follows
Allen/Cocke predominator theory — Randy Allen & Ken Kennedy,
*Optimizing Compilers for Modern Architectures*, Morgan Kaufmann
2002.

**What was added.** The kernel answers the FIRST hoistable site under
the scan discipline (header FIRST — an invariant header test is the
classic unswitch enabler — then the member row in order), where
"hoistable" = eligible AND every operand is invariant: absent, or
const (position-independent), or defined outside the loop by a def
that DOMINATES the preheader (the dominance-interval test). ELIGIBILITY
(purity, trap safety — the compiler's no-Alloc/no-Load/no-Call rules)
is a mask input, i.e. policy. One site per call: the pass hoists,
recomputes its tables, and calls again (a hoist can make more
operands invariant) — `licm.hpp` keeps the rescan discipline.

### 3.8 ren_ns — copy propagation (`build_copy_rename`, `resolve_capped`)
**Reference.** Copy propagation as a rename map: Muchnick 1997
§13.2; the map is transitively closed, i.e. the cheapest congruence
there is.

**What was added.** Sources resolve transitively through the map
BEING BUILT, so copy chains die in one hop; the CROSSED-PAIR guard
refuses the closing copy of a cyclic rename (`resolve(src) == dst` —
a self-loop entry that would never resolve; the phi-degenerate
hazard, found by differential fuzz seed 7406 and pinned by a unit
test); the resolve walk is capped at `n_vars` steps — belt and
braces so a future multi-def IR cannot resurrect an infinite walk.

### 3.9 shape_ns — natural-loop shape (`calculate_header_preds`, `calculate_loop_shape`, `collect_exit_edges`)
**Reference.** The test-first (do-while) loop form with unique
preheader and latch is the loop-simplify/rotate canon: Allen 1970 /
Hecht & Ullman 1974 for natural loops, and cf. LLVM's
`LoopTerminology` (preheader / latch / guard) documentation of the
same shape.

**What was added.** Three policy-free kernels every restructuring
pass composes:
- `calculate_header_preds`: ONE sweep of a header's pred row against
  the loop mask — the LAST inside pred is the latch candidate, the
  LAST outside one the preheader candidate, counts reported. The
  "exactly one of each" test is the CALLER's policy (unroll/lcf
  tolerate multiple back edges, cdfold/loop-shape do not).
- `calculate_loop_shape`: the canonical test-first shape — unique
  latch + pre + a TWO-ARM branch, exactly one arm staying (body),
  one leaving (X). The terminator KIND check stays with the client
  (which instruction ends a block is IR knowledge; the arms and
  their in/out classification are CFG math).
- `collect_exit_edges`: every in-loop -> outside edge minus the
  caller's primary edge, with the degeneracy rule (all of a block's
  arms on ONE outside target — bypass threading cannot represent
  per-arm values) and forbidden targets as PARAMETERS.
Wired into: `loop_shape`, `lsimp`, `unswitch_shape` (+ unswitch/
unswitch_bif/rotate through it), `unroll_impl`, `lcf_impl`,
`cdfold_impl`. The four in the tail kept hand-rolled sweeps until the
last commit of this arc — their exit scans became `collect_exit_
edges` + the only-from-the-header/one-X policy read (the kernel's
degeneracy rule rejects exactly the both-arms-out headers their arm
gates refused one phase later; gate order verified, parity 146/146).

### 3.10 route_ns — the supplier-routing fixpoint (`calculate_supplier_routes`, `calculate_route_needs`)
**Reference.** Conceptually phi placement by FIXPOINT rather than by
frontiers: values flow from two source regions; every block reads
from SOME supplier, and a phi is BORN at a join of differing routes —
the join rule of Cytron et al. 1991, restated as a chaotic iteration
(this tree's own formulation, first written for tail duplication and
now the generic kernel).

**What was added.** The fixpoint (rounds capped at `nb+1`, in-place
visibility within a round, no-progress break, trailing sweep
pending -> dead for cycles) resolves per block: a pending pred blocks
it; no preds / a dead pred / more preds than the arity bound -> dead
(no phi possible there); one pred or all preds on ONE route ->
inherit it; else a phi is BORN (site id `2+k`, pred order preserved).
**The arity bound `MAX_JOIN_ARITY` is a parameter** — the binary-phi
IR passes 2, an N-ary IR passes its own bound; the kernel never
assumes two (the founding rule, stated as an argument). The need
closure (`calculate_route_needs`) is a DFS over the born-site DAG:
which values each SOURCE must carry and each born site must merge,
first-appearance order — the emission order the tail-dup commit
depends on.

### 3.11 alias_ns — allocation-identity resolution (`resolve_single_ids`)
**Reference.** Unification-based (one-point) points-to: Bjarne
Steensgaard, *Points-to Analysis in Almost Linear Time*, POPL 1996 —
copies UNIFY identities; the phi rule here ("single iff every arg
carries the SAME id") is the unification meet stated for phi nodes.

**What was added.** The table arrives PRE-SEEDED (the allocations;
the wrapper's Alloc pass seeds, the kernel resolves in place), the
record list is the def stream in CODE ORDER with kinds `copy | phi |
other`, N-ary phis (ALL args must agree; an absent arg is OBSCURE —
it may name another allocation), and the patience cap is a parameter
(the compiler's historical 64 rounds, kept as policy). Field-
insensitive, allocation-site identities only — exactly the "proves
exactly one X" question mem2reg's promotion gate asks.

### 3.12 mem_ns — the dominator-scoped forwarding table (`run_dom_scoped_table`)
**Reference.** Redundant-load elimination via remembered writes
scoped by dominance — the available-expressions discipline (Gary
Kildall, *A Unified Approach to Global Program Optimization*,
Proceedings of the 1st ACM SIGACT-SIGPLAN Symposium on Principles of
Programming Languages (POPL), 1973) with the SSA-era simplification: on the dominator tree, an
entry is in scope exactly at the blocks its defining site dominates,
so no dataflow fixpoint is needed (cf. Click 1995's combined
analysis); memory disambiguation by the may-kill predicate is the
classic alias-based kill rule.

**What was added.** The event discipline: per block, `store` (kill
everything the MAY_KILL predicate names — the memory model is policy
— then push the written slot; `val == none_v` = kill only), `load`
(first-match lookup on `(group, key)`, the hit recorded, the slot
re-recorded as holding THIS value), `clear` (the model lost track:
an obscure store, a call with handles). **Scope death goes by
creation SEQUENCE, never by `resize(mark)`** — positions would
regrow garbage after a deep join drop emptied them. `drop_at` is a
MASK input: the blocks whose entry state is not the dominator's
continuation (joins — sibling stores are invisible but invalidate
the ancestor's claim). A phi-placed memory model (mem2reg's) passes
all-zero. `ldelim.hpp` (load forwarding / dead-store elimination)
replays the hit records.

### 3.13 iv_ns + ai_ns — the interval lattice and the abstract-interpretation driver
**Papers.** Patrick Cousot & Radhia Cousot, *Abstract Interpretation:
A Unified Lattice Model for Static Analysis of Programs by
Construction or Approximation of Fixpoints*, POPL 1977 (the interval
lattice, widening, narrowing); widening at SELECTED points only:
Francois Bourdoncle, *Efficient Chaotic Iteration Strategies with
Widenings*, FMPA 1993.

**What was added.**
- `iv_ns` is the LATTICE alone — pure int64 math, no op vocabulary:
  the hull (`iv_join`), equality, the narrowing meet (kept only when
  still non-empty; bottom is "unreachable", not "contradiction"),
  and the trap-safety predicates (`excludes 0 / -1 / INT64_MIN` —
  what makes div/mod trap-eliminable).
- `ai_ns::solve_forward` is the DRIVER, the sccp precedent applied
  to ranges: propagation is the algorithm, transfers are policy
  (callbacks: `entry_top`, `refine_edge` — the fact an edge carries,
  `phi_join`, `transfer`). Plain rounds grow up from bottom, then
  WIDENING at the client's chosen points ONLY (widening everywhere
  re-inflates refined states — the Bourdoncle discipline), then
  NARROWING rounds intersect back down; a no-fixpoint give-up is
  reported and the caller falls back to sound TOP.
- `intervals.hpp` supplies the transfers (what Add does to
  intervals, what a taken comparison refines — the compiler's op
  vocabulary); `trapelim.hpp` consumes the trap-safety predicates;
  `jthread.hpp` re-derives per-edge states through the same
  machinery.

### 3.14 The shared key — `mixed_sort_key`
**Reference.** Canonical forms for congruence: Alpern, Wegman, Zadeck
1988 (commutative canonicalization as part of the congruence test).

**What was added.** One orderable tag for a var-or-const operand:
absent -> 0, a known const -> `(1 << 63) ^ value` (constants sort
after every var id, by value), else the var id. The SAME tag
discipline keys vn's bin operands and orders canon's commutative
operands — one canonical shape per equivalence class is what makes
CSE hit.

### 3.15 region_uses_escape — jthread's elbow-leak scan
**Reference.** Liveness of a region's definitions outside it — the
standard live-variable question (Muchnick 1997 §13.4) stated as a
boolean, order-free.

**What was added.** `def_site_of_var` + `args_of_site` + a site
window `[lo, hi)`: a use at a site OUTSIDE the window of a var whose
def site is INSIDE escapes. `jthread.hpp` uses it as the soundness
half of redirecting an edge past an elbow (nothing the elbow defines
may be read after it), then re-derives the per-edge interval state
(3.13) to decide the elbow's comparison on that edge.

### 3.16 ipa_ns — interprocedural parameter liveness (`calculate_live_params`)
**Reference.** Dead-argument elimination as IPO: Muchnick 1997
§18.4; the fixpoint over forwarded-parameter edges is the Kildall
worklist discipline applied to the call graph.

**What was added.** `alive[g][p]` starts from the HARD roots
(operands, observed values, surviving base vars — client policy) and
grows through the SOFT edges: parameter `p` of `g` forwarded as
argument `q` of a call to `h` is alive once `h`'s `q` is alive (or
`h` is not a candidate, so its params can never die). The least
fixpoint — alive only grows — is unique regardless of visit order;
the scan order (g ascending, p ascending, softs in record order) is
kept anyway so the fixpoint after any bounded patience is the
compiler's historical one. `deadargs.hpp` rewrites the survivors.

### 3.17 equi_ns — identical-code folding (`refine_by_keys`)
**Papers.** The refinement-to-fixpoint discipline: Robert Paige &
Robert Tarjan, *Three Partition Refinement Algorithms*, SIAM J.
Computing 16(5), 1987; the practice of merging structurally identical
functions is the linker/ICF and LLVM `MergeFunctions` tradition.

**What was added.** One refinement round as a kernel: every
candidate joins the class of the FIRST candidate with an equal key
(rep-of-first-equal), changed reported against the previous map. The
caller (`icf.hpp`) seeds with wildcarded keys, then re-serializes
with the CURRENT representatives (call targets and phi feeds keyed
by rep) until a round changes nothing — the verified fixpoint the
fold commits at — or the round cap declines. The keys are opaque:
what "identical" means is the client's serialization policy; the
grouping is the algorithm.

---

## 4. Pass-side algorithms that are policy or rewrite rules

These have no kernel because their content IS the compiler's policy
or transfer semantics (the founding rule cuts the other way here —
extracting them would just move the policy around):

- **fold / fold_impl** — the constant evaluator: the language's
  wrapping/trapping arithmetic (`w_add`..`w_mod`) IS the semantics.
  Reference for the wrapping/trap model: the language spec in
  `README.md`; the div/mod trap set (`0`, `MIN/-1`) is the hardware
  behavior x86-64 traps on.
- **reassoc / linform / canon** — algebraic normalization to one
  canonical shape per equivalence class (canon orders commutative
  operands by the kernel's `mixed_sort_key`; reassoc is the
  associative/identity/distributive rewrite set; linform the linear-
  form normalizer lcf/cdfold's matchers consume). Canonical-form
  discipline per Alpern-Wegman-Zadeck 1988.
- **divmagic** — division/modulo by constant into mulhi+shift.
  **Paper:** Torbjorn Granlund & Peter Montgomery, *Division by
  Invariant Integers using Multiplication*, PLDI 1994; cf. Henry
  Warren, *Hacker's Delight*, ch. 10 (the exactness certificate the
  pass checks in 128-bit arithmetic per divisor). The reciprocal
  itself comes from `mjz_lib` (already a library function).
- **ifconv** — provably-binary selects: both arms materialize a
  constant and the range analysis (3.13) proves the condition in
  `[0,1]`, so `dst = K0 + (K1-K0)*c`. The select-of-constants
  algebra; the range proof is the kernel's.
- **splice rebuild, unroll/rotate/unswitch/lcf/taildup commit tails,
  cdfold's closed form, scctco's loop lowering, tailrec's
  [Call;Ret] -> back-edge** — IR surgery over the kernel-produced
  plans. **tailrec reference:** Guy Steele, *Debunking the "Expensive
  Procedure Call" Myth*, MIT AI Memo 443, 1977 (proper tail calls).
  **scctco** composes Tarjan 1972 (1.5) with that same tail-call
  rewrite, grouped by SCC.
- **argpromo / constcall / callcse / fnspec** — IPO rewrite rules
  over the call graph (1.5) and the const/alias/interval kernels.

---

## 5. The verification discipline (what "byte-identical" means here)

Every kernel conversion in this arc was gated by the repo's standing
battery: `scripts/parity_diff.sh` (the committed corpus, 146 runs,
byte-identical), `scripts/fuzz_parity.sh` (old-vs-new differential
fuzz windows, identical modulo timing), the selftest (207 checks,
every optimizer stat line byte-identical), and the unit tests
(6,476 checks — the kernels pinned standalone, including the
policy-free/placement-wrapper DISAGREEMENT cases). The rule: the
kernel may reorganize the computation, but the pass outputs may not
move a byte.

## 6. Index of papers

| # | Paper | Used in |
| --- | --- | --- |
| 1 | Lengauer & Tarjan, *A Fast Algorithm for Finding Dominators in a Flowgraph*, TOPLAS 1979 | dom.hpp |
| 2 | Cytron, Ferrante, Rosen, Wegman, Zadeck, *Efficiently Computing SSA Form and the Control Dependence Graph*, TOPLAS 1991 | idf/phis/ssa_generic, destruct, mem2reg, route_ns |
| 3 | Tarjan, *Depth-First Search and Linear Graph Algorithms*, SIAM J. Comput. 1972 | scc.hpp, callgraph, scctco |
| 4 | Allen, *Control Flow Analysis*, SIGPLAN Notices 5(7), 1970; Hecht & Ullman, *Characterizations of Reducible Flow Graphs*, J. ACM 21(3), 1974 | loop.hpp, shape_ns |
| 5 | Fulkerson & Gross 1965; Tarjan & Yannakakis 1984; Hack, Grund & Goos, *Register Allocation for Programs in SSA Form*, CC 2006 | color.hpp, regalloc |
| 6 | Briggs, Cooper, Harvey & Simpson, *Practical Improvements to the Construction and Destruction of SSA Form*, CC 1998 | destruct, trivial-phi pick, pcopy_ns |
| 7 | Rastello & Bouchez Tichadou (eds.), *SSA-based Compiler Design*, Apress, 2015 | pcopy_ns (lost-copy/swap taxonomy) |
| 8 | Alpern, Wegman & Zadeck, *Detecting Equality of Variables in Programs*, POPL 1988 | vn_ns, mixed_sort_key, canon |
| 9 | Briggs, Cooper, Harvey & Simpson, *Value Numbering*, SPE 1997 | vn_ns (dominator-scoped table) |
| 10 | Click, *Combining Analyses, Combining Optimizations*, PhD 1995 | vn_ns, mem_ns |
| 11 | Wegman & Zadeck, *Constant Propagation with Conditional Branches*, TOPLAS 1991 | sccp_ns |
| 12 | Kildall, *A Unified Approach to Global Program Optimization*, POPL 1973 | mem_ns (availability), dataflow generally |
| 13 | Muchnick, *Advanced Compiler Design and Implementation*, 1997 | dce/licm/copyprop/splice/deadargs framing |
| 14 | Allen & Kennedy, *Optimizing Compilers for Modern Architectures*, 2002 | licm legality |
| 15 | Steensgaard, *Points-to Analysis in Almost Linear Time*, POPL 1996 | alias_ns (unification meet) |
| 16 | Cousot & Cousot, *Abstract Interpretation*, POPL 1977 | iv_ns/ai_ns |
| 17 | Bourdoncle, *Efficient Chaotic Iteration Strategies with Widenings*, 1993 | ai_ns (widening points) |
| 18 | Paige & Tarjan, *Three Partition Refinement Algorithms*, SIAM J. Comput. 1987 | equi_ns (icf) |
| 19 | Granlund & Montgomery, *Division by Invariant Integers using Multiplication*, PLDI 1994; Warren, *Hacker's Delight* ch. 10 | divmagic |
| 20 | Steele, *Debunking the "Expensive Procedure Call" Myth*, 1977 | tailrec, scctco |
| 21 | McCarthy, *Recursive Functions of Symbolic Expressions...*, 1960 | mark_ns (mark-sweep heritage) |
