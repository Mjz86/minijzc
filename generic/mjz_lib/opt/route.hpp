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

#ifndef MJZ_SRC_OPT_route_FILE_
#define MJZ_SRC_OPT_route_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// route_ns: the supplier-routing fixpoint (tail duplication's mini
// SSA construction), generic over arity
// ---------------------------------------------------------------------------
// THE CONTRACT. Two SOURCE regions (0 and 1) supply a value; every
// block reads it from SOME supplier: the source that dominates it,
// or a phi BORN at a join of routes. Inputs: pred rows (RAW, any
// arity), the blocks that are DEAD to the routing (the duplicated
// tail and its preds -- client policy), a dominance mask per
// source (a block dominated by exactly one source reads it
// outright), and MAX_JOIN_ARITY -- the phi arity the client's IR
// can spell (the 2-arg IR passes 2; an N-ary-phi IR passes its
// own bound; the founding rule: the kernel never assumes two).
// The fixpoint (rounds capped at nb+1, in-place visibility within
// a round, no-progress break, trailing sweep pending -> dead for
// cycles) resolves, per block: a pending pred blocks it; no
// preds / a dead pred / more preds than the arity bound -> dead
// (no phi possible there); one pred or all preds on ONE route ->
// inherit it; else a phi is BORN at the join (site id 2+k, pred
// order preserved). OUTPUT: route[b] = route_dead_v | 0 | 1 |
// 2+site, and the born sites (block, pred list, pred routes) in
// creation order -- the order the client must mint their phis.
inline constexpr uintlen_t route_pending_v = uintlen_t(-2);
inline constexpr uintlen_t route_dead_v = none_v;
inline constexpr uintlen_t route_born_base_v = 2;
struct route_site_t {
  uintlen_t W{};
  std::vector<uintlen_t> preds{};   // the join's preds, row order
  std::vector<uintlen_t> proutes{}; // their routes at birth
};
struct route_plan_t {
  std::vector<uintlen_t> route{};
  std::vector<route_site_t> sites{};
};
MJZ_CX_FN route_plan_t calculate_supplier_routes(
    const auto &preds, const auto &dead_blocks, const auto &dom0,
    const auto &dom1, uintlen_t max_join_arity) noexcept {
  route_plan_t R{};
  const uintlen_t nb = uintlen_t(preds.size());
  R.route.assign(nb, route_pending_v);
  for (uintlen_t b = 0; b < nb; ++b) {
    if (b < dead_blocks.size() && dead_blocks[b]) {
      R.route[b] = route_dead_v;
      continue;
    }
    const bool d0 = b < dom0.size() && dom0[b];
    const bool d1 = b < dom1.size() && dom1[b];
    if (d0 && !d1)
      R.route[b] = 0;
    else if (d1 && !d0)
      R.route[b] = 1;
  }
  auto step = [&](uintlen_t b) noexcept { // true = progressed
    if (R.route[b] != route_pending_v)
      return false;
    const auto &ps = preds[b];
    bool any_pending = false, any_dead = false;
    for (const auto &pp : ps) {
      const uintlen_t p = uintlen_t(pp);
      if (p >= nb || R.route[p] == route_dead_v)
        any_dead = true; // out-of-range pred: dead to the routing
      else if (R.route[p] == route_pending_v)
        any_pending = true;
    }
    if (any_pending)
      return false; // wait for the pred's round
    if (ps.empty() || any_dead || ps.size() > max_join_arity) {
      R.route[b] = route_dead_v;
      return true;
    }
    bool all_same = true;
    for (uintlen_t k = 1; k < uintlen_t(ps.size()); ++k)
      all_same &= R.route[uintlen_t(ps[0])] == R.route[uintlen_t(ps[k])];
    if (uintlen_t(ps.size()) == 1 || all_same) {
      R.route[b] = R.route[uintlen_t(ps[0])];
      return true;
    }
    route_site_t S{};
    S.W = b;
    for (const auto &pp : ps) {
      S.preds.push_back(uintlen_t(pp));
      S.proutes.push_back(R.route[uintlen_t(pp)]);
    }
    R.route[b] = route_born_base_v + uintlen_t(R.sites.size());
    R.sites.push_back(std::move(S));
    return true;
  };
  for (uintlen_t round = 0; round <= nb; ++round) {
    bool progressed = false;
    for (uintlen_t b = 0; b < nb; ++b)
      progressed |= step(b);
    if (!progressed)
      break;
  }
  for (uintlen_t b = 0; b < nb; ++b)
    if (R.route[b] == route_pending_v)
      R.route[b] = route_dead_v; // cycles among unresolved blocks
  return R;
}
// THE NEED CLOSURE. Given the born sites and the list of (route,
// value) READS the client's use scan found, compute which values
// each SOURCE must carry (need[src][val]) and which values each
// born site must merge (bvals[site], first-appearance order). The
// walk is a DFS over the born-site DAG: reading through a born
// site needs its preds' suppliers, recursively -- pushed in pred
// order so the LAST pred's closure is processed first (the
// original 2-pred emission order, generalized).
MJZ_CX_FN void
calculate_route_needs(uintlen_t n_sources, uintlen_t n_vals, const auto &sites,
                      const auto &reads, std::vector<std::vector<char>> &need,
                      std::vector<std::vector<uintlen_t>> &bvals) noexcept {
  need.assign(n_sources, std::vector<char>(n_vals, 0));
  bvals.assign(sites.size(), {});
  for (const auto &rr : reads) {
    const uintlen_t r = uintlen_t(rr.first);
    const uintlen_t di = uintlen_t(rr.second);
    std::vector<uintlen_t> stack{r};
    while (!stack.empty()) {
      const uintlen_t x = stack.back();
      stack.pop_back();
      if (x < n_sources) {
        need[x][di] = 1;
        continue;
      }
      const uintlen_t k = x - route_born_base_v;
      if (k >= sites.size())
        continue;
      bool have = false;
      for (const auto &vv : bvals[k])
        have |= (vv == di);
      if (!have)
        bvals[k].push_back(di);
      for (const auto &pr : sites[k].proutes)
        stack.push_back(pr);
    }
  }
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_route_FILE_
