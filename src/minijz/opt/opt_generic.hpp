#pragma once
// === opt_generic.hpp ========================================
// The optimization kernels, GENERIC over index space -- the ssa_ns
// treatment extended to every algorithm the opt/ tree runs. The
// KERNELS now live in the library (mjz_lib/mjz_lib/opt/, one family
// per header, aggregated by opt/alllib.hpp on the graph_ns include
// chain -- the home phis.hpp and the SSA kernel already found); this
// header is the compiler's adapter: the founding rule's text, the
// shared program model, and the include the passes grew up with.
//
// THE RULE (the branch's founding directive): an ALGORITHM must not
// depend on the way the compiler works. Binary phis, the >=2-pred
// rule, the <2-defs skip, root policies, trap policies, hoist
// legality -- those are the COMPILER'S policies and live in the
// passes (the wrappers); the library holds the relation itself,
// stated for ANY arity and ANY policy:
//   * opt/mark.hpp     -- mark-sweep liveness over the def/use
//                         bipartite graph (dce's core; the same
//                         fixpoint underpins every "is it used"
//                         question in the tree)
//   * opt/vn.hpp       -- dominator-scoped value numbering with
//                         scoped table truncation, operand
//                         resolution, const keying by VALUE and
//                         commutative canonicalization (cse and gvn
//                         are the same walk with different entry
//                         kinds), plus the mixed sort key the
//                         canonicalizer orders operands by
//   * opt/sccp.hpp     -- the sparse conditional constant engine:
//                         two worklists (edges + vars), executable-
//                         edge tracking, phi merging over EXECUTABLE
//                         in-edges only, N-ary phi records, monotone
//                         descent, the step guard. The lattice is
//                         TOP/CONST/OVERDEF over int64 -- the
//                         transfer semantics stay with the client (a
//                         callback on scalars, the std::sort-comparator
//                         precedent)
//   * opt/pcopy.hpp    -- parallel-copy sequentialization (the
//                         lost-copy / swap-cycle problem of SSA
//                         destruction) and the critical-edge split
//                         plan
//   * opt/cfg.hpp      -- the reachability flood (unreach), the
//                         splice pairing plan (single-pred/single-succ
//                         absorption as pure CFG math), and the
//                         region-uses-escape scan (jthread's elbow
//                         leak, stated for any IR)
//   * opt/inv.hpp      -- loop-invariant selection: the unique-outside-
//                         pred rule, the invariance predicate over def
//                         blocks/dominance/consts, and the scan
//                         discipline (header first, then member row)
//   * opt/ren.hpp      -- the copy-rename builder with the crossed-pair
//                         self-loop guard (fuzz seed 7406's lesson)
//                         and the capped resolve
//   * opt/trivphi.hpp  -- the trivial-phi pick: all non-self args
//                         equal -> that arg (the N-ary form of dce's
//                         phi collapse)
//   * opt/shape.hpp    -- natural-loop shape as pure CFG math (the
//                         header-pred classification, the canonical
//                         test-first shape, the exit-edge census)
//   * opt/route.hpp    -- the supplier-routing fixpoint (tail
//                         duplication's mini SSA construction) and
//                         its need closure, generic over arity
//   * opt/alias.hpp    -- allocation-identity resolution (the
//                         copy/phi fixpoint)
//   * opt/mem.hpp      -- the dominator-scoped forwarding table
//                         (load elimination)
//   * opt/interval.hpp -- the interval lattice (hull, narrowing
//                         meet, the trap-safety predicates)
//   * opt/ai.hpp       -- the forward abstract-interpretation driver
//                         (plain rounds, widening at the client's
//                         chosen points only, narrowing back down)
//   * opt/ipa.hpp      -- the interprocedural liveness fixpoint
//                         (dead arguments)
//   * opt/equi.hpp     -- class refinement over serial keys
//                         (identical-code folding)
//   * opt/liveness.hpp -- backward block liveness (the fixpoint
//                         regalloc runs) + the interference rows
//                         (defs vs live-outs, entry defs pairwise):
//                         the allocator's graph, stated for any client
//   * opt/layout.hpp   -- reverse postorder (slot-order DFS) + the
//                         trace chain plan (smallest-score open
//                         successor, ties to the earliest slot): the
//                         emission order every native backend lays out
//   * opt/escape.hpp   -- the copy alias closure + the positional
//                         escape scan (stack_alloc's non-escaping
//                         array test, any handle-position policy)
//   * opt/dse.hpp      -- the in-window dead-store kill table: the
//                         same-value and overwrite rules over exact
//                         (group, key) slots, with the client's
//                         window policy (what breaks it: anything
//                         that may trap, observe, or write inexact)
//                         as the soundness core
//
// THE PROGRAM MODEL (identical to ssa_ns, one paragraph each):
//   * blocks  0..n_blocks-1; CFG as slot rows (succs_of_block, the
//     RAW arity -- duplicates and none_v holes kept, exactly like
//     Fn::succ for the binary IR) and positional pred rows.
//   * vars    0..n_vars-1, pseudo-variables (vregs, slots, whatever
//     the client is numbering).
//   * sites   0..n_sites-1, client ids for instruction slots; rows
//     sites_of_block give each block's sites in program order. THE
//     WHOLE IR KNOWLEDGE the kernels need is: which vars a site
//     reads (args_of_site), which var it defines (site info), and
//     for phis the (pred, arg) entries (pred-keyed, N-ary).
//
// PIPELINE POSITION. Not positioned: the kernels are library leaves
// now; their in-tree clients are the opt/ passes (dce/cse/gvn/
// copyprop/sccp/unreach/splice/licm), ir/destruct.hpp and the shared
// sweep helper; every client keeps its theory banner -- what changed
// in each is that the hand-rolled core became a table replay (see
// each pass's banner note). The fuzzers live in apps/fuzz_kernels.hpp
// (the opt.* components: every kernel, unit by unit, against
// references coded from the definition).
//
#include "../graph_lib.hpp"
