#pragma once
// === ssa_generic.hpp ========================================
// The SSA construction kernel, GENERIC over index space -- the graph_ns
// treatment applied to the compiler's own components. Everything here
// speaks uintlen_t: blocks, variables, sites, versions. No Instr, no
// Fn, no phi instruction ever appears; the layer is a pure function
// from graph-shaped inputs to graph-shaped tables, so it compiles and
// unit-tests standalone (tests/unit_tests.cpp section 16) and could
// lift into mjz_lib's graph_ns unchanged.
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
//     driven by the generic calculate_merge_chain_plan below), so the
//     records it lowers always have <= 2 entries; an N-ary-phi IR
//     would consume the same records row-for-row.
//   * VARIABLE RENAMING. Cytron's dominator-tree walk with per-variable
//     version stacks, driven by events instead of instruction fields:
//     a use binds its site to the stack top's SITE, a def pushes its
//     own site, phi results are defs the layer synthesizes at block
//     heads, and phi args are seeded from each pred's END-OF-BLOCK
//     stack top (the value that flows along that edge -- the exact
//     discipline an.hpp 8d and mem2reg's walk hand-rolled). The output
//     is pure data: reaching_def[site] (which def site a use reads),
//     def_ordinal[site] (the classic stack-depth-at-push version
//     number), and the phi records with their N-ary entries. The
//     client replays visit_order to mint its real vreg ids in the same
//     order the old interleaved walk did -- which is why the rewired
//     compiler is byte-identical.
//   * INSTRUCTION REMOVAL. Two answers, one per vantage point. From
//     the layer's vantage, removal is a NON-EVENT: the client simply
//     emits no def/use events for an instruction it is dropping
//     (mem2reg's dead stores) -- the SSA tables never reference it, so
//     construction over a partially-removed block is the same function
//     as construction over the full one. From the materialization
//     vantage (the flat code vector with block ranges that must be
//     rebuilt: phis prepended, dead instructions dropped, anchored
//     insertions after survivors), the generic part is the INDEX MATH:
//     calculate_reflow_plan turns (old ranges, keep mask, per-block
//     head counts, per-index after counts) into (new ranges,
//     old->new map, total) -- ssa_construct's phi-prepend rebuild,
//     mem2reg's drop+prepend+init rebuild and its dead-alloc sweep all
//     consume the one plan (THREE dedups, the house precedent).
//
// PIPELINE POSITION. Not positioned: this header is a library leaf.
// Its two in-tree clients are an.hpp (ssa_construct: source vregs over
// the whole function) and opt/mem2reg.hpp (slots over promoted
// arrays); pred_norm.hpp takes the merge-chain planner. Both clients
// keep their theory banners; what changed in each is the hand-rolled
// walk became a table replay (see their 8b/8d sections).

#include "../graph_lib.hpp"

// ============================================================================
// ssa_ns: SSA construction over index space (graph_ns conventions:
// uintlen_t ids, version_t-templated, CSR forests, MJZ_CX_FN noexcept,
// uintlen_t(-1) as NONE)
// ============================================================================
namespace mjz::ssa_ns {

inline constexpr uintlen_t ssa_none_v = uintlen_t(-1);
inline constexpr uintlen_t ssa_use_v = 0; // event kinds
inline constexpr uintlen_t ssa_def_v = 1;

// one use or def of one variable at one client site
struct ssa_event_t {
  uintlen_t kind{}; // ssa_use_v / ssa_def_v
  uintlen_t var{};
  uintlen_t site{};
};

// ---- placement ------------------------------------------------------------
// Minimal-SSA phi placement (Cytron): variable v needs a phi at block Y
// iff Y is in the iterated dominance frontier of v's def blocks AND Y has
// >= 2 incoming edge slots (with duplicates -- a [p,p] row from cbr(c,Y,Y)
// counts, matching the IR's own rule). Vars with < 2 def blocks are
// skipped outright (the classic construction; the language's builder
// guarantees their single def dominates every use that matters).
// Rows of the returned forest = the variables needing phis in block b,
// in ASCENDING variable order -- the order the old per-client loops
// produced and the phi-placeholder emission order depends on.
template <version_t version_v, class Rdf = std::span<const uintlen_t>,
          class Rpr = std::span<const uintlen_t>>
MJZ_CX_FN gns::treversal_result_t<version_v> calculate_phi_placements(
    const gns::basic_forest_t<version_v, Rdf> &dominance_frontiers,
    const gns::basic_forest_t<version_v, Rpr> &preds_of_block,
    const auto &def_blocks_of_var) noexcept {
  const uintlen_t nb = uintlen_t(std::ranges::size(preds_of_block.range()));
  std::vector<std::vector<uintlen_t>> rows(nb);
  uintlen_t v = 0;
  for (const auto &def_row : def_blocks_of_var) {
    if (std::ranges::size(def_row) >= 2) {
      std::vector<uintlen_t> idf{};
      gns::calculate_iterated_dominance_frontiers_given_dominance_frontiers<
          version_v>(idf, dominance_frontiers, def_row);
      for (uintlen_t blk : idf) {
        if (blk >= nb)
          continue;
        if (std::ranges::size(preds_of_block.range()[blk]) < 2)
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

// ---- renaming -------------------------------------------------------------
// The Cytron walk's result, pure data (see the banner). phi records are
// enumerated (block ascending, row order): phi index p belongs to
// phi_block[p], merges phi_var[p], defines phi_def_site[p], and holds
// entries [phi_entry_begin[p], phi_entry_begin[p+1]) -- one per entry of
// the block's pred row, positional: entry i is the value arriving along
// the edge from preds_of_block[phi_block[p]][i]. Entries keep their
// (ssa_none_v, ssa_none_v) initialization when never seeded: a pred the
// dominator walk never reaches (unreachable), or a duplicate-pred row
// where only the LAST slot of a repeated pred is written (both are the
// hand-rolled walk's behavior, preserved exactly).
template <version_t version_v> struct ssa_renaming_t {
  std::vector<uintlen_t> visit_order{};  // blocks, rename-DFS pre-order
  std::vector<uintlen_t> reaching_def{}; // site -> def site / ssa_none_v
  std::vector<uintlen_t> def_ordinal{};  // def site -> version number
  std::vector<uintlen_t> phi_block{};
  std::vector<uintlen_t> phi_var{};
  std::vector<uintlen_t> phi_def_site{};
  std::vector<uintlen_t> phi_entry_begin{}; // n_phis + 1 offsets
  std::vector<uintlen_t> phi_entry_pred{};
  std::vector<uintlen_t> phi_entry_def{};
};

// dom tree rows drive the DFS (children in row order -- the same forest
// An::dom_tree always was, so visit_order matches the old walk's
// allocation order to the block); succs_of_block seeds successor phis in
// SLOT order with duplicates (both arms of a cbr); preds_of_block gives
// entry positions; phis_of_block is calculate_phi_placements's forest;
// events_of_block rows are the client streams. Phi def sites are
// [site_count, site_count + n_phis): the client reserves that id range.
template <version_t version_v, class Rdt = std::span<const uintlen_t>,
          class Rsu = std::span<const uintlen_t>,
          class Rpr = std::span<const uintlen_t>,
          class Rph = std::span<const uintlen_t>>
MJZ_CX_FN ssa_renaming_t<version_v>
rename_ssa(uintlen_t var_count, uintlen_t site_count,
           const gns::basic_forest_t<version_v, Rdt> &dominator_tree,
           const gns::basic_forest_t<version_v, Rsu> &succs_of_block,
           const gns::basic_forest_t<version_v, Rpr> &preds_of_block,
           const gns::basic_forest_t<version_v, Rph> &phis_of_block,
           const auto &events_of_block) noexcept {
  ssa_renaming_t<version_v> ret{};
  const uintlen_t nb = uintlen_t(std::ranges::size(dominator_tree.range()));
  if (!nb)
    return ret;
  auto dom_rows = dominator_tree.range();
  auto succ_rows = succs_of_block.range();
  auto pred_rows = preds_of_block.range();
  auto phi_rows = phis_of_block.range();
  asserts(uintlen_t(std::ranges::size(succ_rows)) == nb);
  asserts(uintlen_t(std::ranges::size(pred_rows)) == nb);
  asserts(uintlen_t(std::ranges::size(phi_rows)) == nb);
  // -- phi records: (block ascending, row order); entries per pred row --
  uintlen_t n_phis = 0, n_entries = 0;
  for (uintlen_t b = 0; b < nb; ++b) {
    n_phis += uintlen_t(std::ranges::size(phi_rows[b]));
    n_entries += uintlen_t(std::ranges::size(phi_rows[b])) *
                 uintlen_t(std::ranges::size(pred_rows[b]));
  }
  ret.phi_block.reserve(n_phis);
  ret.phi_var.reserve(n_phis);
  ret.phi_def_site.reserve(n_phis);
  ret.phi_entry_begin.reserve(n_phis + 1);
  ret.phi_entry_pred.assign(n_entries, ssa_none_v);
  ret.phi_entry_def.assign(n_entries, ssa_none_v);
  std::vector<uintlen_t> phi_ord_base(nb, 0); // row start ordinals
  {
    uintlen_t ord = 0, ent = 0;
    for (uintlen_t b = 0; b < nb; ++b) {
      phi_ord_base[b] = ord;
      for (uintlen_t v : phi_rows[b]) {
        ret.phi_block.push_back(b);
        ret.phi_var.push_back(v);
        ret.phi_def_site.push_back(site_count + ord);
        ret.phi_entry_begin.push_back(ent);
        ent += uintlen_t(std::ranges::size(pred_rows[b]));
        ++ord;
      }
    }
    ret.phi_entry_begin.push_back(ent);
    asserts(ord == n_phis);
    asserts(ent == n_entries);
  }
  ret.reaching_def.assign(site_count + n_phis, ssa_none_v);
  ret.def_ordinal.assign(site_count + n_phis, ssa_none_v);
  // -- the walk: version stacks of SITES; undo log = the push discipline --
  std::vector<std::vector<uintlen_t>> stacks(var_count);
  std::vector<uintlen_t> undo_log{};
  struct frame_t {
    uintlen_t block, child_idx, undo_len;
  };
  std::vector<frame_t> frames{};
  auto process = [&](uintlen_t b) noexcept {
    ret.visit_order.push_back(b);
    // phi defs first: they are the block head (the phis-first invariant)
    for (uintlen_t pi = 0; pi < uintlen_t(std::ranges::size(phi_rows[b]));
         ++pi) {
      const uintlen_t phi = phi_ord_base[b] + pi;
      const uintlen_t v = ret.phi_var[phi];
      const uintlen_t site = ret.phi_def_site[phi];
      ret.def_ordinal[site] = uintlen_t(stacks[v].size());
      stacks[v].push_back(site);
      undo_log.push_back(v);
    }
    // client events in program order
    for (const ssa_event_t &ev : events_of_block[b]) {
      if (ev.kind == ssa_use_v) {
        ret.reaching_def[ev.site] =
            stacks[ev.var].empty() ? ssa_none_v : stacks[ev.var].back();
      } else {
        ret.def_ordinal[ev.site] = uintlen_t(stacks[ev.var].size());
        stacks[ev.var].push_back(ev.site);
        undo_log.push_back(ev.var);
      }
    }
    // seed successor phis: the value at the END of this block is the
    // stack top now; it arrives along every succ slot (duplicates: the
    // LAST slot of a repeated pred wins, like the hand-rolled lookup)
    for (uintlen_t s : succ_rows[b]) {
      if (s >= nb)
        continue; // ssa_none_v and junk both bail out
      for (uintlen_t pi = 0; pi < uintlen_t(std::ranges::size(phi_rows[s]));
           ++pi) {
        const uintlen_t phi = phi_ord_base[s] + pi;
        const uintlen_t v = ret.phi_var[phi];
        uintlen_t entry = ssa_none_v;
        auto pr = pred_rows[s];
        for (uintlen_t j = 0; j < uintlen_t(std::ranges::size(pr)); ++j)
          if (pr[j] == b)
            entry = j;
        asserts(entry != ssa_none_v); // CFG-consistent inputs
        if (entry == ssa_none_v)
          continue;
        const uintlen_t at = ret.phi_entry_begin[phi] + entry;
        ret.phi_entry_pred[at] = b;
        ret.phi_entry_def[at] =
            stacks[v].empty() ? ssa_none_v : stacks[v].back();
      }
    }
  };
  // the frame's undo mark is the log length BEFORE the block is
  // processed, so exiting unwinds the block's OWN pushes too -- the
  // old walk's `pushed`-count epilogue, as one integer per frame
  const uintlen_t root_mark = uintlen_t(undo_log.size());
  process(0);
  frames.push_back({0, 0, root_mark});
  while (!frames.empty()) {
    frame_t &fr = frames.back();
    auto ch = dom_rows[fr.block];
    if (fr.child_idx < uintlen_t(std::ranges::size(ch))) {
      const uintlen_t next = ch[fr.child_idx++];
      const uintlen_t mark = uintlen_t(undo_log.size());
      process(next);
      frames.push_back({next, 0, mark});
    } else {
      const uintlen_t undo_len = fr.undo_len;
      frames.pop_back();
      while (undo_log.size() > undo_len) {
        stacks[undo_log.back()].pop_back();
        undo_log.pop_back();
      }
    }
  }
  return ret;
}

// ---- reflow: the generic rebuild-with-removal plan -------------------------
// The flat-code-vector rebuild every SSA-shaped pass hand-rolls: some
// instructions die (keep[i] = 0), some blocks gain head instructions
// (head_count[b], the phi placeholders), some survivors gain trailing
// insertions (after_count[i], mem2reg's zero-inits after an Alloc --
// counted only where keep != 0). The plan is the pure index math: new
// block ranges and the old->new map; the client walks it to emit real
// instructions. old_to_new[i] = ssa_none_v exactly when keep[i] = 0.
template <version_t version_v> struct reflow_plan_t {
  std::vector<uintlen_t> new_begin{};
  std::vector<uintlen_t> new_end{};
  std::vector<uintlen_t> old_to_new{};
  uintlen_t total{};
};

template <version_t version_v>
MJZ_CX_FN reflow_plan_t<version_v>
calculate_reflow_plan(const auto &old_begin, const auto &old_end,
                      const auto &keep, const auto &head_count,
                      const auto &after_count) noexcept {
  const uintlen_t nb = uintlen_t(std::ranges::size(old_begin));
  const uintlen_t old_total = uintlen_t(std::ranges::size(keep));
  asserts(std::ranges::size(old_end) == std::ranges::size(old_begin));
  asserts(std::ranges::size(head_count) == std::ranges::size(old_begin));
  asserts(std::ranges::size(after_count) == keep.size());
  reflow_plan_t<version_v> ret{};
  ret.new_begin.reserve(nb);
  ret.new_end.reserve(nb);
  ret.old_to_new.assign(old_total, ssa_none_v);
  uintlen_t w = 0;
  for (uintlen_t b = 0; b < nb; ++b) {
    ret.new_begin.push_back(w);
    w += uintlen_t(head_count[b]);
    for (uintlen_t i = uintlen_t(old_begin[b]); i < uintlen_t(old_end[b]);
         ++i) {
      if (!keep[i])
        continue;
      ret.old_to_new[i] = w;
      ++w;
      w += uintlen_t(after_count[i]);
    }
    ret.new_end.push_back(w);
  }
  ret.total = w;
  return ret;
}

// ---- the multi-branch lowering: N preds -> a binary merge chain ------------
// For a binary-phi IR (this tree's), an N-pred join is lowered BEFORE SSA
// into a right-leaning chain of fresh 2-pred blocks:
//     preds p0 p1 p2 .. p(k-1) of b
//     ->  p0 -> b,  p1,p2 -> C0,  C0,p3 -> C1,  .. C(k-4),p(k-1) -> C(k-3) -> b
// (the pred_norm rewrite, expressed once as data). chain_count = k-2;
// chain block c targets b when c+1 == chain_count, else chain c+1; the
// client assigns the fresh block ids. redirect_pred/redirect_chain pair
// up: pred p_j (j >= 1) retargets its b-edge to chain redirect_chain[j].
// The N-ary phi RECORDS of rename_ssa never need this -- an N-ary-phi IR
// reads them directly -- but every binary IR does, so the shape lives
// here rather than in one client.
template <version_t version_v> struct merge_chain_plan_t {
  uintlen_t block{};       // the >2-pred join being split
  uintlen_t chain_count{}; // fresh 2-pred chain blocks (k - 2)
  std::vector<uintlen_t> redirect_pred{};
  std::vector<uintlen_t> redirect_chain{};
};

template <version_t version_v>
MJZ_CX_FN merge_chain_plan_t<version_v>
calculate_merge_chain_plan(uintlen_t block, const auto &preds_row) noexcept {
  const uintlen_t k = uintlen_t(std::ranges::size(preds_row));
  merge_chain_plan_t<version_v> ret{};
  ret.block = block;
  ret.chain_count = k >= 2 ? k - 2 : 0;
  if (k <= 2)
    return ret;
  for (uintlen_t j = 1; j < k; ++j) {
    ret.redirect_pred.push_back(uintlen_t(preds_row[j]));
    ret.redirect_chain.push_back(j <= 2 ? 0 : j - 2);
  }
  return ret;
}

}; // namespace mjz::ssa_ns
