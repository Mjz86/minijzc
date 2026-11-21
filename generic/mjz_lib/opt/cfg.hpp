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

#ifndef MJZ_SRC_OPT_cfg_FILE_
#define MJZ_SRC_OPT_cfg_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// cfg_ns: the reachability flood + the splice pairing plan
// ---------------------------------------------------------------------------
// THE FLOOD: from the entry, through the VALID succ slots (the
// client's rows hold valid targets only). The mask is a pure
// function of the rows -- order-independent.
MJZ_CX_FN std::vector<char> calculate_reachable_mask(const auto &succs_rows,
                                                     uintlen_t entry) noexcept {
  const uintlen_t nb = uintlen_t(succs_rows.size());
  std::vector<char> reach(nb, 0);
  if (!nb || entry >= nb)
    return reach;
  std::vector<uintlen_t> wl{entry};
  reach[entry] = 1;
  while (!wl.empty()) {
    const uintlen_t b = wl.back();
    wl.pop_back();
    for (const auto &s : succs_rows[b]) {
      const uintlen_t t = uintlen_t(s);
      if (t < nb && !reach[t]) {
        reach[t] = 1;
        wl.push_back(t);
      }
    }
  }
  return reach;
}

// THE SPLICE PLAN: matchings only (no chains): b absorbs its single
// successor s when s is not b, not the entry, not already paired,
// entered ONLY from b, and phi-free. The sweep rekeys the preds of
// s's successors s -> b AS IT GOES (a later pair's "entered only
// from b'" check reads the rekeyed rows), so the kernel keeps its
// own mutated copy and the client never sees it -- the client's
// final preds are recomputed from succ anyway. The plan is the two
// arrays; the rebuild (concatenating code ranges, dropping the
// absorbed blocks' terminators) is IR surgery and stays with the
// caller, as do the phi t/f rekeys (derivable: for every absorber
// pair, rekey the phis of the absorbed block's successors).
struct splice_plan_t {
  std::vector<uintlen_t> absorbed_of{}; // block -> absorber / none_v
  std::vector<uintlen_t> merges_of{};   // absorber -> absorbed / none_v
};
MJZ_CX_FN splice_plan_t calculate_splice_plan(
    const auto &succs_rows, std::vector<std::vector<uintlen_t>> preds,
    uintlen_t entry, const auto &has_phi) noexcept {
  splice_plan_t ret{};
  const uintlen_t nb = uintlen_t(succs_rows.size());
  ret.absorbed_of.assign(nb, none_v);
  ret.merges_of.assign(nb, none_v);
  for (uintlen_t b = 0; b < nb; ++b) {
    if (ret.absorbed_of[b] != none_v || ret.merges_of[b] != none_v)
      continue; // already in a pair this sweep
    const auto &row = succs_rows[b];
    if (row.size() != 1)
      continue; // dead (0) or multi-succ
    const uintlen_t s = uintlen_t(row[0]);
    if (s == b || s == entry)
      continue; // self-loop / entry: keep
    if (s >= nb)
      continue;
    if (ret.absorbed_of[s] != none_v || ret.merges_of[s] != none_v)
      continue;
    if (preds[s].size() != 1 || preds[s][0] != b)
      continue;
    if (has_phi[s])
      continue;
    ret.absorbed_of[s] = b;
    ret.merges_of[b] = s;
    for (const auto &tt : succs_rows[s]) { // rekey preds of s's succs
      const uintlen_t t = uintlen_t(tt);
      if (t >= nb)
        continue;
      for (uintlen_t &p : preds[t])
        if (p == s)
          p = b;
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// cfg_ns::region_uses_escape: does anything OUTSIDE the region
// read a value defined INSIDE it?
// ---------------------------------------------------------------------------
// def_site_of_var[v] = the site defining v (none_v when none);
// args_of_site[s] = site s's operand vars, ANY order. A use at a
// site outside [lo, hi) of a var whose def site is inside [lo,
// hi) escapes: the value would not exist on a path that skips
// the region. jthread's elbow-leak scan, stated for any IR --
// the FIRST scan of the walk is order-free (a boolean OR).
MJZ_CX_FN bool region_uses_escape(const auto &def_site_of_var,
                                  const auto &args_of_site, uintlen_t lo,
                                  uintlen_t hi) noexcept {
  const uintlen_t n_sites = uintlen_t(std::ranges::size(args_of_site));
  const uintlen_t n_vars = uintlen_t(std::ranges::size(def_site_of_var));
  for (uintlen_t s = 0; s < n_sites; ++s) {
    if (lo <= s && s < hi)
      continue; // uses inside the region are fine
    for (const auto &vv : args_of_site[s]) {
      const uintlen_t v = uintlen_t(vv);
      if (v >= n_vars)
        continue;
      const uintlen_t d = uintlen_t(def_site_of_var[v]);
      if (d != none_v && lo <= d && d < hi)
        return true; // a region-local def read outside
    }
  }
  return false;
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_cfg_FILE_
