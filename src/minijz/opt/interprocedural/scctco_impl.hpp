#pragma once
// === scctco_impl.hpp ========================================
// the SCC-TAILCALL MEMBER-COLLAPSE SUPPORT layer, split out of
// scctco.hpp (the code-quality continuation, round nine -- the
// cdfold/lcf/unroll/fold/unswitch_bif/taildup/mem2reg/rotate
// treatment): the collapse helpers (the tail-call predicate, the
// block-editing quartet), the merge-head record, the per-member
// context (what the former 392-line scctco_member held in
// locals), and the phase functions (the D+H prepend, the merge-
// phi machinery, the site drain, the sibling snapshot inline, the
// pass-through rotation). The slim driver -- the phase list --
// stays in scctco.hpp.
//
// ALGORITHM (the phase list; every order below is the original's):
//   gate    nparams <= 6 and block 0 phi-free (the entry is D
//           material's predecessor; phis there would re-key wrong).
//   prepend D (block 0: t0=511, k1=1, z0=0, br H) + H (block 1,
//           br E); every old block/edge shifts by 2. The carried
//           set is NV = nparams + 1 values (params + trip).
//   drain   one internal tail site per round (iter_cap = 64 + 16*
//           snapshots; the site scan is ascending block, ascending
//           instr, FIRST site wins): the frame guard mints tp/cmp,
//           (first site only) the shared trap block's c1/c0/dx,
//           rebuilds the site's block minus [the Call; its Ret] as
//           [r'=r-k1; cmp=r'<z0; cbr trapb/-], then dispatches --
//           self site -> st_add_backedge(self); first edge to a
//           sibling -> st_inline_sibling (the PRE-collapse
//           snapshot body, params mapped to the call's args);
//           further sibling edge -> st_add_backedge(heads[sib]).
//   fixup   the raw-param rotation (3b): a phi arg still holding a
//           RAW param vreg on any edge but the true entry reads
//           the CURRENT param phi instead (pass-through params).
//   tail    resucc; scctco_fold on the pristine shape; resucc.
//
// THE OBSERVABLE-ORDER LEDGER (vreg ids are observable -- the
// audit table keys on them; every mint below keeps the original's
// order exactly):
//   * t0, k1, z0 (phase 1, in that order) -- D's three consts.
//   * st_promote: one dst per carried value v = 0..NV-1 ascending
//     (base_of(root_vals[v])); the self head's phi_vals is the
//     first nparams of them.
//   * st_add_backedge's chain block: one dst per v ascending
//     (base_of(tip_vals[v])), then the appended block.
//   * the frame guard: tp, then cmp, then (first site only)
//     c1, c0, dx -- the trap block's consts.
//   * st_inline_sibling: vmap[v] for v = 0..nvG-1 ascending (v <
//     G.nparams reuses args[v], else a fresh base_of(v) anon).
//
// CONTRACT (byte-identical differential, the ckpt 23-28 protocol):
//   * the drain's three exits are distinct observables -- bail (a
//     non-tail internal site surfaced: the member returns FALSE,
//     dropping even prior rounds' changed), capped (iter bound:
//     return changed NOW, skipping the fixup/fold tail -- the
//     remaining sites stay ordinary calls, always safe), done
//     (run the tail). st_drain_t spells them out.
//   * the `continue'-vs-`break'-vs-`return' discipline of the
//     site scan is behavior; st_find_site returns the tri-state.
//   * the sibling entry-block patch (bug 3) rewrites NON-PHI
//     reads only, params then trip, per instr -- phi args keep
//     their raw root-edge values (rewriting them would make each
//     phi self-referential on its root edge).
//   * preserved asymmetry, documented: scctco_set_block treats
//     blocks after b by begin >= en (a block ENDING at en is
//     unaffected), while scctco_prepend shifts by begin > pos --
//     both original, both load-bearing for adjacent-block shapes.
//   * trip_read's heads scan is ORDER-INSENSITIVE only because the
//     sibling ranges [cb,ce) are disjoint by construction; the
//     first match wins, exactly like the original's linear walk.
#include "../../ir/instr.hpp"

// ---- the tail-call predicate ----------------------------------------------

// uses of vreg v anywhere in fn (a/b2/c2/args; phi args included)
constexpr uint32_t scctco_uses(const Fn &fn, uint32_t v) {
  uint32_t n = 0;
  for (const Instr &I : fn.code) {
    if (I.a == v)
      ++n;
    if (I.b2 == v)
      ++n;
    if (I.c2 == v)
      ++n;
    for (uint32_t u : I.args)
      if (u == v)
        ++n;
  }
  return n;
}

// Is the Call at (block b, index i) a TAIL call: its dst is read ONLY by
// the block's terminating Ret (which returns it)?
constexpr bool scctco_is_tail(const Fn &fn, uint32_t b, uint32_t i) {
  const Instr &I = fn.code[i];
  if (I.k != K::Call || I.dst == NONE)
    return false;
  const Block &B = fn.blocks[b];
  if (B.begin >= B.end)
    return false; // empty/tombstone: no terminator
  if (fn.code[B.end - 1].k != K::Ret)
    return false;
  if (fn.code[B.end - 1].a != I.dst)
    return false;
  return scctco_uses(fn, I.dst) == 1;
}

// ---- the block-editing quartet ---------------------------------------------

// one instr's operand rewrite (a/b2/c2/args), the leaf both the
// range rewrite and the sibling entry patch walk
constexpr void st_rw_one(Instr &I, uint32_t from, uint32_t to) {
  if (I.a == from)
    I.a = to;
  if (I.b2 == from)
    I.b2 = to;
  if (I.c2 == from)
    I.c2 = to;
  for (uint32_t &u : I.args)
    u = (u == from) ? to : u;
}

// prepend instrs to block `blk` (phis must stay first); later blocks shift
constexpr void scctco_prepend(Fn &fn, uint32_t blk,
                              const std::vector<Instr> &ins) {
  const uint32_t pos = fn.blocks[blk].begin;
  fn.code.insert(fn.code.begin() + pos, ins.begin(), ins.end());
  const int64_t d = (int64_t)ins.size();
  for (uint32_t q = 0; q < fn.nblocks(); ++q) {
    Block &B = fn.blocks[q];
    if (q == blk) {
      B.end = (uint32_t)((int64_t)B.end + d);
      continue;
    }
    if (B.begin > pos) { // a block ending AT pos is unaffected
      B.begin = (uint32_t)((int64_t)B.begin + d);
      B.end = (uint32_t)((int64_t)B.end + d);
    }
  }
}

// replace block b's code wholesale (block ranges after it shift)
constexpr void scctco_set_block(Fn &fn, uint32_t b, std::vector<Instr> ns) {
  const uint32_t bg = fn.blocks[b].begin, en = fn.blocks[b].end;
  fn.code.erase(fn.code.begin() + bg, fn.code.begin() + en);
  fn.code.insert(fn.code.begin() + bg, ns.begin(), ns.end());
  const int64_t d = (int64_t)ns.size() - (int64_t)(en - bg);
  fn.blocks[b].end = bg + (uint32_t)ns.size();
  for (uint32_t q = 0; q < fn.nblocks(); ++q) {
    if (q == b)
      continue;
    Block &B = fn.blocks[q];
    if (B.begin >= en) {
      B.begin = (uint32_t)((int64_t)B.begin + d);
      B.end = (uint32_t)((int64_t)B.end + d);
    }
  }
}

// point block b's terminator edge `from` -> `to` (from == NONE sets the
// unset false arm of a fresh guard Cbr)
constexpr void scctco_retarget(Fn &fn, uint32_t b, uint32_t from, uint32_t to) {
  const Block &B = fn.blocks[b];
  if (B.begin >= B.end)
    return; // empty/tombstone
  Instr &T = fn.code[B.end - 1];
  if (T.k == K::Br && T.t == from)
    T.t = to;
  else if (T.k == K::Cbr) {
    if (T.t == from)
      T.t = to;
    if (T.f == from)
      T.f = to;
  }
}

// rewrite uses of vreg `from` -> `to` in blocks [lo, hi) except `skip`
constexpr void scctco_rewrite(Fn &fn, uint32_t lo, uint32_t hi, uint32_t skip,
                              uint32_t from, uint32_t to) {
  for (uint32_t b = lo; b < hi && b < fn.nblocks(); ++b) {
    if (b == skip)
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i)
      st_rw_one(fn.code[i], from, to);
  }
}

// ---- the merge head + the per-member context --------------------------------

struct ScctcoHead {
  uint32_t entry{NONE};     // block carrying the merge phis
  uint32_t root_pred{NONE}; // the one non-back-edge predecessor
  std::vector<uint32_t> root_vals{};
  bool phis{false};
  uint32_t tip_pred{NONE}; // current second predecessor
  std::vector<uint32_t> tip_vals{};
  uint32_t cb{NONE}, ce{NONE};      // the sibling copy's block range
  uint32_t trip_src{NONE};          // trip ARRIVING at the entry (pre-phi)
  uint32_t trip_phi{NONE};          // ... and the phi result (post-phi)
  std::vector<uint32_t> phi_vals{}; // self only: the param rotation phis
};

struct ScctcoCtx {
  Fn &f;
  const std::vector<Fn> &snap;         // PRE-collapse member snapshots
  const std::vector<uint32_t> &snap_of; // fn id -> snapshot id (NONE: out)
  uint32_t nfn{}, self_fn{};
  uint32_t nparams{}, NV{}; // carried values: params + trip
  uint32_t t0{}, k1{}, z0{}; // D's consts: 511 / 1 / 0
  ScctcoHead self{};
  std::vector<ScctcoHead> heads{};
  uint32_t trapb{NONE}; // the shared 1/0 trap block
  uint32_t T_cur{NONE}; // the trip vreg f-region sites read
  bool changed{false};
  constexpr ScctcoCtx(Fn &f_, const std::vector<Fn> &snap_,
                      const std::vector<uint32_t> &snap_of_, uint32_t nfn_,
                      uint32_t self_fn_)
      : f(f_), snap(snap_), snap_of(snap_of_), nfn(nfn_),
        self_fn(self_fn_), nparams(f_.nparams) {}
};

// ---- the gates --------------------------------------------------------------

// the member is collapsible: <= 6 params (SysV register budget of the
// collapse's shape) and a phi-free entry (D prepends BEFORE it; entry
// phis would need re-keying the collapse never does)
constexpr bool st_gate_ok(const Fn &f) {
  if (f.nparams > 6)
    return false;
  for (uint32_t i = f.blocks[0].begin; i < f.blocks[0].end; ++i)
    if (f.code[i].k == K::Phi)
      return false; // entry must be phi-free
  return true;
}

// ---- phase 1: prepend D + H -------------------------------------------------

constexpr void st_prepend_dh(Fn &f, ScctcoCtx &C) {
  const uint32_t nb0 = f.nblocks();
  // trip counts DOWN from 511 (the frame budget): each collapsed edge does
  // r' = r - 1 and traps past r' < 0 (edge #512 = the 513th virtual call,
  // exactly the depth cap). A DOWN counter is also what makes the interval
  // range analysis converge in ~2 rounds: the guard's false arm refines
  // r' >= 0 immediately, bounding BOTH sides of the phi (an UP counter's
  // interval grows +1 per round for ~512 rounds before widening, which
  // multiplied by inlined copies made analysis of the caller quadratic).
  C.t0 = new_anon_vreg(f, 0); // r = 511 on the entry edge
  C.k1 = new_anon_vreg(f, 0); // 1 (the guard decrement)
  C.z0 = new_anon_vreg(f, 0); // 0 (the guard comparison)
  {
    std::vector<Instr> nc;
    nc.reserve(f.code.size() + 8);
    nc.push_back(Instr{.k = K::Const, .dst = C.t0, .c = 511});
    nc.push_back(Instr{.k = K::Const, .dst = C.k1, .c = 1});
    nc.push_back(Instr{.k = K::Const, .dst = C.z0, .c = 0});
    nc.push_back(Instr{.k = K::Br, .t = 1});
    nc.push_back(Instr{.k = K::Br, .t = 2}); // H: phis come with the 1st edge
    for (uint32_t b = 0; b < nb0; ++b)
      for (uint32_t i = f.blocks[b].begin; i < f.blocks[b].end; ++i) {
        Instr I = f.code[i];
        if (I.k == K::Br) {
          if (I.t != NONE)
            I.t += 2;
        } else if (I.k == K::Cbr) {
          if (I.t != NONE)
            I.t += 2;
          if (I.f != NONE)
            I.f += 2;
        } else if (I.k == K::Phi) {
          if (I.t != NONE)
            I.t += 2;
          if (I.f != NONE)
            I.f += 2;
        }
        nc.push_back(I);
      }
    std::vector<Block> nbl;
    std::vector<uint32_t> nsucc;
    nbl.push_back({0, 4}); // D:  [t0, k1, z0, br H]
    nsucc.push_back(1);
    nsucc.push_back(NONE);
    nbl.push_back({4, 5}); // H:  [br E] (phis come with the first edge)
    nsucc.push_back(2);
    nsucc.push_back(NONE);
    for (uint32_t b = 0; b < nb0; ++b) {
      nbl.push_back({5 + f.blocks[b].begin, 5 + f.blocks[b].end});
      nsucc.push_back(f.succ[2 * b] == NONE ? NONE : f.succ[2 * b] + 2);
      nsucc.push_back(f.succ[2 * b + 1] == NONE ? NONE : f.succ[2 * b + 1] + 2);
    }
    f.code = std::move(nc);
    f.blocks = std::move(nbl);
    f.succ = std::move(nsucc);
  }
  f.recompute_preds();

  C.self.entry = 1;
  C.self.root_pred = 0;
  for (uint32_t p = 0; p < C.nparams; ++p)
    C.self.root_vals.push_back(p);
  C.self.root_vals.push_back(C.t0);
  C.self.trip_src = C.t0;
  C.NV = C.nparams + 1; // carried values: params + trip

  C.heads.assign(C.nfn, ScctcoHead{});
  C.T_cur = C.t0; // the trip vreg f-region sites read
}

// ---- the merge-phi machinery ------------------------------------------------

// append a block of instrs at the end; returns its id
constexpr uint32_t st_append_block(Fn &f, const std::vector<Instr> &ins) {
  uint32_t id = f.nblocks();
  uint32_t base = (uint32_t)f.code.size();
  f.code.insert(f.code.end(), ins.begin(), ins.end());
  f.blocks.push_back({base, (uint32_t)f.code.size()});
  f.succ.push_back(NONE);
  f.succ.push_back(NONE);
  return id;
}

// the trip vreg a site in block b increments: f-level, or the copy's
// (a copy's sites count from the trip ARRIVING at the copy's entry)
constexpr uint32_t st_trip_read(const ScctcoCtx &C, uint32_t blk) {
  for (const ScctcoHead &h : C.heads) {
    if (h.cb == NONE || blk < h.cb || blk >= h.ce)
      continue;
    return h.phis ? h.trip_phi : h.trip_src;
  }
  return C.T_cur;
}

// materialize the merge phis at Hd.entry for (root edge + first edge b)
constexpr void st_promote(Fn &f, ScctcoCtx &C, ScctcoHead &Hd, uint32_t b,
                          const std::vector<uint32_t> &vals) {
  std::vector<Instr> phis;
  std::vector<uint32_t> res;
  for (uint32_t v = 0; v < C.NV; ++v) {
    uint32_t dst = new_anon_vreg(f, f.base_of(Hd.root_vals[v]));
    Instr P{
        .k = K::Phi, .dst = dst, .c = (int64_t)f.base_of(Hd.root_vals[v])};
    if (Hd.root_pred < b) {
      P.a = Hd.root_vals[v];
      P.t = Hd.root_pred;
      P.b2 = vals[v];
      P.f = b;
    } else {
      P.a = vals[v];
      P.t = b;
      P.b2 = Hd.root_vals[v];
      P.f = Hd.root_pred;
    }
    phis.push_back(P);
    res.push_back(dst);
  }
  if (Hd.root_pred == 0) // the self head: remember the param phis
    Hd.phi_vals.assign(res.begin(), res.begin() + C.nparams);
  scctco_prepend(f, Hd.entry, phis);
  if (Hd.cb == NONE) { // self: whole body (D=0 / H=1 excluded)
    for (uint32_t v = 0; v < C.nparams; ++v)
      scctco_rewrite(f, 2, f.nblocks(), NONE, Hd.root_vals[v], res[v]);
    scctco_rewrite(f, 2, f.nblocks(), NONE, Hd.trip_src, res[C.nparams]);
    C.T_cur = res[C.nparams];
  } else { // sibling copy: local rewrite (entry block excluded)
    for (uint32_t v = 0; v < C.nparams; ++v)
      scctco_rewrite(f, Hd.cb, Hd.ce, Hd.entry, Hd.root_vals[v], res[v]);
    scctco_rewrite(f, Hd.cb, Hd.ce, Hd.entry, Hd.trip_src, res[C.nparams]);
    // BUGFIX (bug 3, scctco sibling-copy rotation): the copy's ENTRY
    // block is real body code -- the sibling's first block (its
    // base-case test; for a one-block sibling even a guard site).
    // Its reads were left pinned to the RAW root-edge vregs, so on
    // every back edge that does not pass through the root edge's
    // defining block the entry test saw a STALE argument forever:
    // fa(8) -> fb(2) -> fb(0) kept testing the fa-entry value (2) at
    // fb's `n==0`, never hit the base case and spun to the trip TRAP.
    // The phis yield exactly the value that arrived on the taken
    // edge, so rewriting the entry block's NON-PHI reads to them is
    // exact. Phi instructions are skipped: their args must keep the
    // raw root-edge value (rewriting them would make each phi
    // self-referential on its root edge).
    for (uint32_t i = f.blocks[Hd.entry].begin; i < f.blocks[Hd.entry].end;
         ++i) {
      Instr &I = f.code[i];
      if (I.k == K::Phi)
        continue;
      for (uint32_t v = 0; v < C.nparams; ++v)
        st_rw_one(I, Hd.root_vals[v], res[v]);
      st_rw_one(I, Hd.trip_src, res[C.nparams]);
    }
  }
  Hd.trip_phi = res[C.nparams];
  Hd.phis = true;
  Hd.tip_pred = b;
  Hd.tip_vals = vals;
}

// add a back edge (b, vals) to Hd, extending the merge chain as needed
constexpr void st_add_backedge(Fn &f, ScctcoCtx &C, ScctcoHead &Hd,
                               uint32_t b,
                               const std::vector<uint32_t> &vals) {
  if (!Hd.phis) {
    st_promote(f, C, Hd, b, vals);
    scctco_retarget(f, b, NONE, Hd.entry);
    return;
  }
  // further edge: one more 2-pred chain block
  std::vector<Instr> phis;
  std::vector<uint32_t> res;
  for (uint32_t v = 0; v < C.NV; ++v) {
    uint32_t dst = new_anon_vreg(f, f.base_of(Hd.tip_vals[v]));
    Instr P{.k = K::Phi, .dst = dst, .c = (int64_t)f.base_of(Hd.tip_vals[v])};
    if (Hd.tip_pred < b) {
      P.a = Hd.tip_vals[v];
      P.t = Hd.tip_pred;
      P.b2 = vals[v];
      P.f = b;
    } else {
      P.a = vals[v];
      P.t = b;
      P.b2 = Hd.tip_vals[v];
      P.f = Hd.tip_pred;
    }
    phis.push_back(P);
    res.push_back(dst);
  }
  phis.push_back(Instr{.k = K::Br, .t = Hd.entry});
  const uint32_t M = st_append_block(f, phis);
  scctco_retarget(f, Hd.tip_pred, Hd.entry, M);
  scctco_retarget(f, b, NONE, M);
  // the entry phis' tip-side arg now arrives from M
  uint32_t v = 0;
  for (uint32_t i = f.blocks[Hd.entry].begin; i < f.blocks[Hd.entry].end;
       ++i) {
    Instr &P = f.code[i];
    if (P.k != K::Phi)
      break;
    if (P.t == Hd.tip_pred) {
      P.t = M;
      P.a = res[v];
    }
    if (P.f == Hd.tip_pred) {
      P.f = M;
      P.b2 = res[v];
    }
    ++v;
  }
  Hd.tip_pred = M;
  Hd.tip_vals = res;
}

// ---- the site drain ---------------------------------------------------------

// the first still-present internal tail call (ascending block, ascending
// instr). bail = a non-tail internal Call surfaced (cannot happen: the
// driver pre-checked every internal edge -- but the check is load-bearing
// against future driver edits, and the original returned false on it)
enum class st_site_t : uint8_t { none_e, found_e, bail_e };
constexpr st_site_t st_find_site(Fn &f, ScctcoCtx &C, uint32_t &sb,
                                 uint32_t &ci, uint32_t &callee) {
  sb = NONE;
  ci = NONE;
  callee = NONE;
  for (uint32_t b = 0; b < f.nblocks() && sb == NONE; ++b) {
    for (uint32_t i = f.blocks[b].begin; i < f.blocks[b].end; ++i) {
      const Instr &I = f.code[i];
      if (I.k != K::Call)
        continue;
      uint32_t g = (uint32_t)I.c;
      if (g >= C.nfn || C.snap_of[g] == NONE)
        continue; // not in this SCC
      if (!scctco_is_tail(f, b, i))
        return st_site_t::bail_e;
      sb = b;
      ci = i;
      callee = g;
      break;
    }
  }
  return sb == NONE ? st_site_t::none_e : st_site_t::found_e;
}

// the shared 1/0 trap block (1/0 div: every backend's TRAP), minted once
constexpr void st_ensure_trapb(Fn &f, ScctcoCtx &C) {
  if (C.trapb != NONE)
    return;
  const uint32_t c1 = new_anon_vreg(f, 0), c0 = new_anon_vreg(f, 0),
                 dx = new_anon_vreg(f, 0);
  C.trapb = st_append_block(f,
                            {Instr{.k = K::Const, .dst = c1, .c = 1},
                             Instr{.k = K::Const, .dst = c0, .c = 0},
                             Instr{.k = K::Bin, .b = B::Div, .dst = dx,
                                   .a = c1, .b2 = c0},
                             Instr{.k = K::Ret, .a = dx}});
}

// the frame guard: [r'=r-k1; cmp=r'<z0; cbr trapb/-] over the site's
// block minus [the Call; its Ret]; returns tp (the decremented trip)
constexpr uint32_t st_guard_site(Fn &f, ScctcoCtx &C, uint32_t sb,
                                 uint32_t ci, uint32_t tripv) {
  const uint32_t tp = new_anon_vreg(f, 0);
  const uint32_t cmp = new_anon_vreg(f, 0);
  st_ensure_trapb(f, C);
  std::vector<Instr> ns;
  for (uint32_t i = f.blocks[sb].begin; i < f.blocks[sb].end; ++i)
    if (i != ci && i + 1 != f.blocks[sb].end)
      ns.push_back(f.code[i]);
  ns.push_back(
      Instr{.k = K::Bin, .b = B::Sub, .dst = tp, .a = tripv, .b2 = C.k1});
  ns.push_back(
      Instr{.k = K::Bin, .b = B::Lt, .dst = cmp, .a = tp, .b2 = C.z0});
  ns.push_back(Instr{.k = K::Cbr, .a = cmp, .t = C.trapb, .f = NONE});
  scctco_set_block(f, sb, std::move(ns));
  return tp;
}

// inline the sibling's PRE-collapse snapshot body once; the copy's entry
// block becomes its merge head, the site's edge retargeted into it
constexpr void st_inline_sibling(Fn &f, ScctcoCtx &C, uint32_t callee,
                                 uint32_t sb,
                                 const std::vector<uint32_t> &args,
                                 const std::vector<uint32_t> &vals) {
  const Fn &G = C.snap[C.snap_of[callee]];
  const uint32_t nvG = G.nvars + (uint32_t)G.version_base.size();
  std::vector<uint32_t> vmap(nvG, NONE);
  for (uint32_t v = 0; v < nvG; ++v)
    vmap[v] = (v < G.nparams) ? args[v] : new_anon_vreg(f, G.base_of(v));
  auto mapv = [&](uint32_t v) -> uint32_t {
    return (v == NONE || v >= nvG) ? v : vmap[v];
  };
  const uint32_t nbG = G.nblocks();
  const uint32_t cb = f.nblocks();
  for (uint32_t g = 0; g < nbG; ++g) {
    std::vector<Instr> gb;
    for (uint32_t i = G.blocks[g].begin; i < G.blocks[g].end; ++i) {
      Instr I = G.code[i];
      if (I.dst != NONE)
        I.dst = mapv(I.dst);
      I.a = mapv(I.a);
      I.b2 = mapv(I.b2);
      if (I.c2 != NONE)
        I.c2 = mapv(I.c2);
      for (uint32_t &av : I.args)
        av = mapv(av);
      if (I.k == K::Phi) {
        if (I.t != NONE && I.t < nbG)
          I.t = cb + I.t;
        if (I.f != NONE && I.f < nbG)
          I.f = cb + I.f;
      } else if (I.k == K::Br) {
        if (I.t != NONE && I.t < nbG)
          I.t = cb + I.t;
      } else if (I.k == K::Cbr) {
        if (I.t != NONE && I.t < nbG)
          I.t = cb + I.t;
        if (I.f != NONE && I.f < nbG)
          I.f = cb + I.f;
      }
      gb.push_back(I);
    }
    st_append_block(f, gb);
  }
  const uint32_t ce = f.nblocks();
  C.heads[callee].entry = cb;
  C.heads[callee].root_pred = sb;
  C.heads[callee].root_vals = vals;
  C.heads[callee].cb = cb;
  C.heads[callee].ce = ce;
  C.heads[callee].trip_src = vals[C.nparams]; // the ARRIVING trip
  scctco_retarget(f, sb, NONE, cb);
}

// ---- phase 3b: the raw-param rotation ---------------------------------------

// A phi incoming arg that is still a RAW param vreg (arg < nparams)
// means the tail call passed that parameter through unchanged: gcd's
// `gcd(b, a % b)`, flat argument swaps, binary search's pass-through
// bounds. Every BODY read of p_w was rewritten to the rotation phi
// P_w -- but the phi args themselves were not: they sit in head blocks,
// which the rewrites skip (the self entry) or which were appended after
// the rewrite ran (merge-chain blocks). Pinned to the raw param, the
// loop would feed the ORIGINAL argument back forever. On every edge
// except the true entry (pred 0 = block D) the value that flows is the
// CURRENT P_w, so patch those args to the phi. This is also harmless
// for one-shot edges: on the first iteration P_w still holds the
// original argument. When self never got a back edge (self.phis false,
// nothing rotates anywhere), raw params are already correct.
constexpr void st_rotate_raw_params(Fn &f, ScctcoCtx &C) {
  if (!C.self.phis)
    return;
  for (uint32_t b = 1; b < f.nblocks(); ++b)
    for (uint32_t i = f.blocks[b].begin; i < f.blocks[b].end; ++i) {
      Instr &P = f.code[i];
      if (P.k != K::Phi)
        continue;
      if (P.a != NONE && P.a < C.nparams && P.t != 0)
        P.a = C.self.phi_vals[P.a];
      if (P.b2 != NONE && P.b2 < C.nparams && P.f != 0)
        P.b2 = C.self.phi_vals[P.b2];
    }
}

// ---- phase 2: drain the internal tail sites ---------------------------------

// Hard iteration bound: each round converts exactly one tail site, and
// the site count is bounded by the SCC's total code, so anything past
// that means a construction bug (or a constexpr-evaluation pathology);
// bailing leaves the remaining sites as ordinary calls, which is always
// semantically safe -- just less optimized. The three exits (done /
// capped / bail) are distinct observables -- see the contract above.
enum class st_drain_t : uint8_t { done_e, capped_e, bail_e };
constexpr st_drain_t st_drain_sites(Fn &f, ScctcoCtx &C) {
  const uint32_t iter_cap = 64 + 16 * (uint32_t)C.snap.size();
  uint32_t iters = 0;
  for (bool progress = true; progress;) {
    if (++iters > iter_cap)
      return st_drain_t::capped_e;
    progress = false;
    // find the first still-present internal tail call
    uint32_t sb = NONE, ci = NONE, callee = NONE;
    const st_site_t s = st_find_site(f, C, sb, ci, callee);
    if (s == st_site_t::bail_e)
      return st_drain_t::bail_e; // cannot happen (the driver pre-checked)
    if (s == st_site_t::none_e)
      break;
    progress = true;
    C.changed = true;
    const std::vector<uint32_t> args = f.code[ci].args;
    // frame guard: r' = r - 1; trap if r' < 0 (the 513th virtual call)
    const uint32_t tripv = st_trip_read(C, sb);
    const uint32_t tp = st_guard_site(f, C, sb, ci, tripv);
    std::vector<uint32_t> vals = args;
    vals.push_back(tp);
    if (callee == C.self_fn) {
      st_add_backedge(f, C, C.self, sb, vals);
    } else if (C.heads[callee].entry == NONE) {
      st_inline_sibling(f, C, callee, sb, args, vals);
    } else {
      st_add_backedge(f, C, C.heads[callee], sb, vals);
    }
  }
  return st_drain_t::done_e;
}
