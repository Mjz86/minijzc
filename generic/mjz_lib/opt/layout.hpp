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


#ifndef MJZ_SRC_OPT_layout_FILE_
#define MJZ_SRC_OPT_layout_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// layout_ns: reverse postorder + the trace chain plan
// ---------------------------------------------------------------------------
// THE MODEL. succs_rows holds, per block, the successor ids in SLOT order
// (a conditional branch's true arm first -- the order is the tiebreak's
// vocabulary, so it IS part of the input); reach is the reachability mask
// (cfg_ns::calculate_reachable_mask's output). Everything unreachable is
// invisible to both kernels.
//
// THE ORDER (calculate_reverse_postorder): an iterative DFS from the
// entry, pushing successors in slot order, emits blocks in postorder;
// reversed, that is the RPO -- loop headers precede their bodies, and
// the slot-order preference keeps it deterministic for any arity.
//
// THE CHAINS (calculate_trace_layout): walk the RPO; from each still
// unplaced block, grow a chain by repeatedly appending the preferred
// OPEN successor: among the row's unplaced reachable candidates, the
// one with the SMALLEST score_of_block (the backend passes the pred
// count -- fewer preds means a cheaper jump), ties going to the
// EARLIEST slot. A binary backend that wants its false arm to fall
// through passes score = pred count and puts the false arm in slot 1:
// both arms open -> fewer preds wins; only slot 1 open -> it wins. The
// plan is order[] (the emission order, a permutation of the reachable
// blocks) and next[] (per block, the block emitted right after it --
// none_v at every chain end and for unreachable/unplaced blocks).
// WHAT a chain means for text (fallthroughs, label placement) is
// client policy.
MJZ_CX_FN std::vector<uintlen_t> calculate_reverse_postorder(
    const auto &succs_rows, const auto &reach, uintlen_t entry) noexcept {
  const uintlen_t nb = uintlen_t(succs_rows.size());
  std::vector<uintlen_t> rpo{};
  if (!nb || entry >= nb || !reach[entry])
    return rpo;
  std::vector<uintlen_t> post{};
  post.reserve(nb);
  std::vector<char> seen(nb, 0);
  std::vector<std::pair<uintlen_t, uintlen_t>> st{};
  seen[entry] = 1;
  st.push_back({entry, 0});
  while (!st.empty()) {
    const uintlen_t b = st.back().first;
    const uintlen_t ci = st.back().second;
    const auto &row = succs_rows[b];
    if (ci < row.size()) {
      const uintlen_t s = uintlen_t(row[ci]);
      ++st.back().second;
      if (s < nb && reach[s] && !seen[s]) {
        seen[s] = 1;
        st.push_back({s, 0});
      }
    } else {
      post.push_back(b);
      st.pop_back();
    }
  }
  rpo.assign(post.rbegin(), post.rend());
  return rpo;
}

struct trace_layout_t {
  std::vector<uintlen_t> order{}; // reachable blocks, emission order
  std::vector<uintlen_t> next{};  // per block: the block emitted after it / none_v
};

MJZ_CX_FN trace_layout_t calculate_trace_layout(
    const auto &succs_rows, const auto &reach, const auto &score_of_block,
    const auto &rpo) noexcept {
  const uintlen_t nb = uintlen_t(succs_rows.size());
  trace_layout_t t{};
  t.next.assign(nb, none_v);
  if (!nb)
    return t;
  std::vector<char> placed(nb, 0);
  for (const auto &bb : rpo) {
    const uintlen_t b = uintlen_t(bb);
    if (b >= nb || !reach[b] || placed[b])
      continue;
    uintlen_t cur = b;
    for (;;) {
      placed[cur] = 1;
      t.order.push_back(cur);
      uintlen_t pick = none_v;
      uintlen_t pick_score = 0;
      const auto &row = succs_rows[cur];
      for (uintlen_t j = 0; j < row.size(); ++j) {
        const uintlen_t s = uintlen_t(row[j]);
        if (s >= nb || !reach[s] || placed[s])
          continue;
        const uintlen_t sc = uintlen_t(score_of_block[s]);
        if (pick == none_v || sc < pick_score) {
          pick = s; // smallest score wins; ties keep the earliest slot
          pick_score = sc;
        }
      }
      if (pick == none_v)
        break; // the chain ends here
      cur = pick;
    }
  }
  for (uintlen_t k = 0; k + 1 < t.order.size(); ++k)
    t.next[t.order[k]] = t.order[k + 1];
  return t;
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_layout_FILE_
