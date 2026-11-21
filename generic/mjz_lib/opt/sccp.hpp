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

#ifndef MJZ_SRC_OPT_sccp_FILE_
#define MJZ_SRC_OPT_sccp_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// sccp_ns: the sparse conditional constant engine
// ---------------------------------------------------------------------------
// THE CONTRACT. The ENGINE owns the two worklists and the
// executability tables; the SEMANTICS stay with the client as one
// callback over scalars (the std::sort-comparator precedent: the
// algorithm is generic, the ordering -- here the transfer -- is
// policy). Per site the client states:
//   kind  sccp_value_v  -- the callback evaluates it
//         sccp_phi_v    -- the KERNEL merges it over its EXECUTABLE
//                          in-edges (phi merging is the algorithm;
//                          the N-ary records are ssa_ns's model:
//                          one (pred, arg) entry per pred-slot edge)
//         sccp_branch_v -- the callback decides which out-edge slots
//                          of the site's block become executable
// The callback (site, block) reads engine.lat/engine.cval and calls
// engine.lower(var, tag, val) for defs and engine.push_edge(block,
// slot) for control; the kernel does the monotone-descent check, the
// use-list revisits, the edge worklist, the newly-executable-block
// revisits, and the step guard. run() returns false when the guard
// trips (no rewrite, the caller's rule).
// THE LATTICE: TOP (not yet known) -> CONST (this exact value on
// every executable path) -> OVERDEF (anything); descent is monotone
// by construction (lower() only fires on change), so the fixpoint is
// unique regardless of visit order -- which is why the compiler's
// rewrite (a pure read of lat/bexec) is byte-identical even though
// the engine's internals are new.
inline constexpr uint8_t sccp_top_v = 0;
inline constexpr uint8_t sccp_cn_v = 1;
inline constexpr uint8_t sccp_od_v = 2;

inline constexpr uintlen_t sccp_value_v = 0;
inline constexpr uintlen_t sccp_phi_v = 1;
inline constexpr uintlen_t sccp_branch_v = 2;

template <version_t version_v> struct sccp_site_t {
  uintlen_t kind{sccp_value_v};
  uintlen_t dst{none_v}; // defined var (value/phi sites)
  uintlen_t phi{none_v}; // phi record index (phi sites)
};

// meet two lattice values (result written to r/rc)
MJZ_CX_FN void sccp_meet(uint8_t a, int64_t av, uint8_t b, int64_t bv,
                         uint8_t &r, int64_t &rc) noexcept {
  if (a == sccp_top_v) {
    r = b;
    rc = bv;
    return;
  }
  if (b == sccp_top_v) {
    r = a;
    rc = av;
    return;
  }
  if (a == sccp_od_v || b == sccp_od_v) {
    r = sccp_od_v;
    return;
  }
  if (av == bv) {
    r = sccp_cn_v;
    rc = av;
    return;
  }
  r = sccp_od_v;
}

template <version_t version_v> struct sccp_engine_t {
  // ---- inputs (client sets these before run) ----
  std::vector<std::vector<uintlen_t>> succs{}; // RAW slot rows, none_v holes
  std::vector<std::vector<uintlen_t>> sites_of_block{};
  std::vector<sccp_site_t<version_v>> sites{};
  std::vector<uintlen_t> phi_block{};
  std::vector<uintlen_t> phi_entry_begin{}; // n_phis + 1
  std::vector<uintlen_t> phi_entry_pred{};
  std::vector<uintlen_t> phi_entry_arg{};
  std::vector<std::vector<uintlen_t>> uses{}; // var -> use sites
  uintlen_t entry_block{};
  std::vector<uint8_t> lat{}; // initial states (client pre-filled)
  std::vector<int64_t> cval{};

  // ---- outputs ----
  std::vector<char> bexec{};
  std::vector<char> eexec{};

  std::vector<uintlen_t> phi_entry_dst{}; // per-phi defining var

  // ---- internals ----
  std::vector<uintlen_t> edge_base{}; // nb + 1 slot prefix sums
  std::vector<uintlen_t> blkof{};     // site -> block
  std::vector<uintlen_t> ssa_wl{}, edge_wl{};

  MJZ_CX_FN uintlen_t edge_id(uintlen_t b, uintlen_t slot) const noexcept {
    return edge_base[b] + slot;
  }
  MJZ_CX_FN uintlen_t n_blocks() const noexcept {
    return uintlen_t(succs.size());
  }
  MJZ_CX_FN uintlen_t n_vars() const noexcept { return uintlen_t(lat.size()); }
  // is any p -> target edge executable? (a repeated pred: ANY of its
  // slots reaching target -- the duplicate-pred rule, N-ary)
  MJZ_CX_FN bool pred_exec_into(uintlen_t p, uintlen_t target) const noexcept {
    if (p == none_v || p >= n_blocks())
      return false;
    const auto &row = succs[p];
    for (uintlen_t j = 0; j < uintlen_t(row.size()); ++j)
      if (row[j] == target && eexec[edge_id(p, j)])
        return true;
    return false;
  }
  // monotone descent, ENFORCED: only a genuine descent writes. TOP is
  // never written (a meet over no executable edges yet knows nothing
  // -- it cannot erase a seeded or learned value: the entry block's
  // phis evaluate before the edge worklist drains), and OVERDEF is
  // never left (the bottom is final). Without the refusal the engine
  // could clobber a pre-seeded OVERDEF back to TOP and land on a
  // non-fixpoint; with it the least fixpoint is unique, the order the
  // two worklists drain in cannot matter. CONST/OVERDEF feed the var
  // worklist.
  MJZ_CX_FN void lower(uintlen_t v, uint8_t tag, int64_t val) noexcept {
    if (v >= n_vars())
      return;
    if (tag != lat[v] && lat[v] != sccp_od_v && tag != sccp_top_v) {
      lat[v] = tag;
      cval[v] = val;
      ssa_wl.push_back(v);
    }
  }
  MJZ_CX_FN void push_edge(uintlen_t b, uintlen_t slot) noexcept {
    if (b < n_blocks() && slot < uintlen_t(succs[b].size()))
      edge_wl.push_back(edge_id(b, slot));
  }
  MJZ_CX_FN void eval_phi(uintlen_t p) noexcept {
    uint8_t r = sccp_top_v;
    int64_t rc = 0;
    const uintlen_t home = phi_block[p];
    for (uintlen_t e = phi_entry_begin[p]; e < phi_entry_begin[p + 1]; ++e) {
      const uintlen_t pred = phi_entry_pred[e];
      const uintlen_t arg = phi_entry_arg[e];
      if (!pred_exec_into(pred, home))
        continue;
      const bool ok = arg != none_v && arg < n_vars();
      const uint8_t al = ok ? lat[arg] : sccp_od_v;
      const int64_t av = ok ? cval[arg] : 0;
      uint8_t mr{};
      int64_t mc{};
      sccp_meet(r, rc, al, av, mr, mc);
      r = mr;
      rc = mc;
    }
    // the phi's defining var travels with the record
    lower(phi_entry_dst[p], r, rc);
  }

  template <class Eval>
  MJZ_CX_FN bool run(const Eval &eval, uint64_t guard) noexcept {
    const uintlen_t nb = n_blocks();
    const uintlen_t n_sites = uintlen_t(sites.size());
    edge_base.assign(nb + 1, 0);
    for (uintlen_t b = 0; b < nb; ++b)
      edge_base[b + 1] = edge_base[b] + uintlen_t(succs[b].size());
    eexec.assign(edge_base[nb], 0);
    bexec.assign(nb, 0);
    blkof.assign(n_sites, none_v);
    for (uintlen_t b = 0; b < nb; ++b)
      for (const auto &ss : sites_of_block[b]) {
        const uintlen_t s = uintlen_t(ss);
        if (s < n_sites)
          blkof[s] = b;
      }
    ssa_wl.clear();
    edge_wl.clear();
    uint64_t steps = 0;
    auto visit_site = [&](uintlen_t s) noexcept {
      const uintlen_t b = blkof[s];
      if (b == none_v || !bexec[b])
        return; // never evaluate code in a non-executable block
      const sccp_site_t<version_v> &info = sites[s];
      if (info.kind == sccp_phi_v) {
        if (info.phi != none_v && info.phi < phi_block.size())
          eval_phi(info.phi);
        return;
      }
      eval(s, b); // value: lower(...); branch: push_edge(...)
    };
    auto visit_block = [&](uintlen_t b) noexcept {
      if (b >= nb || !bexec[b])
        return;
      for (const auto &ss : sites_of_block[b])
        visit_site(uintlen_t(ss));
    };
    if (nb == 0)
      return true;
    const uintlen_t entry = entry_block < nb ? entry_block : 0;
    bexec[entry] = 1;
    visit_block(entry);
    while (!edge_wl.empty() || !ssa_wl.empty()) {
      if (++steps > guard)
        return false; // non-termination safety: no rewrite
      if (!edge_wl.empty()) {
        const uintlen_t e = edge_wl.back();
        edge_wl.pop_back();
        if (eexec[e])
          continue;
        eexec[e] = 1;
        uintlen_t b = nb; // find the block whose slot range holds e
        while (b > 0 && edge_base[b] > e)
          --b;
        const uintlen_t slot = e - edge_base[b];
        const uintlen_t t = succs[b][slot];
        if (t == none_v || t >= nb)
          continue;
        bexec[t] = 1;
        visit_block(t); // re-eval: new in-edge may lower its phis
        continue;
      }
      const uintlen_t v = ssa_wl.back();
      ssa_wl.pop_back();
      if (v >= n_vars())
        continue;
      for (const auto &ss : uses[v])
        visit_site(uintlen_t(ss)); // re-eval each use
    }
    return true;
  }
};

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_sccp_FILE_
