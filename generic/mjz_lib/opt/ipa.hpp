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

#ifndef MJZ_SRC_OPT_ipa_FILE_
#define MJZ_SRC_OPT_ipa_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// ipa_ns::calculate_live_params: the interprocedural liveness
// fixpoint (dead arguments)
// ---------------------------------------------------------------------------
// alive[g][p] starts from the HARD roots (operands, observed
// values, surviving base vars -- client policy) and grows through
// the SOFT edges: param p of g forwarded as argument q of a call
// to h is alive once h's q is alive (or h is not a candidate, so
// its params can never die). The least fixpoint -- alive only
// grows -- is unique regardless of visit order; the scan order
// (g ascending, p ascending, softs in record order) is kept
// anyway so the fixpoint after any bounded patience is the
// compiler's historical one.
struct soft_edge_t {
  uintlen_t h{}, q{}; // forwarded to (fn h, argument q)
};
MJZ_CX_FN void
calculate_live_params(const auto &eligible, const auto &softs,
                      std::vector<std::vector<char>> &alive) noexcept {
  const uintlen_t n = uintlen_t(eligible.size());
  for (bool ch = true; ch;) {
    ch = false;
    for (uintlen_t g = 0; g < n; ++g) {
      if (!(g < eligible.size() && eligible[g]))
        continue;
      for (uintlen_t p = 0; p < alive[g].size(); ++p) {
        if (alive[g][p])
          continue;
        for (const soft_edge_t &s : softs[g][p]) {
          if (!(s.h < eligible.size() && eligible[s.h]) ||
              (s.q < alive[s.h].size() && alive[s.h][s.q])) {
            alive[g][p] = 1; // feeds a LIVE param (or a non-candidate)
            ch = true;
            break;
          }
        }
      }
    }
  }
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_ipa_FILE_
