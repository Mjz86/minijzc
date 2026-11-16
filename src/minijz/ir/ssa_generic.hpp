#pragma once
// === ssa_generic.hpp ========================================
// The SSA construction kernel, GENERIC over index space -- the graph_ns
// treatment applied to the compiler's own components. The PURE KERNELS
// (the event vocabulary, rename_ssa, the reflow plan, the merge-chain
// plan) now live in the library, mjz_lib/mjz_lib/graph/ssa.hpp, next
// to the pure Cytron placement they build on (graph/phis.hpp); this
// header keeps THE IR'S POLICY WRAPPER and the design record. No
// Instr, no Fn, no phi instruction appears in the kernels; they are
// pure functions from graph-shaped inputs to graph-shaped tables, so
// they compile and unit-test standalone (tests/unit_tests.cpp
// section 16).
//
// THE PROGRAM MODEL (what "generic" concretely means here):
//   * blocks     0..n_blocks-1, a CFG as two CSR forests over the SAME
//     edge set: succs_of_block (SLOT order, duplicates kept -- a cbr
//     with both arms on one target contributes TWO entries, exactly
//     like Fn::succ) and preds_of_block (POSITIONAL rows, ascending
//     source order, exactly like Fn::preds -- a phi entry's index in
//     the row is its edge's identity).
//   * variables  0..n_vars-1 -- pseudo-variables. For ssa_construct
//     these are the source vregs; for mem2reg the promoted array
//     slots; the layer never asks.
//   * sites      0..n_sites-1 -- client-chosen ids for every use/def
//     OCCURRENCE (an operand slot, an init, a phi result). The layer
//     hands them back untouched; ids only have to be dense.
//   * events     per block, an ordered (kind, var, site) stream: the
//     block's uses and defs in program order. This is the WHOLE IR
//     knowledge the layer needs -- which instruction fields carry
//     uses and defs stays client-side (an.hpp knows a/b2/c2/args/dst;
//     mem2reg knows Load/Store/init).
//
// THE THREE HARD PARTS, AND HOW EACH GOES GENERIC:
//
//   * MULTI-BRANCH PHI. A phi is not an instruction here -- it is a
//     RECORD: (block, var) plus one (pred, incoming) pair per entry of
//     the block's pred row. Arity is therefore the pred count, ANY
//     pred count: the layer never truncates to two arguments, and a
//     3-pred join gets a 3-entry record as naturally as the IR-shaped
//     2-pred join (unit test 16e proves one directly -- the compiler's
//     own Instr cannot even express it). How a record becomes phi
//     instructions is the CLIENT's lowering: this tree's binary IR
//     pre-normalizes >2-pred joins into merge chains (pred_norm, now
//     driven by the generic calculate_merge_chain_plan), so the
//     records it lowers always have <= 2 entries; an N-ary-phi IR
//     would consume the same records row-for-row. PLACEMENT ITSELF
//     has the same shape: the pure Cytron relation (every var, any
//     def count, any pred count) lives in graph_ns (graph/phis.hpp --
//     calculate_phi_placements is var-major, calculate_inv_phi_
//     placements the block-major transpose); the calculate_phi_
//     placements below is THIS IR's policy wrapper (the <2-defs skip
//     and the >=2-pred-slot rule are compiler policy, and they live
//     with the compiler, not in the algorithm).
//   * VARIABLE RENAMING. Cytron's dominator-tree walk with per-variable
//     version stacks, driven by events instead of instruction fields
//     (rename_ssa, graph/ssa.hpp): a use binds its site to the stack
//     top's SITE, a def pushes its own site, phi results are defs the
//     layer synthesizes at block heads, and phi args are seeded from
//     each pred's END-OF-BLOCK stack top (the value that flows along
//     that edge -- the exact discipline an.hpp 8d and mem2reg's walk
//     hand-rolled). The output is pure data: reaching_def[site] (which
//     def site a use reads), def_ordinal[site] (the classic
//     stack-depth-at-push version number), and the phi records with
//     their N-ary entries. The client replays visit_order to mint its
//     real vreg ids in the same order the old interleaved walk did --
//     which is why the rewired compiler is byte-identical.
//   * INSTRUCTION REMOVAL. Two answers, one per vantage point. From
//     the layer's vantage, removal is a NON-EVENT: the client simply
//     emits no def/use events for an instruction it is dropping
//     (mem2reg's dead stores) -- the SSA tables never reference it, so
//     construction over a partially-removed block is the same function
//     as construction over the full one. From the materialization
//     vantage (the flat code vector with block ranges that must be
//     rebuilt: phis prepended, dead instructions dropped, anchored
//     insertions after survivors), the generic part is the INDEX MATH:
//     calculate_reflow_plan (graph/ssa.hpp) turns (old ranges, keep
//     mask, per-block head counts, per-index after counts) into (new
//     ranges, old->new map, total) -- ssa_construct's phi-prepend
//     rebuild, mem2reg's drop+prepend+init rebuild and its dead-alloc
//     sweep all consume the one plan (THREE dedups, the house
//     precedent).
//
// PIPELINE POSITION. Not positioned: the kernels are library leaves
// (graph/ssa.hpp). Their two in-tree clients are an.hpp (ssa_
// construct: source vregs over the whole function) and opt/mem2reg.
// hpp (slots over promoted arrays); pred_norm.hpp takes the
// merge-chain planner. Both clients keep their theory banners; what
// changed in each is the hand-rolled walk became a table replay (see
// their 8b/8d sections).
//

// self-contained: the umbrella brings the kernels (graph/ssa.hpp,
// graph/phis.hpp) and the vocabulary they share (version_t, MJZ_CX_FN,
// gns, uintlen_t); the wrapper's own std deps follow
#include "../graph_lib.hpp"

#include <span>
#include <vector>

// ============================================================================
// ssa_ns: this file's share is THE IR POLICY WRAPPER (the pure kernels
// live in graph/ssa.hpp; graph_ns conventions: uintlen_t ids,
// version_t-templated, CSR forests, MJZ_CX_FN noexcept, uintlen_t(-1)
// as NONE)
// ============================================================================
namespace mjz::ssa_ns {

// ---- placement ------------------------------------------------------------
// THE COMPILER'S POLICY WRAPPER over the pure placement (graph_ns,
// graph/phis.hpp: the Cytron relation itself -- every var, any def
// count, any pred count, zero IR assumptions, N-arity by construction).
// This IR keeps exactly two policies, applied to the pure var-major
// rows before the transpose to block-major:
//   * vars with < 2 def blocks place NOTHING (the classic
//     construction; the language's builder guarantees their single
//     def dominates every use that matters -- the pure layer WOULD
//     place their DF(def) phis, and unit test 16i pins that gap),
//   * blocks with < 2 incoming edge slots place nothing (the IR's own
//     phi rule; duplicates count -- a [p,p] row from cbr(c,Y,Y) is 2;
//     the pure layer places at one-pred blocks when the theory says
//     so, e.g. an entry sitting in its own cycle).
// Output contract unchanged from the hand-rolled era: rows = the
// variables needing phis in block b, in ASCENDING variable order --
// the order the old per-client loops produced and the phi-placeholder
// emission order depends on. (Name note: the pure graph_ns function
// of the same name returns the VAR-major relation; this wrapper
// returns the transposed, policy-filtered direction the IR consumes.)
template <version_t version_v, class Rdf = std::span<const uintlen_t>,
          class Rpr = std::span<const uintlen_t>>
MJZ_CX_FN gns::treversal_result_t<version_v> calculate_phi_placements(
    const gns::basic_forest_t<version_v, Rdf> &dominance_frontiers,
    const gns::basic_forest_t<version_v, Rpr> &preds_of_block,
    const auto &def_blocks_of_var) noexcept {
  const uintlen_t nb = uintlen_t(std::ranges::size(preds_of_block.range()));
  // the pure placement: row v = IDF(defs(v)), EVERY variable (the
  // worklist also runs for single-def vars -- one pop plus a usually
  // empty frontier row -- whose rows the policy below drops)
  const gns::treversal_result_t<version_v> pure =
      gns::calculate_phi_placements<version_v>(dominance_frontiers,
                                               def_blocks_of_var);
  auto pure_rows = pure.range();
  auto pred_rows = preds_of_block.range();
  std::vector<std::vector<uintlen_t>> rows(nb);
  uintlen_t v = 0;
  auto def_it = std::ranges::begin(def_blocks_of_var);
  for (const auto &pure_row : pure_rows) {
    const bool multi_def = std::ranges::size(*def_it) >= 2;
    ++def_it;
    if (multi_def) {
      for (uintlen_t blk : pure_row) {
        if (blk >= nb)
          continue;
        if (std::ranges::size(pred_rows[blk]) < 2)
          continue;
        bool dup = false;
        for (uintlen_t x : rows[blk])
          dup |= (x == v);
        if (!dup)
          rows[blk].push_back(v);
      }
    }
    ++v;
  }
  return gns::make_basic_forest<version_v>(rows);
}
}; // namespace mjz::ssa_ns
