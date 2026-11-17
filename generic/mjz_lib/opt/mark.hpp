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


#ifndef MJZ_SRC_OPT_mark_FILE_
#define MJZ_SRC_OPT_mark_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// mark_ns: mark-sweep liveness over the def/use bipartite graph
// ---------------------------------------------------------------------------
// THE CONTRACT. def_of_var[v] = the site defining v (SSA: one def;
// none_v when none); args_of_site[s] = the vars site s reads (ANY
// order, duplicates and out-of-range entries tolerated); roots = the
// sites the client's policy calls observable. A site is live iff it
// is a root or transitively feeds one: popping a live site marks the
// def sites of its operands. The fixpoint is order-independent (a
// site is pushed at most once), so the mask is a pure function of
// the three tables -- which is why dce's byte-identity survives the
// hand-rolled walk becoming this call.
template <version_t version_v>
MJZ_CX_FN std::vector<char> calculate_live_sites(const auto &def_of_var,
                                                 const auto &args_of_site,
                                                 const auto &roots) noexcept {
  const uintlen_t n_vars = uintlen_t(std::ranges::size(def_of_var));
  const uintlen_t n_sites = uintlen_t(std::ranges::size(args_of_site));
  std::vector<char> live(n_sites, 0);
  std::vector<uintlen_t> wl{};
  wl.reserve(n_sites);
  for (const auto &r : roots) {
    const uintlen_t s = uintlen_t(r);
    if (s < n_sites && !live[s]) {
      live[s] = 1;
      wl.push_back(s);
    }
  }
  while (!wl.empty()) {
    const uintlen_t i = wl.back();
    wl.pop_back();
    for (const auto &vv : args_of_site[i]) {
      const uintlen_t v = uintlen_t(vv);
      if (v >= n_vars)
        continue; // none_v and junk both bail out
      const uintlen_t d = uintlen_t(def_of_var[v]);
      if (d != none_v && d < n_sites && !live[d]) {
        live[d] = 1;
        wl.push_back(d);
      }
    }
  }
  return live;
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_mark_FILE_
