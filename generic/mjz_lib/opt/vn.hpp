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

#ifndef MJZ_SRC_OPT_vn_FILE_
#define MJZ_SRC_OPT_vn_FILE_
#include "base.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// ---------------------------------------------------------------------------
// vn_ns: dominator-scoped value numbering
// ---------------------------------------------------------------------------
// THE CONTRACT. A preorder walk of the dominator tree with a SCOPED
// table: entering a child watermarks the table, leaving truncates --
// an entry is in scope exactly at the blocks its defining site
// dominates. Per site the client states a KEY:
//   tag   vn_skip_v (not a value / policy-excluded) | vn_const_v |
//         vn_phi_v | vn_bin_v
//   dst   the var the site defines (the replacement candidate)
//   op    client op id (bin only)
//   c     the const payload (const only)
//   comm  commutative canonicalization (bin only)
// and the rows: args_of_site[s] = the site's operand VARS in field
// order (phi: the incoming args, raw; bin: the two operands),
// preds_of_site[s] = the phi's per-arg pred ids. The kernel resolves
// operands through its own growing ren map (so a hit keys by the
// canonical representative), keys bin operands by const VALUE when
// the client's const table knows them (and an absent operand keys as
// const 0 -- the compiler's historical encoding, kept bit-for-bit),
// sorts commutative pairs by the mixed var/value tag, and scans the
// table first-match-wins. Phi congruence = same block, same resolved
// incoming map (per-pred), no mutual reference (the taint rule:
// either phi's raw args may not contain the other's dst -- checked
// over ALL of the block's phi sites, hit-dropped ones included,
// which is why taint reads the site rows and not the table).
// OUTPUT: ren (var -> the surviving defining var), drop (per site),
// hits. The client's second pass (rewrite every operand through the
// completed map) and sweep stay client-side: which fields carry
// operands is IR knowledge.
inline constexpr uintlen_t vn_skip_v = 0;
inline constexpr uintlen_t vn_const_v = 1;
inline constexpr uintlen_t vn_phi_v = 2;
inline constexpr uintlen_t vn_bin_v = 3;

template <version_t version_v> struct vn_site_t {
  uintlen_t tag{vn_skip_v};
  uintlen_t dst{none_v}; // the defined var
  uintlen_t op{};        // bin op id (client vocabulary)
  int64_t c{};           // const payload
  uint8_t flags{};       // bit0: commutative
};
inline constexpr uint8_t vn_comm_v = 1;

template <version_t version_v> struct vn_result_t {
  std::vector<uintlen_t> ren{}; // var -> surviving var / none_v
  std::vector<char> keep{};     // site -> 0 when renamed onto a dominator
  uintlen_t hits{};
};

// one keyed bin operand: a var id, or a constant (is_const) keyed by
// value -- an ABSENT operand (none_v) keys as const 0, reproducing
// the compiler's table encoding exactly (see the banner).
struct vn_op_t {
  bool is_const{};
  uint64_t val{};
  MJZ_CX_FN uint64_t sort_key() const noexcept {
    return is_const ? (uint64_t(1) << 63) ^ val : val;
  }
  MJZ_CX_FN friend bool operator==(const vn_op_t &a,
                                   const vn_op_t &b) noexcept {
    return a.is_const == b.is_const && a.val == b.val;
  }
};

template <version_t version_v>
MJZ_CX_FN vn_result_t<version_v>
value_number(uintlen_t n_vars, const auto &dominator_tree,
             const auto &sites_of_block, const auto &site_info,
             const auto &args_of_site, const auto &preds_of_site,
             const auto &const_mask, const auto &const_val) noexcept {
  vn_result_t<version_v> ret{};
  ret.ren.assign(n_vars, none_v);
  const uintlen_t nb = uintlen_t(std::ranges::size(dominator_tree.range()));
  const uintlen_t n_sites = uintlen_t(std::ranges::size(site_info));
  ret.keep.assign(n_sites, 1);
  if (!nb)
    return ret;
  auto dom_rows = dominator_tree.range();
  // the scoped table: fixed fields per entry + three flat payload
  // stores (resolved phi args, phi preds, keyed bin operands), all
  // truncated together on scope exit via the frame's four marks
  struct ent_t {
    uintlen_t tag{}, op{}, dst{}, home{};
    int64_t c{};
    uintlen_t ab{}, ae{}; // resolved phi args [ab, ae)
    uintlen_t pb{}, pe{}; // phi preds [pb, pe)
    uintlen_t kb{}, ke{}; // keyed bin operands [kb, ke)
  };
  std::vector<ent_t> table;
  std::vector<uintlen_t> phi_args{}, phi_preds{};
  std::vector<vn_op_t> keyed{};
  auto resolve = [&](uintlen_t v) noexcept {
    while (v != none_v && v < n_vars && ret.ren[v] != none_v)
      v = ret.ren[v];
    return v;
  };
  // key one bin operand: const table first, absent -> const 0
  auto key_op = [&](uintlen_t raw) noexcept {
    vn_op_t o{};
    const uintlen_t r = resolve(raw);
    if (r != none_v && r < n_vars && const_mask[r]) {
      o.is_const = true;
      o.val = uint64_t(const_val[r]);
    } else if (r == none_v) {
      o.is_const = true; // absent operand: the historical const-0 key
      o.val = 0;
    } else {
      o.is_const = false;
      o.val = r;
    }
    return o;
  };
  struct frame_t {
    uintlen_t block, child_idx;
    uintlen_t mark_ent, mark_ab, mark_pb, mark_kb;
  };
  std::vector<frame_t> frames;
  frames.push_back({0, 0, 0, 0, 0, 0});
  while (!frames.empty()) {
    frame_t &fr = frames.back();
    const uintlen_t b = fr.block;
    if (fr.child_idx == 0) { // entering: scan the block's sites once
      for (const auto &ss : sites_of_block[b]) {
        const uintlen_t s = uintlen_t(ss);
        if (s >= n_sites)
          continue;
        const vn_site_t<version_v> &info = site_info[s];
        if (info.tag == vn_skip_v || info.dst == none_v || info.dst >= n_vars)
          continue;
        if (info.tag == vn_const_v) {
          bool found = false;
          for (const ent_t &t : table)
            if (t.tag == vn_const_v && t.c == info.c) {
              ret.ren[info.dst] = t.dst;
              found = true;
              break;
            }
          if (found) {
            ret.keep[s] = 0;
            ++ret.hits;
          } else {
            table.push_back(
                {.tag = vn_const_v, .dst = info.dst, .home = b, .c = info.c});
          }
          continue;
        }
        if (info.tag == vn_phi_v) {
          // resolved incoming map + the raw pred row
          const auto &arow = args_of_site[s];
          const auto &prow = preds_of_site[s];
          const uintlen_t alen = uintlen_t(std::ranges::size(arow));
          const uintlen_t plen = uintlen_t(std::ranges::size(prow));
          const uintlen_t ab = uintlen_t(phi_args.size());
          for (const auto &a : arow)
            phi_args.push_back(resolve(uintlen_t(a)));
          const uintlen_t pb = uintlen_t(phi_preds.size());
          for (const auto &p : prow)
            phi_preds.push_back(uintlen_t(p));
          bool found = false;
          for (const ent_t &t : table) {
            if (t.tag != vn_phi_v || t.home != b || t.dst == info.dst)
              continue;
            // taint: the current phi's raw args contain t.dst, or the
            // site DEFINING t.dst (an earlier phi of this block --
            // hit-dropped ones included, hence the site rows) has the
            // current phi's dst among its raw args
            bool taint = false;
            for (const auto &a : arow)
              taint |= (uintlen_t(a) == t.dst);
            if (!taint) {
              for (const auto &s2 : sites_of_block[b]) {
                const uintlen_t si = uintlen_t(s2);
                if (si >= n_sites || si == s)
                  continue;
                if (site_info[si].dst != t.dst)
                  continue;   // the unique definer of t.dst (SSA)
                if (si < s) { // strictly earlier in the block
                  for (const auto &a2 : args_of_site[si])
                    taint |= (uintlen_t(a2) == info.dst);
                }
              }
            }
            if (taint)
              continue;
            if (t.ae - t.ab != alen || t.pe - t.pb != plen)
              continue;
            bool eq = true;
            for (uintlen_t k = 0; k < alen && eq; ++k)
              eq &= phi_args[ab + k] == phi_args[t.ab + k];
            for (uintlen_t k = 0; k < plen && eq; ++k)
              eq &= phi_preds[pb + k] == phi_preds[t.pb + k];
            if (eq) {
              ret.ren[info.dst] = t.dst;
              found = true;
              break;
            }
          }
          if (found) {
            ret.keep[s] = 0;
            ++ret.hits;
            phi_args.resize(ab);
            phi_preds.resize(pb); // not entered: drop the scratch
          } else {
            table.push_back({.tag = vn_phi_v,
                             .dst = info.dst,
                             .home = b,
                             .ab = ab,
                             .ae = ab + uintlen_t(std::ranges::size(arow)),
                             .pb = pb,
                             .pe = pb + plen});
          }
          continue;
        }
        if (info.tag == vn_bin_v) {
          const auto &arow = args_of_site[s];
          const uintlen_t kb = uintlen_t(keyed.size());
          for (const auto &a : arow)
            keyed.push_back(key_op(uintlen_t(a)));
          // commutative canonicalization (binary form: swap on the
          // strict tag order -- a stable sort of two, kept exactly)
          if ((info.flags & vn_comm_v) && keyed.size() - kb == 2 &&
              keyed[kb + 1].sort_key() < keyed[kb].sort_key())
            std::swap(keyed[kb], keyed[kb + 1]);
          const uintlen_t klen = uintlen_t(keyed.size() - kb);
          bool found = false;
          for (const ent_t &t : table) {
            if (t.tag != vn_bin_v || t.op != info.op)
              continue;
            if (t.ke - t.kb != klen)
              continue;
            bool eq = true;
            for (uintlen_t k = 0; k < klen && eq; ++k)
              eq &= keyed[kb + k] == keyed[t.kb + k];
            if (eq) {
              ret.ren[info.dst] = t.dst;
              found = true;
              break;
            }
          }
          if (found) {
            ret.keep[s] = 0;
            ++ret.hits;
            keyed.resize(kb);
          } else {
            table.push_back({.tag = vn_bin_v,
                             .op = info.op,
                             .dst = info.dst,
                             .home = b,
                             .kb = kb,
                             .ke = kb + klen});
          }
          continue;
        }
      }
    }
    auto ch = dom_rows[b];
    if (fr.child_idx < uintlen_t(std::ranges::size(ch))) {
      const uintlen_t next = uintlen_t(ch[fr.child_idx++]);
      frames.push_back({next, 0, uintlen_t(table.size()),
                        uintlen_t(phi_args.size()), uintlen_t(phi_preds.size()),
                        uintlen_t(keyed.size())});
      continue;
    }
    // leaving b: its entries go out of scope
    table.resize(fr.mark_ent);
    phi_args.resize(fr.mark_ab);
    phi_preds.resize(fr.mark_pb);
    keyed.resize(fr.mark_kb);
    frames.pop_back();
  }
  return ret;
}

// ---------------------------------------------------------------------------
// vn_ns::mixed_sort_key: the orderable operand tag canon shares
// ---------------------------------------------------------------------------
// The mixed var/value key the canonicalizer orders commutative
// operands by (and the SAME tag discipline vn keys bin operands
// with): absent -> 0, a known const -> (1<<63) ^ value (so
// constants sort after every var id, by value), else the var id.
// One canonical shape per equivalence class is what makes CSE
// hit -- the key is the algorithm, the ops it orders are policy.
MJZ_CX_FN uint64_t mixed_sort_key(uintlen_t n_vars, const auto &const_mask,
                                  const auto &const_val, uintlen_t v) noexcept {
  if (v == none_v)
    return 0;
  if (v < n_vars && const_mask[v])
    return (uint64_t(1) << 63) ^ uint64_t(const_val[v]);
  return uint64_t(v);
}

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_vn_FILE_
