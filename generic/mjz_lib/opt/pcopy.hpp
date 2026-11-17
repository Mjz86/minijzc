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


#ifndef MJZ_SRC_OPT_pcopy_FILE_
#define MJZ_SRC_OPT_pcopy_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// pcopy_ns: parallel-copy sequentialization + the edge-split plan
// ---------------------------------------------------------------------------
// A parallel copy set is a SIMULTANEOUS assignment; lowering it to
// sequential copies must preserve that. The greedy filter emits only
// copies whose dst is not a pending SOURCE; whatever survives is
// exactly a permutation cycle, broken by saving ONE source into a
// fresh temp (the callback allocates it -- the client's vreg
// minting, called at exactly the cycle-break points). src == none_v
// is the UNDEF arg: materialized as const zero. Self-copies are
// no-ops and would otherwise look like unbreakable 1-cycles.
struct pcopy_op_t {
  uintlen_t dst{}, src{}; // src none_v = const zero
  bool zero{};            // the undef materialization
};
struct pcopy_stats_t {
  uintlen_t self_copies{}, zeros{}, copies{}, cycles_broken{};
};
template <class AllocTemp>
MJZ_CX_FN std::vector<pcopy_op_t>
sequentialize_pcopy(std::vector<std::pair<uintlen_t, uintlen_t>> pcs,
                    const AllocTemp &alloc_temp, pcopy_stats_t &st) noexcept {
  std::vector<pcopy_op_t> out{};
  for (uintlen_t i = 0; i < pcs.size();) {
    if (pcs[i].first == pcs[i].second && pcs[i].second != none_v) {
      pcs.erase(pcs.begin() + (long)i);
      ++st.self_copies;
    } else {
      ++i;
    }
  }
  while (!pcs.empty()) {
    bool progress = false;
    for (uintlen_t i = 0; i < pcs.size(); ++i) {
      const uintlen_t d = pcs[i].first;
      bool d_needed = false; // is d still a source of a pending copy?
      for (const auto &p : pcs)
        if (p.second == d) {
          d_needed = true;
          break;
        }
      if (d_needed)
        continue; // emitting now would clobber a needed value
      if (pcs[i].second == none_v) { // undef arg: materialize 0
        out.push_back({.dst = d, .src = none_v, .zero = true});
        ++st.zeros;
      } else {
        out.push_back({.dst = d, .src = pcs[i].second});
        ++st.copies;
      }
      pcs.erase(pcs.begin() + (long)i);
      progress = true;
      break;
    }
    if (!progress && !pcs.empty()) {
      // swap cycle: save the source into a fresh temp, redirect
      const uintlen_t d = pcs[0].first, s = pcs[0].second;
      const uintlen_t t = alloc_temp(d);
      out.push_back({.dst = t, .src = s});
      pcs[0].second = t;
      ++st.copies;
      ++st.cycles_broken;
    }
  }
  return out;
}

// the critical-edge split plan: an edge (pred p, slot j -> target t)
// needs a block of its own iff p fans out to >= 2 DISTINCT targets
// (with fewer, copies can sit before p's terminator; with all-equal
// targets every path runs them) and t holds phis (nothing to place
// otherwise). Splits are enumerated (p ascending, slot ascending),
// fresh ids assigned sequentially -- the order the client's rebuild
// must apply to mint identical block ids.
struct edge_split_t {
  uintlen_t pred{}, slot{}, target{}, fresh{};
};
MJZ_CX_FN std::vector<edge_split_t>
calculate_edge_splits(const auto &succs_rows, const auto &has_phi) noexcept {
  std::vector<edge_split_t> ret{};
  const uintlen_t nb = uintlen_t(succs_rows.size());
  const uintlen_t n0 = nb;
  for (uintlen_t p = 0; p < n0; ++p) {
    const auto &row = succs_rows[p];
    uintlen_t valid = 0, distinct = 0;
    uintlen_t first = none_v;
    for (const auto &t : row)
      if (t != none_v) {
        ++valid;
        if (first == none_v) {
          first = t;
          distinct = 1;
        } else if (t != first)
          distinct = 2;
      }
    if (valid < 2 || distinct < 2)
      continue; // single-succ / all arms on one target: no split
    for (uintlen_t j = 0; j < uintlen_t(row.size()); ++j) {
      const uintlen_t t = row[j];
      if (t == none_v || t >= nb)
        continue;
      if (!has_phi[t])
        continue;
      ret.push_back({.pred = p,
                     .slot = j,
                     .target = t,
                     .fresh = n0 + uintlen_t(ret.size())});
    }
  }
  return ret;
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_pcopy_FILE_
