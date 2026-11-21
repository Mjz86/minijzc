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

#ifndef MJZ_SRC_OPT_liveness_FILE_
#define MJZ_SRC_OPT_liveness_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// liveness_ns: backward block liveness + the interference rows
// ---------------------------------------------------------------------------
// THE MODEL. A program is blocks of sites: block_sites[b] = the half-open
// site range {begin, end}; args_of_site[i] = the vars site i reads (ANY
// order, duplicates and out-of-range entries tolerated); def_of_site[i] =
// the var site i defines (none_v when none); skip_site[i] = 1 makes the
// site invisible to every reader (the backend's dead/inlined flags). The
// rows of succs_rows hold valid successor ids only (the adapters'
// convention; duplicates fine -- the union is idempotent).
//
// THE FIXPOINT (calculate_block_liveness): live_out(b) = the union of
// live_in over b's successors; inside a block the sites are scanned
// BACKWARD with def-kills-use (a def removes its var from live, a use
// adds it). The sweep runs over the blocks in DESCENDING order (loop
// tails propagate to headers on the first pass) for at most max_rounds
// rounds, stopping at the round that changed nothing. The fixpoint of
// these equations is unique -- the round cap is a runaway guard, not a
// precision knob; a monotone descent never needs more than a few rounds
// past the block-chain depth.
//
// THE ROWS (calculate_interference_rows): the register allocator's
// graph, stated for any client. Two vars interfere when one is defined
// at a site where the other is live, OR when both are defined at the
// entry point: entry_defs are the "defined before the first site" vars
// (parameters); each conflicts with everything live into the entry
// block AND, unconditionally, with every other entry def -- an entry
// move that homes two of them together would clobber the sibling
// (regalloc's founding bug, fuzz seed 804). Rows come back symmetric,
// duplicate-free and self-loop-free -- exactly what a coloring kernel
// wants. WHICH home a color means, and what spills, is client policy.
struct block_liveness_t {
  std::vector<std::vector<char>> live_in{}, live_out{};
};

template <version_t version_v>
MJZ_CX_FN block_liveness_t calculate_block_liveness(
    const auto &succs_rows, const auto &block_sites, const auto &args_of_site,
    const auto &def_of_site, const auto &skip_site, uintlen_t n_vars,
    uintlen_t max_rounds) noexcept {
  const uintlen_t nb = uintlen_t(succs_rows.size());
  block_liveness_t liv{};
  liv.live_in.assign(nb, std::vector<char>(n_vars, 0));
  liv.live_out.assign(nb, std::vector<char>(n_vars, 0));
  if (!nb || !n_vars)
    return liv;
  for (uintlen_t round = 0; round < max_rounds; ++round) {
    bool changed = false;
    for (uintlen_t bi = nb; bi-- > 0;) {
      const uintlen_t b = bi;
      std::vector<char> &lo = liv.live_out[b];
      std::fill(lo.begin(), lo.end(), 0);
      for (const auto &ss : succs_rows[b]) {
        const uintlen_t s = uintlen_t(ss);
        if (s >= nb)
          continue; // none_v and junk both bail out
        for (uintlen_t v = 0; v < n_vars; ++v)
          lo[v] = char(lo[v] | liv.live_in[s][v]);
      }
      std::vector<char> live = lo;
      const uintlen_t begin = uintlen_t(block_sites[b].first);
      const uintlen_t end = uintlen_t(block_sites[b].second);
      for (uintlen_t i = end; i-- > begin;) {
        if (skip_site[i])
          continue; // invisible to the reader
        const uintlen_t d = uintlen_t(def_of_site[i]);
        if (d != none_v && d < n_vars)
          live[d] = 0;
        for (const auto &vv : args_of_site[i]) {
          const uintlen_t v = uintlen_t(vv);
          if (v < n_vars)
            live[v] = 1;
        }
      }
      for (uintlen_t v = 0; v < n_vars; ++v)
        if (live[v] != liv.live_in[b][v]) {
          liv.live_in[b][v] = live[v];
          changed = true;
        }
    }
    if (!changed)
      break;
  }
  return liv;
}

template <version_t version_v>
MJZ_CX_FN std::vector<std::vector<uintlen_t>>
calculate_interference_rows(const auto &block_sites, const auto &args_of_site,
                            const auto &def_of_site, const auto &skip_site,
                            const block_liveness_t &liv, uintlen_t n_vars,
                            uintlen_t entry, const auto &entry_defs) noexcept {
  const uintlen_t nb = uintlen_t(liv.live_out.size());
  std::vector<std::vector<uintlen_t>> adj(n_vars);
  auto edge = [&](uintlen_t a, uintlen_t b) noexcept {
    if (a == b || a >= n_vars || b >= n_vars)
      return;
    for (uintlen_t x : adj[a])
      if (x == b)
        return; // rows stay duplicate-free
    adj[a].push_back(b);
    adj[b].push_back(a);
  };
  if (nb && entry < nb) { // the entry defs: defined before the first site
    std::vector<char> still(liv.live_in[entry]);
    for (const auto &dd : entry_defs) {
      const uintlen_t d = uintlen_t(dd);
      if (d >= n_vars)
        continue;
      for (uintlen_t u = 0; u < n_vars; ++u)
        if (still[u] && u != d)
          edge(d, u);
      still[d] = 0; // an earlier entry def is no longer "live through"
    }
    // ...and PAIRWISE, always: an entry move homes every argument
    // unconditionally, so two entry defs never share a home even when
    // neither is live-in (the uses could sit in unreachable code)
    for (uintlen_t ai = 0; ai < entry_defs.size(); ++ai) {
      const uintlen_t a = uintlen_t(entry_defs[ai]);
      for (uintlen_t bi = ai + 1; bi < entry_defs.size(); ++bi)
        edge(a, uintlen_t(entry_defs[bi]));
    }
  }
  for (uintlen_t b = 0; b < nb; ++b) {
    std::vector<char> live = liv.live_out[b];
    const uintlen_t begin = uintlen_t(block_sites[b].first);
    const uintlen_t end = uintlen_t(block_sites[b].second);
    for (uintlen_t i = end; i-- > begin;) {
      if (skip_site[i])
        continue;
      const uintlen_t d = uintlen_t(def_of_site[i]);
      if (d != none_v && d < n_vars) {
        for (uintlen_t u = 0; u < n_vars; ++u)
          if (live[u])
            edge(d, u);
        live[d] = 0;
      }
      for (const auto &vv : args_of_site[i]) {
        const uintlen_t v = uintlen_t(vv);
        if (v < n_vars)
          live[v] = 1;
      }
    }
  }
  return adj;
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_liveness_FILE_
