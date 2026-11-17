

/*MIT License

Copyright (c) 2026 Mjz86

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef MJZ_SRC_GRAPH_phis_FILE_
#define MJZ_SRC_GRAPH_phis_FILE_
#include "csr.hpp"
#include "idf.hpp"
#include "inverse.hpp"

MJZ_EXPORT
//
namespace mjz::graph_ns {

// THE PURE PHI PLACEMENT -- the Cytron relation itself: for variable v
// with def-site set S, phi(v) = IDF(S), the iterated dominance
// frontier of the def blocks. Row v of the result is the set of blocks
// that need a phi for v. NO client assumption rides here (the graph
// cannot even see an instruction, so it cannot filter like an IR):
//   * every def count places per the theory -- zero-def vars have no
//     seeds and place nothing, SINGLE-def vars get their DF(def)
//     phis exactly as the textbook's minimal SSA says,
//   * pred rows are not even an input: a phi's existence never
//     depended on how a client counts predecessors (an entry that
//     sits in its own cycle, a one-pred loop header, all place),
//   * arity is a LOWERING concern -- this layer only says WHERE.
// Which placements a client keeps is policy and lives above (this
// tree's is ssa_ns::calculate_phi_placements, the compiler wrapper).
template <version_t version_v, class Rdf = std::span<const uintlen_t>>
MJZ_CX_FN treversal_result_t<version_v> calculate_phi_placements(
    const basic_forest_t<version_v, Rdf> &dominance_frontiers,
    const auto &def_blocks_of_var) noexcept {
  const uintlen_t node_count =
      uintlen_t(std::ranges::size(dominance_frontiers.range()));
  treversal_result_t<version_v> ret{};
  if constexpr (requires() {
                  { def_blocks_of_var } -> std::ranges::sized_range;
                }) {
    ret.nodes_index.reserve(
        uintlen_t(std::ranges::size(def_blocks_of_var))); // one row per var
  }
  ret.edges.reserve(node_count + std::ranges::size(dominance_frontiers.edges));
  std::vector<uintlen_t> idf{};
  idf.reserve(node_count * 2); // the IDF callee's scratch demand
  for (const auto &def_row : def_blocks_of_var) {
    ret.nodes_index.push_back(uintlen_t(ret.edges.size()));
    idf.clear();
    calculate_iterated_dominance_frontiers_given_dominance_frontiers<version_v>(
        idf, dominance_frontiers, def_row);
    ret.edges.insert(ret.edges.end(), idf.begin(), idf.end());
  }
  return ret;
}

// the transposed index, the direction block-walking clients want: row
// b = the variables whose phi is at b, in ASCENDING variable order.
// The relation is bipartite (variable rows, block entries), so the
// square make_basic_inv_forest is WRONG here -- the universe transpose
// sizes the output by the block space (see make_basic_inv_forest_
// universe); the block universe is the DF forest's own row count.
template <version_t version_v, class Rdf = std::span<const uintlen_t>>
MJZ_CX_FN treversal_result_t<version_v> calculate_inv_phi_placements(
    const basic_forest_t<version_v, Rdf> &dominance_frontiers,
    const auto &def_blocks_of_var) noexcept {
  return make_basic_inv_forest_universe<version_v>(
      calculate_phi_placements<version_v>(dominance_frontiers,
                                          def_blocks_of_var)
          .range(),
      uintlen_t(std::ranges::size(dominance_frontiers.range())));
}
};     // namespace mjz::graph_ns
#endif // MJZ_SRC_GRAPH_phis_FILE_
