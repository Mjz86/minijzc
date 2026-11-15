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

}; // namespace mjz::opt_ns
