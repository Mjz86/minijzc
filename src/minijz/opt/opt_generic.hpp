#pragma once
// === opt_generic.hpp ========================================
// The optimization kernels, GENERIC over index space -- the ssa_ns
// treatment (ir/ssa_generic.hpp) extended to every algorithm the
// opt/ tree runs. Everything here speaks uintlen_t: blocks, vars,
// sites as dense client-chosen ids; CSR rows and event streams as
// the only inputs; pure tables as the only outputs. No Instr, no
// Fn, no K/B enums ever appear, so the layer compiles and
// unit-tests standalone (tests/unit_tests.cpp section 17) and could
// lift into mjz_lib's graph_ns unchanged.
//
// THE RULE (the branch's founding directive): an ALGORITHM must not
// depend on the way the compiler works. Binary phis, the >=2-pred
// rule, the <2-defs skip, root policies, trap policies, hoist
// legality -- those are the COMPILER'S policies and live in the
// passes (the wrappers); what lives here is the relation itself,
// stated for ANY arity and ANY policy:
//   * mark_ns     -- mark-sweep liveness over the def/use bipartite
//                    graph (dce's core; the same fixpoint underpins
//                    every "is it used" question in the tree)
//   * vn_ns       -- dominator-scoped value numbering with scoped
//                    table truncation, operand resolution, const
//                    keying by VALUE and commutative
//                    canonicalization (cse and gvn are the same
//                    walk with different entry kinds)
//   * sccp_ns     -- the sparse conditional constant engine: two
//                    worklists (edges + vars), executable-edge
//                    tracking, phi merging over EXECUTABLE in-edges
//                    only, N-ary phi records, monotone descent, the
//                    step guard. The lattice itself is
//                    TOP/CONST/OVERDEF over int64 -- the transfer
//                    semantics stay with the client (a callback on
//                    scalars, the std::sort-comparator precedent)
//   * pcopy_ns    -- parallel-copy sequentialization (the lost-copy
//                    / swap-cycle problem of SSA destruction) and
//                    the critical-edge split plan
//   * cfg_ns      -- the reachability flood (unreach) and the
//                    splice pairing plan (single-pred/single-succ
//                    absorption as pure CFG math)
//   * inv_ns      -- loop-invariant selection: the unique-outside-
//                    pred rule, the invariance predicate over def
//                    blocks/dominance/consts, and the scan
//                    discipline (header first, then member row)
//   * ren_ns      -- the copy-rename builder with the crossed-pair
//                    self-loop guard (fuzz seed 7406's lesson) and
//                    the capped resolve
//   * the trivial-phi pick: all non-self args equal -> that arg
//                    (the N-ary form of dce's phi collapse)
//
// THE PROGRAM MODEL (identical to ssa_ns, one paragraph each):
//   * blocks  0..n_blocks-1; CFG as slot rows (succs_of_block, the
//     RAW arity -- duplicates and none_v holes kept, exactly like
//     Fn::succ for the binary IR) and positional pred rows.
//   * vars    0..n_vars-1, pseudo-variables (vregs, slots, whatever
//     the client is numbering).
//   * sites   0..n_sites-1, client ids for instruction slots; rows
//     sites_of_block give each block's sites in program order. THE
//     WHOLE IR KNOWLEDGE the kernels need is: which vars a site
//     reads (args_of_site), which var it defines (site info), and
//     for phis the (pred, arg) entries (pred-keyed, N-ary).
//
// PIPELINE POSITION. Not positioned: a library leaf, like
// ssa_generic.hpp. Its in-tree clients are the opt/ passes
// (dce/cse/gvn/copyprop/sccp/unreach/splice/licm), ir/destruct.hpp
// and the shared sweep helper; every client keeps its theory banner
// -- what changed in each is that the hand-rolled core became a
// table replay (see each pass's banner note).
//
#include "../graph_lib.hpp"

// ============================================================================
// opt_ns: the optimization kernels over index space (graph_ns
// conventions: uintlen_t ids, version_t-templated, CSR rows,
// MJZ_CX_FN noexcept, uintlen_t(-1) as NONE)
// ============================================================================
namespace mjz::opt_ns {

inline constexpr uintlen_t none_v = uintlen_t(-1);

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
  // monotone descent: only on change; CONST/OVERDEF feed the var wl
  MJZ_CX_FN void lower(uintlen_t v, uint8_t tag, int64_t val) noexcept {
    if (v >= n_vars())
      return;
    if (tag != lat[v]) {
      lat[v] = tag;
      cval[v] = val;
      if (tag != sccp_top_v)
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
// inv_ns: loop-invariant selection
// ---------------------------------------------------------------------------
// THE CONTRACT. The client states the loop (member row + header +
// in-loop mask), the preheader (from find_unique_outside_pred), the
// def tables (def_block_of_var, the client's const table), the
// dominance intervals, the block's sites in program order with their
// operand rows, and an ELIGIBILITY mask (the policy: purity, trap
// safety, has-a-def -- all compiler rules). The kernel answers the
// FIRST hoistable site under the scan discipline (header FIRST --
// an invariant header test is the classic unswitch enabler -- then
// the member row in order), where "hoistable" = eligible and every
// operand is invariant: absent, or const (position-independent), or
// defined outside the loop by a def that DOMINATES the preheader.
// One site per call: the client hoists, recomputes its tables, and
// calls again (the pass's rescan discipline -- a hoist can make more
// operands invariant).
MJZ_CX_FN std::pair<uintlen_t, uintlen_t>
find_unique_outside_pred(const auto &preds_of_header,
                         const auto &in_loop) noexcept {
  uintlen_t pre = none_v, outside = 0;
  for (const auto &pp : preds_of_header) {
    const uintlen_t p = uintlen_t(pp);
    if (!in_loop[p]) {
      pre = p;
      ++outside;
    }
  }
  return {pre, outside};
}

template <version_t version_v>
MJZ_CX_FN std::pair<uintlen_t, uintlen_t> calculate_first_invariant_site(
    const auto &loop_row, uintlen_t header, uintlen_t preheader,
    const auto &in_loop, const auto &def_block_of_var, const auto &const_mask,
    const auto &dom_begin, const auto &dom_end, const auto &sites_of_block,
    const auto &args_of_site, const auto &eligible) noexcept {
  const uintlen_t n_vars = uintlen_t(def_block_of_var.size());
  const uintlen_t n_blocks = uintlen_t(in_loop.size());
  auto dominates = [&](uintlen_t d, uintlen_t s) noexcept {
    if (d >= n_blocks || s >= n_blocks)
      return false;
    if (dom_begin[d] == 0 || dom_begin[s] == 0)
      return false;
    return dom_begin[d] <= dom_begin[s] && dom_end[d] >= dom_end[s];
  };
  auto invariant = [&](uintlen_t v) noexcept {
    if (v == none_v)
      return true;
    if (v >= n_vars)
      return false;
    const uintlen_t d = uintlen_t(def_block_of_var[v]);
    if (d == none_v)
      return false;
    if (const_mask[v])
      return true; // a Const's VALUE is position-independent
    if (in_loop[d])
      return false;                 // defined inside: not invariant
    return dominates(d, preheader); // reachable into the preheader
  };
  const uintlen_t row_len = uintlen_t(std::ranges::size(loop_row));
  for (uintlen_t bi = 0; bi <= row_len; ++bi) {
    const uintlen_t B = (bi == 0) ? header : uintlen_t(loop_row[bi - 1]);
    if (B >= n_blocks)
      continue;
    for (const auto &ss : sites_of_block[B]) {
      const uintlen_t s = uintlen_t(ss);
      if (!eligible[s])
        continue;
      bool ok = true;
      for (const auto &vv : args_of_site[s])
        ok &= invariant(uintlen_t(vv));
      if (ok)
        return {B, s};
    }
  }
  return {none_v, none_v};
}

// ---------------------------------------------------------------------------
// ren_ns: the copy-rename builder
// ---------------------------------------------------------------------------
// THE CONTRACT. pairs = (dst, src) of every eligible copy in code
// order (dst/src valid, dst != src). Sources resolve transitively
// through the map being built, so chains die in one hop; the
// CROSSED-PAIR guard refuses the closing copy of a cyclic rename
// (resolve(src) == dst -- a self-loop entry that would never
// resolve; the phi-degenerate hazard, fuzz seed 7406). The resolve
// walk is capped at n_vars steps: belt and braces so a future
// multi-def IR cannot resurrect an infinite walk.
MJZ_CX_FN std::vector<uintlen_t> build_copy_rename(uintlen_t n_vars,
                                                   const auto &pairs) noexcept {
  std::vector<uintlen_t> ren(n_vars, none_v);
  auto resolve = [&](uintlen_t v) noexcept {
    uintlen_t steps = 0;
    while (v != none_v && v < n_vars && ren[v] != none_v) {
      v = ren[v];
      if (++steps > n_vars)
        break;
    }
    return v;
  };
  for (const auto &pr : pairs) {
    const uintlen_t d = uintlen_t(pr.first), s = uintlen_t(pr.second);
    if (d >= n_vars || s >= n_vars)
      continue;
    const uintlen_t r = resolve(s);
    if (r != d) // r == d: crossed pair -- do not record
      ren[d] = r;
  }
  return ren;
}
MJZ_CX_FN uintlen_t resolve_capped(const std::vector<uintlen_t> &ren,
                                   uintlen_t v) noexcept {
  const uintlen_t n_vars = uintlen_t(ren.size());
  uintlen_t steps = 0;
  while (v != none_v && v < n_vars && ren[v] != none_v) {
    v = ren[v];
    if (++steps > n_vars)
      break;
  }
  return v;
}

// ---------------------------------------------------------------------------
// the trivial-phi pick (dce's collapse rule, N-ary)
// ---------------------------------------------------------------------------
// All non-self incoming args equal (absent args ignored) -> that
// arg; more than one distinct value or nothing but self/absent ->
// none_v (a genuine phi). phi(a,a), phi(a,self), phi(a) pick a --
// the collapse the copyprop pass then feeds on.
MJZ_CX_FN uintlen_t calculate_trivial_phi_pick(uintlen_t dst,
                                               const auto &args_row) noexcept {
  uintlen_t pick = none_v;
  for (const auto &aa : args_row) {
    const uintlen_t a = uintlen_t(aa);
    if (a == none_v || a == dst)
      continue;
    if (pick == none_v)
      pick = a;
    else if (pick != a)
      return none_v; // genuine phi
  }
  return pick;
}

// ---------------------------------------------------------------------------
// shape_ns: natural-loop shape as pure CFG math
// ---------------------------------------------------------------------------
// THE CONTRACT. classify_header_preds: one sweep over a header's
// pred row against the loop mask -- the LAST inside pred is the
// latch candidate, the LAST outside one the preheader candidate,
// with the counts (the "exactly one of each" test is the caller's
// loop-shape policy). calculate_loop_shape: the canonical test-
// first shape -- unique in-pred (latch), unique out-pred (pre),
// and a TWO-ARM branch with exactly one arm staying (body) and
// one leaving (X); the terminator KIND check stays with the
// client (which instr ends a block is IR knowledge; the arms and
// their in/out classification are CFG math). collect_exit_edges:
// every in-loop -> outside edge except the client's primary edge,
// with the degeneracy rule (two VALID slots on the same outside
// target = a both-arms-out branch -- bypass threading cannot
// represent per-arm values) and the forbidden-target paranoia as
// parameters. Every restructuring pass (unroll / lcf / unswitch /
// rotate / lsimp) asks these three questions; none of them may
// assume the binary terminator -- the row shapes are the client's.
struct header_preds_t {
  uintlen_t pre{none_v}, latch{none_v};
  uintlen_t n_in{}, n_out{};
};
MJZ_CX_FN header_preds_t calculate_header_preds(const auto &pred_row,
                                                const auto &in_loop) noexcept {
  header_preds_t r{};
  for (const auto &pp : pred_row) {
    const uintlen_t p = uintlen_t(pp);
    if (p < in_loop.size() && in_loop[p]) {
      r.latch = p;
      ++r.n_in;
    } else {
      r.pre = p;
      ++r.n_out;
    }
  }
  return r;
}
struct loop_shape_t {
  uintlen_t pre{none_v}, latch{none_v}, body{none_v}, X{none_v};
  bool ok{};
};
// arm0/arm1: the header branch's two VALID targets (slot order).
MJZ_CX_FN loop_shape_t calculate_loop_shape(const auto &pred_row,
                                            const auto &in_loop,
                                            uintlen_t arm0,
                                            uintlen_t arm1) noexcept {
  loop_shape_t sh{};
  const header_preds_t hp = calculate_header_preds(pred_row, in_loop);
  if (hp.n_in != 1 || hp.n_out != 1)
    return sh;
  sh.pre = hp.pre;
  sh.latch = hp.latch;
  const uintlen_t nb = uintlen_t(in_loop.size());
  const bool v0 = arm0 < nb, v1 = arm1 < nb;
  const bool in0 = v0 && in_loop[arm0], in1 = v1 && in_loop[arm1];
  if (in0 && !in1 && v1) {
    sh.body = arm0;
    sh.X = arm1;
  } else if (in1 && !in0 && v0) {
    sh.body = arm1;
    sh.X = arm0;
  } else
    return sh;
  sh.ok = true;
  return sh;
}
struct exit_edge_t {
  uintlen_t from{}, to{};
};
// Every in-loop -> outside edge except (skip_from -> skip_to).
// Returns false on the degeneracy (two valid slots, same outside
// target) or a forbidden target (forbid0/forbid1, none_v = none):
// the caller discards the partial list either way.
MJZ_CX_FN bool collect_exit_edges(const auto &succs_rows, const auto &in_loop,
                                  uintlen_t skip_from, uintlen_t skip_to,
                                  uintlen_t forbid0, uintlen_t forbid1,
                                  std::vector<exit_edge_t> &out) noexcept {
  const uintlen_t nb = uintlen_t(succs_rows.size());
  for (uintlen_t b = 0; b < nb; ++b) {
    if (!(b < in_loop.size() && in_loop[b]))
      continue;
    const auto &row = succs_rows[b];
    uintlen_t valid = 0, distinct = 0;
    uintlen_t first = none_v;
    for (const auto &ss : row) {
      const uintlen_t s = uintlen_t(ss);
      if (s == none_v || s >= nb)
        continue;
      ++valid;
      if (first == none_v) {
        first = s;
        distinct = 1;
      } else if (s != first)
        distinct = 2;
    }
    if (valid >= 2 && distinct == 1 && first < in_loop.size() &&
        !in_loop[first])
      return false; // every arm on ONE outside target: degenerate
    for (const auto &ss : row) {
      const uintlen_t s = uintlen_t(ss);
      if (s == none_v || s >= nb || in_loop[s])
        continue;
      if (b == skip_from && s == skip_to)
        continue; // the edge the caller replaces outright
      out.push_back({.from = b, .to = s});
      if (s == forbid0 || s == forbid1)
        return false; // would tangle the versioned preds; paranoia
    }
  }
  return true;
}

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
  std::vector<uintlen_t> preds{};  // the join's preds, row order
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
MJZ_CX_FN void calculate_route_needs(
    uintlen_t n_sources, uintlen_t n_vals, const auto &sites,
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

// ---------------------------------------------------------------------------
// alias_ns: allocation-identity resolution (the copy/phi fixpoint)
// ---------------------------------------------------------------------------
// THE CONTRACT. Per var, the client states its def in CODE ORDER:
// kind alias_copy_v (single[dst] := single[src]), alias_phi_v
// (single iff EVERY arg is a valid var whose single is the SAME
// non-none id -- N-ary: all args must agree; an absent arg is
// obscure), or alias_other_v (leave as seeded). single[] arrives
// PRE-SEEDED (the allocations; the wrapper's Alloc pass) and is
// resolved in place: rounds over the record list (max_rounds is
// the client's patience cap -- the compiler's historical 64,
// kept as policy), each transition sets grew. The values are
// client ids (allocation sites, handle classes, whatever the
// "proves exactly one X" question is about).
inline constexpr uintlen_t alias_other_v = 0;
inline constexpr uintlen_t alias_copy_v = 1;
inline constexpr uintlen_t alias_phi_v = 2;
struct alias_def_t {
  uintlen_t var{};
  uintlen_t kind{alias_other_v};
  uintlen_t src{};                  // copy source (copy only)
  std::vector<uintlen_t> args{};    // phi args, raw (phi only)
};
template <version_t version_v>
MJZ_CX_FN void resolve_single_ids(const std::vector<alias_def_t> &defs,
                                  std::vector<uintlen_t> &single,
                                  uintlen_t n_vars,
                                  uintlen_t max_rounds) noexcept {
  bool grew = true;
  for (uintlen_t guard = 0; grew && guard < max_rounds; ++guard) {
    grew = false;
    for (const alias_def_t &d : defs) {
      if (d.var >= n_vars)
        continue;
      if (d.kind == alias_copy_v) {
        if (d.src < n_vars && single[d.var] != single[d.src]) {
          single[d.var] = single[d.src];
          grew = true;
        }
      } else if (d.kind == alias_phi_v) {
        // single iff EVERY arg is a valid var, none obscure, and
        // all carry the SAME id (an absent arg is obscure)
        bool ok = !d.args.empty();
        uintlen_t want = none_v;
        for (uintlen_t k = 0; k < d.args.size() && ok; ++k) {
          const uintlen_t a = d.args[k];
          if (a >= n_vars || single[a] == none_v) {
            ok = false;
            break;
          }
          if (k == 0)
            want = single[a];
          else
            ok &= (single[a] == want);
        }
        want = ok ? want : none_v;
        if (single[d.var] != want) {
          single[d.var] = want;
          grew = true;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// mem_ns: the dominator-scoped forwarding table (load elimination)
// ---------------------------------------------------------------------------
// THE CONTRACT. A table of ENTRIES (group, key-tag, key payload,
// value, from_store) lives along the dominator path: entering a
// child watermarks the creation sequence, leaving truncates every
// entry created inside the subtree (seq > watermark -- positions
// would regrow garbage after a deep join drop emptied them, so
// scope death goes by creation order, never by resize(mark)).
// The client supplies, per block, an EVENT list in program order:
//   mem_store_v  -- kill every entry the MAY_KILL predicate names
//                   (the memory model: which remembered writes a
//                   new store can overwrite -- policy), then push
//                   the written slot (val none_v = kill only)
//   mem_load_v   -- first-match lookup on (group, key), the hit
//                   recorded for the client's rewrite, then the
//                   slot re-recorded as holding THIS value
//   mem_clear_v  -- the model lost track (obscure store, a call
//                   with handles): drop everything
// DROP_AT[b] names the blocks whose ENTRY state the client cannot
// treat as the dominator's continuation (joins: the memory state
// merges, a sibling arm's stores are invisible but invalidate the
// ancestor's "last write" claim) -- a mask INPUT, because a
// different memory model (phi-placed, like mem2reg) would pass
// all-zero. OUTPUT: one hit record per load event, in event order.
inline constexpr uintlen_t mem_store_v = 0;
inline constexpr uintlen_t mem_load_v = 1;
inline constexpr uintlen_t mem_clear_v = 2;
struct mem_event_t {
  uintlen_t kind{mem_load_v};
  uintlen_t group{};     // the allocation / slot-space id
  uintlen_t key_tag{};   // client key tag (const-ness, ...)
  uintlen_t key_a{};     // key payload A (the vreg key)
  uintlen_t key_b{};     // key payload B (the const value)
  uintlen_t val{none_v}; // store: the written value; load: the dst
};
struct mem_hit_t {
  bool found{};
  uintlen_t val{};
  bool from_store{};
};
template <version_t version_v, class MayKill>
MJZ_CX_FN std::vector<mem_hit_t> run_dom_scoped_table(
    const auto &dominator_tree, const auto &sites_of_block,
    const auto &drop_at, const auto &events_of_block,
    const MayKill &may_kill) noexcept {
  struct ent_t {
    uintlen_t group{}, key_tag{}, key_a{}, key_b{}, val{};
    bool from_store{};
    uintlen_t seq{};
  };
  std::vector<ent_t> table;
  uintlen_t seq_ctr = 0;
  auto push_ent = [&](ent_t e) noexcept {
    e.seq = ++seq_ctr;
    table.push_back(e);
  };
  std::vector<mem_hit_t> hits{};
  auto rows = dominator_tree.range();
  const uintlen_t nb = uintlen_t(std::ranges::size(rows));
  struct frame_t {
    uintlen_t b, child_idx, w;
  };
  std::vector<frame_t> st;
  st.push_back({0, 0, seq_ctr});
  while (!st.empty()) {
    frame_t &fr = st.back();
    const uintlen_t b = fr.b;
    if (fr.child_idx == 0) { // entering: scan the block's events once
      if (b < drop_at.size() && drop_at[b])
        table.clear(); // join: the memory state merges
      for (const auto &ee : events_of_block[b]) {
        const mem_event_t &ev = ee;
        if (ev.kind == mem_store_v) {
          size_t w = 0;
          for (size_t r = 0; r < table.size(); ++r)
            if (!may_kill(table[r].group, table[r].key_tag, table[r].key_a,
                          table[r].key_b, ev))
              table[w++] = table[r];
          table.resize(w);
          if (ev.val != none_v)
            push_ent({.group = ev.group,
                      .key_tag = ev.key_tag,
                      .key_a = ev.key_a,
                      .key_b = ev.key_b,
                      .val = ev.val,
                      .from_store = true});
        } else if (ev.kind == mem_load_v) {
          mem_hit_t h{};
          for (const ent_t &e : table)
            if (e.group == ev.group && e.key_tag == ev.key_tag &&
                (ev.key_tag ? e.key_b == ev.key_b : e.key_a == ev.key_a)) {
              h = {.found = true, .val = e.val, .from_store = e.from_store};
              break;
            }
          hits.push_back(h);
          push_ent({.group = ev.group,
                    .key_tag = ev.key_tag,
                    .key_a = ev.key_a,
                    .key_b = ev.key_b,
                    .val = ev.val,
                    .from_store = false});
        } else if (ev.kind == mem_clear_v) {
          table.clear();
        }
      }
    }
    auto ch = rows[b];
    if (fr.child_idx < uintlen_t(std::ranges::size(ch))) {
      const uintlen_t next = uintlen_t(ch[fr.child_idx++]);
      st.push_back({next, 0, seq_ctr});
      continue;
    }
    // leaving b: drop everything created inside its subtree
    {
      size_t wi = 0;
      for (size_t r = 0; r < table.size(); ++r)
        if (table[r].seq <= fr.w)
          table[wi++] = table[r];
      table.resize(wi);
    }
    st.pop_back();
  }
  return hits;
}

// ---------------------------------------------------------------------------
// iv_ns / ai_ns: the interval lattice + the forward abstract-
// interpretation driver (range analysis)
// ---------------------------------------------------------------------------
// THE SPLIT (the sccp precedent: propagation is the algorithm,
// semantics is policy). iv_ns is the INTERVAL LATTICE -- pure
// int64 math, no op vocabulary: hull (the join), equality, the
// narrowing meet, and the trap-safety predicates. The TRANSFERS
// (what Add does to intervals, what a taken comparison refines)
// stay with the client -- they speak the compiler's op ids.
// ai_ns::solve_forward is the DRIVER: plain rounds growing up
// from bottom, then WIDENING at the client's chosen points only
// (the Cousot discipline -- widening everywhere re-inflates
// refined states), then NARROWING rounds intersecting back down;
// a no-fixpoint give-up is reported and the client falls back to
// sound TOP. Callbacks (per the std::sort-comparator rule):
//   entry_top(st)              -- the entry block's initial state
//   refine_edge(p, b, st)      -- the fact the p->b edge carries
//   phi_join(b, st, exits, nv) -- the phi rule (exits = the pred
//                                exit table, read at p*nv+v)
//   transfer(b, st)            -- walk b's body through st
// The visit order (blocks ascending, preds in row order, rounds
// capped by the client's patience) is the driver's; the states
// are a pure function of that order, which is what keeps the
// rewrite byte-identical when the driver moves into the kernel.
namespace iv_ns {
struct iv_t {
  int64_t lo{INT64_MIN}, hi{INT64_MAX};
  bool bot{}; // empty: unreachable program point
};
MJZ_CX_FN bool iv_equal(const iv_t &a, const iv_t &b) noexcept {
  return a.bot == b.bot && a.lo == b.lo && a.hi == b.hi;
}
MJZ_CX_FN iv_t iv_join(const iv_t &a, const iv_t &b) noexcept { // hull
  if (a.bot)
    return b;
  if (b.bot)
    return a;
  return {a.lo < b.lo ? a.lo : b.lo, a.hi > b.hi ? a.hi : b.hi, false};
}
// narrowing meet: the tighter of two sound approximations, kept
// only when it is still non-empty (bot is "unreachable", not
// "contradiction": the original skips bot operands untouched)
MJZ_CX_FN void iv_narrow(iv_t &old, const iv_t &st) noexcept {
  if (st.bot || old.bot)
    return;
  const int64_t lo = st.lo > old.lo ? st.lo : old.lo;
  const int64_t hi = st.hi < old.hi ? st.hi : old.hi;
  if (lo <= hi)
    old = {lo, hi, false};
}
// trap-safety predicates (a sound over-approximation answers
// them): divisor excludes 0 / excludes -1, dividend excludes MIN
MJZ_CX_FN bool iv_excludes_zero(const iv_t &i) noexcept {
  return i.bot || i.hi < 0 || i.lo > 0;
}
MJZ_CX_FN bool iv_excludes_minus1(const iv_t &i) noexcept {
  return i.bot || i.hi < -1 || i.lo > -1;
}
MJZ_CX_FN bool iv_excludes_min(const iv_t &i) noexcept {
  return i.bot || i.lo > INT64_MIN;
}
} // namespace iv_ns

namespace ai_ns {
template <version_t version_v> struct ai_result_t {
  std::vector<iv_ns::iv_t> entry{}; // nb * nv, block-entry states
  bool converged{};
};
// preds: raw pred rows; widen_at: the widening points (loop
// headers); entry_block: the start (its state = entry_top);
// plain_cap / widen_cap / narrow_rounds: the client's patience.
template <version_t version_v, class CB>
MJZ_CX_FN ai_result_t<version_v>
solve_forward(uintlen_t nb, uintlen_t nv, const auto &preds,
              const auto &widen_at, uintlen_t entry_block,
              uintlen_t plain_cap, uintlen_t widen_cap,
              uintlen_t narrow_rounds, const CB &cb) noexcept {
  ai_result_t<version_v> R{};
  if (!nb || !nv)
    return R;
  std::vector<iv_ns::iv_t> entry((size_t)nb * nv, iv_ns::iv_t{0, 0, true});
  std::vector<iv_ns::iv_t> exit_((size_t)nb * nv, iv_ns::iv_t{0, 0, true});
  std::vector<char> touched(nb, 0);
  std::vector<iv_ns::iv_t> st(nv), edge(nv);
  bool widening = false;
  uintlen_t round = 0;
  bool converged = false;
  for (;;) {
    bool changed = false;
    for (uintlen_t b = 0; b < nb; ++b) {
      // ---- entry = hull over incoming (refined) pred exits + phi rule
      for (uintlen_t v = 0; v < nv; ++v)
        st[v] = {0, 0, true};
      bool any_pred = false;
      for (const auto &pp : preds[b]) {
        const uintlen_t p = uintlen_t(pp);
        if (p >= nb || !touched[p])
          continue;
        any_pred = true;
        for (uintlen_t v = 0; v < nv; ++v)
          edge[v] = exit_[(size_t)p * nv + v];
        cb.refine_edge(p, b, edge); // the fact the edge carries
        for (uintlen_t v = 0; v < nv; ++v)
          st[v] = iv_ns::iv_join(st[v], edge[v]);
      }
      if (!any_pred && b != entry_block) // unreachable: keep bottom
        for (uintlen_t v = 0; v < nv; ++v)
          st[v] = {0, 0, true};
      if (b == entry_block)
        cb.entry_top(st);
      else
        cb.phi_join(b, st, exit_.data(), nv);
      // ---- convergence bookkeeping (+ widening at the chosen points)
      {
        bool diff = false;
        for (uintlen_t v = 0; v < nv; ++v)
          if (!iv_ns::iv_equal(st[v], entry[(size_t)b * nv + v])) {
            diff = true;
            break;
          }
        if (diff) {
          if (widening && b < widen_at.size() && widen_at[b]) {
            for (uintlen_t v = 0; v < nv; ++v) {
              iv_ns::iv_t &old = entry[(size_t)b * nv + v];
              if (st[v].bot)
                continue;
              if (st[v].lo < old.lo)
                st[v].lo = INT64_MIN; // grow the growing sides
              if (st[v].hi > old.hi)
                st[v].hi = INT64_MAX;
              if (st[v].lo > st[v].hi)
                st[v] = old; // paranoia
            }
          }
          for (uintlen_t v = 0; v < nv; ++v)
            entry[(size_t)b * nv + v] = st[v];
          changed = true;
        }
      }
      // ---- transfer through the block
      for (uintlen_t v = 0; v < nv; ++v)
        st[v] = entry[(size_t)b * nv + v];
      cb.transfer(b, st);
      for (uintlen_t v = 0; v < nv; ++v)
        exit_[(size_t)b * nv + v] = st[v];
      touched[b] = 1;
    }
    if (!changed) {
      converged = true;
      break;
    }
    ++round;
    if (!widening && round >= plain_cap)
      widening = true;
    if (widening && round >= plain_cap + widen_cap)
      break; // give up: the caller falls back to sound TOP
  }
  if (converged) {
    // ---- narrowing: re-transfer and intersect with the widened state
    for (uintlen_t n = 0; n < narrow_rounds; ++n) {
      for (uintlen_t b = 0; b < nb; ++b) {
        for (uintlen_t v = 0; v < nv; ++v)
          st[v] = {0, 0, true};
        for (const auto &pp : preds[b]) {
          const uintlen_t p = uintlen_t(pp);
          if (p >= nb || !touched[p])
            continue;
          for (uintlen_t v = 0; v < nv; ++v)
            edge[v] = exit_[(size_t)p * nv + v];
          cb.refine_edge(p, b, edge);
          for (uintlen_t v = 0; v < nv; ++v)
            st[v] = iv_ns::iv_join(st[v], edge[v]);
        }
        if (b == entry_block)
          cb.entry_top(st);
        else
          cb.phi_join(b, st, exit_.data(), nv);
        for (uintlen_t v = 0; v < nv; ++v)
          iv_ns::iv_narrow(entry[(size_t)b * nv + v], st[v]);
        for (uintlen_t v = 0; v < nv; ++v)
          st[v] = entry[(size_t)b * nv + v];
        cb.transfer(b, st);
        for (uintlen_t v = 0; v < nv; ++v)
          exit_[(size_t)b * nv + v] = st[v];
      }
    }
  }
  R.entry = std::move(entry);
  R.converged = converged;
  return R;
}
} // namespace ai_ns

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
                                  const auto &const_val,
                                  uintlen_t v) noexcept {
  if (v == none_v)
    return 0;
  if (v < n_vars && const_mask[v])
    return (uint64_t(1) << 63) ^ uint64_t(const_val[v]);
  return uint64_t(v);
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
MJZ_CX_FN void calculate_live_params(
    const auto &eligible, const auto &softs,
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

}; // namespace mjz::opt_ns
