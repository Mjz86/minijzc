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


#ifndef MJZ_SRC_OPT_equi_FILE_
#define MJZ_SRC_OPT_equi_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// equi_ns::refine_by_keys: class refinement over serial keys
// (identical-code folding)
// ---------------------------------------------------------------------------
// One refinement round: every candidate joins the class of the
// FIRST candidate with an equal key (rep-of-first-equal), changed
// reported against the previous map. The caller seeds with
// wildcarded keys, then re-serializes with the CURRENT
// representatives until a round changes nothing (the verified
// fixpoint the fold commits at) or the round cap declines. The
// keys are opaque -- what "identical" means is the client's
// serialization policy; the grouping is the algorithm.
MJZ_CX_FN bool refine_by_keys(const std::vector<uint64_t> *keys,
                              const auto &cand,
                              std::vector<uintlen_t> &rep) noexcept {
  const uintlen_t n = uintlen_t(rep.size());
  std::vector<uintlen_t> nrep(n);
  for (uintlen_t f = 0; f < n; ++f)
    nrep[f] = f;
  bool changed = false;
  for (uintlen_t f = 0; f < n; ++f) {
    if (!(f < cand.size() && cand[f]))
      continue;
    for (uintlen_t g = 0; g < f; ++g) { // first candidate, equal key
      if (!(g < cand.size() && cand[g]))
        continue;
      if (keys[g] == keys[f]) {
        nrep[f] = nrep[g];
        break;
      }
    }
    changed |= (nrep[f] != rep[f]);
  }
  rep = std::move(nrep);
  return changed;
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_equi_FILE_
