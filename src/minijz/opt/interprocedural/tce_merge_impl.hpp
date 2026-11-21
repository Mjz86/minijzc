#pragma once
// === tce_merge_impl.hpp ========================================
// the TAIL-CALL-ELIMINATION SUPPORT layer, split out of tailrec.hpp
// (the code-quality continuation, round eleven -- the unswitch round
// ten treatment): the shared tce vocabulary (the member/site caps,
// is_tail_call, the budget-init / site-check / trap emitters that
// tce_self and tce_merge both speak), TceSite, and tce_merge's
// machinery -- TmCtx (the per-merge context the former 357-line
// monolith held in locals) + the tm_* phase functions. The slim
// driver -- the phase list + the pass bookkeeping -- stays in
// tailrec.hpp next to tce_self and the SCC walk.
//
// ALGORITHM (the phase list; every order below is the original's):
//   collect  tm_collect_sites: the member-count gates (2..
//           MJZ_TCE_MAX_MEMBERS), the max-arity gate (1+M params
//           must fit SysV's 6), then the intra-SCC tail-site census
//           per member (count_operand_uses + is_tail_call; a
//           non-tail intra edge bails -- mixed SCCs are left alone;
//           out-of-SCC calls are NOT sites, they are copied as real
//           calls), then the site-count gate (2..MJZ_TCE_MAX_SITES).
//   alloc    tm_alloc: Fm(sel, a0..a{M-1})'s shell, the block
//           layout (0=P0, 1=H, 2..k-1 the dispatch chain, then the
//           member bodies back to back at bmap_base[x], then the
//           latch chain M_1..M_{ks-1}, then T), and the vreg
//           PRE-creation in exactly this order: selHv, aHv[0..M-1],
//           bHv, the member vmaps (params map to the aHv ids, the
//           rest are fresh), the per-site siteBL/siteEQ pairs, the
//           latch outputs Mp/Mbv. Pre-creation matters: the header
//           and latch phis reference these ids BEFORE the blocks
//           that define them are emitted.
//   header   tm_emit_header: P0 (budget = CAP+1 - Depth(); br H),
//           H (the sel/a/budget phis + the sel==1 dispatch), the
//           dispatch chain blocks 2..k-1 (sel==i -> E_i : next).
//   members  tm_emit_members: every member body verbatim through
//           the member vmap (remap_vreg_fields -- mapv maps NONE to
//           NONE, so the shared guarded form matches), block ids
//           rekeyed by bmap_base[x]; a member's tail Ret becomes
//           [sel'=callee Const; the zero-pad consts for unused
//           args; the budget check; cbr T : chain-next] and its
//           tail Call dies.
//   latches  tm_emit_latches: the latch merge chain (DESCENDING j,
//           M_{ks-1} first -- ids come from the layout pass and
//           positions from emission, decoupled by design; contrast
//           tce_self's bug #1, where ids were derived from
//           positions), the phis for [sel, a_0..a_{M-1}, budget]
//           merging consecutive site edges, then the trap block;
//           then the commit (Fm.code/blocks move, resucc, tce_sel).
//   callers  tm_rewrite_callers: every external call to a member
//           becomes Fm(sel=m, ...) -- the sel Const + arg pads
//           spliced right before the call, the call retargeted to
//           FmIdx, the block ranges shifted past the splice.
//
// THE LANDMINES (preserved exactly, per the house rule):
//   * new_anon_vreg allocation order is OBSERVABLE (the audit table
//     keys on vreg ids): the phases must run in exactly the
//     monolith's sequence, and within tm_alloc in exactly the
//     monolith's nesting (selHv/aHv/bHv BEFORE the member vmaps,
//     siteBL/siteEQ pairs site-major, Mp/Mbv latch-major).
//   * TmCtx's id fields default to NONE exactly like the monolith's
//     locals did (`uint32_t bInitV = NONE;`, the NONE-filled Mb/
//     siteSEL/siteBL/siteEQ/Mp/Mbv vectors) -- the UsCtx lesson
//     (ckpt 34): a context struct replacing function locals must
//     copy the locals' INITIALIZERS exactly.
//   * siteSEL is NOT pre-created (unlike siteBL/siteEQ): it is
//     allocated at the member's Ret emission, and the latch phis
//     read it only after every member is emitted.
//   * site args are PADDED in place during member emission (the
//     lazily-allocated zero const, once per site) so the latch
//     chain's sitearg can index [sel', a_0..a_{M-1}] uniformly --
//     the mutation of TmCtx::sites is part of the protocol.
//   * the dispatch chain's false arm: the sel==i check falls to
//     block i+1 (the next dispatch), the LAST one falls to
//     bmap_base[0] (member 0's entry) -- bug #2's fix; k==2 has no
//     chain and falls straight to member 0. The historical
//     `if (k == 2) emit_dispatch(1); else emit_dispatch(1);` is
//     kept verbatim (both arms identical; the shape split documents
//     itself).
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"

constexpr uint32_t MJZ_TCE_MAX_MEMBERS = 5;  // sel dispatch chain depth
constexpr uint32_t MJZ_TCE_MAX_SITES = 8;    // tail sites per merge
constexpr uint32_t MJZ_TCE_MAX_ARITY = 5;    // 1+M params must fit SysV's 6
constexpr uint32_t MJZ_TCE_MAX_FNSIZE = 400; // members stay mergeable

// is the Call at index ci in block b of fn a tail call? (dst used exactly
// once, by the block's terminating Ret, after the call)
constexpr bool is_tail_call(const Fn &fn, uint32_t b, uint32_t ci,
                            const std::vector<uint32_t> &uses) {
  const Instr &I = fn.code[ci];
  if (I.k != K::Call || I.dst == NONE)
    return false;
  if (I.dst >= uses.size() || uses[I.dst] != 1)
    return false;
  const uint32_t end = fn.blocks[b].end;
  if (end == 0 || fn.code[end - 1].k != K::Ret)
    return false;
  if (fn.code[end - 1].a != I.dst)
    return false;
  return ci + 1 < end; // the call precedes the ret
}

// ---- shared construction helpers -------------------------------------------
// append the recursion-budget preheader instrs; returns the bInit vreg
constexpr uint32_t tce_emit_budget_init(Fn &fn, std::vector<Instr> &out) {
  uint32_t dv = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Depth, .dst = dv});
  uint32_t kc = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Const, .dst = kc, .c = (val_t)MJZ_MAX_CALL_DEPTH + 1});
  uint32_t b0 = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Bin, .b = B::Sub, .dst = b0, .a = kc, .b2 = dv});
  return b0;
}
// append the per-site budget check + decrement into `out`; bL/eq are the
// (pre-created) destination vregs -- pre-creation matters because header
// phis reference bL BEFORE the site block is emitted
constexpr void tce_emit_site_check(Fn &fn, std::vector<Instr> &out, uint32_t bH,
                                   uint32_t bL, uint32_t eq) {
  uint32_t one = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Const, .dst = one, .c = 1});
  out.push_back({.k = K::Bin, .b = B::Sub, .dst = bL, .a = bH, .b2 = one});
  uint32_t cz = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Const, .dst = cz, .c = 0});
  out.push_back({.k = K::Bin, .b = B::Eq, .dst = eq, .a = bH, .b2 = cz});
}
// the synthesized trap block instrs: t = 1 / 0 (the existing idiom)
constexpr void tce_emit_trap(Fn &fn, std::vector<Instr> &out) {
  uint32_t c1 = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Const, .dst = c1, .c = 1});
  uint32_t cz = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Const, .dst = cz, .c = 0});
  uint32_t r = new_anon_vreg(fn, 0);
  out.push_back({.k = K::Bin, .b = B::Div, .dst = r, .a = c1, .b2 = cz});
  out.push_back({.k = K::Ret}); // unreachable after the trap; a terminator
}

// ---- multi member: merge a pure-tail SCC into one sel-dispatched function --
// Fm(sel, a0..a{M-1}): sel = the entry member's index (callers pass it;
// tailrec rewrites every external call site itself, so sel in [0,k) is an
// invariant recorded in Fn::tce_sel). The header dispatches on sel; each
// member's tail call to a sibling becomes [sel'=sibling; args; budget
// check; jump back through the latch chain]. Members' Ret instrs stay Ret
// (a tail caller returned the callee's value directly, so the merged fn
// returning it is exactly the original semantics).
struct TceSite {
  uint32_t m, b, ci;          // member, block (member-local id), call idx
  uint32_t callee;            // member index of the sibling callee
  std::vector<uint32_t> args; // call args (MEMBER vreg space)
};

// ---- the per-merge context --------------------------------------------------
// The id fields default to NONE exactly like the monolith's locals did
// (`uint32_t bInitV = NONE;`, the NONE-filled Mb/siteSEL/siteBL/siteEQ/Mp/
// Mbv vectors): zero-init vs NONE-init is observable through the `!= NONE`
// guards downstream -- the UsCtx lesson (unswitch, ckpt 34).
struct TmCtx {
  uint32_t k = 0;             // members in the SCC ring
  uint32_t M = 0;             // max member arity
  uint32_t ks = 0;            // tail sites (>= k >= 2)
  Fn Fm{};                    // the merged function under construction
  std::vector<TceSite> sites; // the intra-SCC tail sites
  std::vector<std::vector<uint32_t>> vmap; // member vreg -> Fm vreg
  std::vector<uint32_t> bmap_base;         // member -> first Fm block id
  std::vector<uint32_t> Mb;                // latch chain M_j (1-based)
  uint32_t T = NONE;                       // the trap block id
  uint32_t latch = NONE;                   // the header's latch pred
  uint32_t selHv = NONE;                   // H phi: sel
  std::vector<uint32_t> aHv;               // H phis: the a params
  uint32_t bHv = NONE;                     // H phi: the budget
  uint32_t bInitV = NONE;                  // P0's budget init value
  std::vector<uint32_t> siteSEL;           // per-site sel' Const dsts
  std::vector<uint32_t> siteBL;            // per-site budget-1 dsts
  std::vector<uint32_t> siteEQ;            // per-site budget==0 dsts
  std::vector<std::vector<uint32_t>> Mp;   // latch phis [sel, a0.., budget]
  std::vector<uint32_t> Mbv;               // latch phi: the budget
  std::vector<Instr> nc{};                 // Fm's code under construction
  std::vector<Block> nbl{};                // Fm's blocks under construction
  uint32_t FmIdx = NONE;                   // Fm's future fns[] index
};

// close block b (opened at `begin`) against the current nc tail
constexpr void tm_setblock(TmCtx &C, uint32_t b, uint32_t begin) {
  C.nbl[b].begin = begin;
  C.nbl[b].end = (uint32_t)C.nc.size();
}

// ---- phase 1: the gates + the intra-SCC tail-site census --------------------
constexpr bool tm_collect_sites(const std::vector<Fn> &fns,
                                const std::vector<uint32_t> &members,
                                TmCtx &C) {
  const uint32_t k = (uint32_t)members.size();
  if (k < 2 || k > MJZ_TCE_MAX_MEMBERS)
    return false;
  uint32_t M = 0;
  for (uint32_t mi : members)
    M = M > fns[mi].nparams ? M : fns[mi].nparams;
  if (M > MJZ_TCE_MAX_ARITY)
    return false;
  // collect the intra-SCC tail sites (args in member vreg space)
  std::vector<TceSite> sites;
  for (uint32_t x = 0; x < k; ++x) {
    const Fn &F = fns[members[x]];
    const uint32_t nvx = F.nvars + (uint32_t)F.version_base.size();
    std::vector<uint32_t> uses{};
    count_operand_uses(F, nvx, uses);
    for (uint32_t b = 0; b < F.nblocks(); ++b)
      for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
        if (F.code[i].k != K::Call)
          continue;
        uint32_t callee = (uint32_t)F.code[i].c;
        bool intra = false;
        for (uint32_t y : members)
          intra |= (y == callee);
        if (!intra)
          continue; // call out of the SCC: copied as a real call
        if (!is_tail_call(F, b, i, uses))
          return false; // mixed SCC: bail
        TceSite s;
        s.m = x;
        s.b = b;
        s.ci = i;
        s.callee = NONE;
        for (uint32_t y = 0; y < k; ++y)
          if (members[y] == callee)
            s.callee = y;
        s.args = F.code[i].args;
        sites.push_back(std::move(s));
      }
  }
  if (sites.size() < 2 || sites.size() > MJZ_TCE_MAX_SITES)
    return false;
  // (a k>=2-member cycle has >= k >= 2 tail sites: every member needs an
  // outgoing intra-SCC edge, so the latch chain always exists)
  C.k = k;
  C.M = M;
  C.ks = (uint32_t)sites.size();
  C.sites = std::move(sites);
  return true;
}

// ---- phase 2: Fm's shell, the block layout, the vreg pre-creation ----------
constexpr void tm_alloc(const std::vector<Fn> &fns,
                        const std::vector<uint32_t> &members, TmCtx &C) {
  const uint32_t k = C.k;
  const uint32_t M = C.M;
  const uint32_t ks = C.ks;
  Fn &Fm = C.Fm;
  Fm.nparams = 1 + M;
  Fm.nvars = 1 + M;
  Fm.var_names.push_back("sel");
  for (uint32_t j = 0; j < M; ++j)
    Fm.var_names.push_back("a");
  // per-member vreg map: param p -> the aH phi output; others -> fresh
  C.vmap.assign(k, std::vector<uint32_t>{});
  for (uint32_t x = 0; x < k; ++x) {
    const Fn &F = fns[members[x]];
    const uint32_t nvx = F.nvars + (uint32_t)F.version_base.size();
    C.vmap[x].assign(nvx, NONE);
  }
  // block layout: 0=P0, 1=H, 2..(1+k-2)=dispatch chain (k>2), then the
  // member bodies back to back, then the latch chain M_1..M_{ks-1}, then T
  C.bmap_base.assign(k, 0);
  uint32_t next_blk = 2 + (k > 2 ? k - 2 : 0);
  for (uint32_t x = 0; x < k; ++x) {
    C.bmap_base[x] = next_blk;
    next_blk += fns[members[x]].nblocks();
  }
  C.Mb.assign(ks + 1, NONE); // latch chain M_j (1-based)
  for (uint32_t j = 1; j < ks; ++j)
    C.Mb[j] = next_blk++;
  C.T = next_blk++;
  C.latch = C.Mb[1];
  // ---- pre-create vreg ids --------------------------------------------------
  C.selHv = new_anon_vreg(Fm, 0);
  C.aHv.assign(M, NONE);
  for (uint32_t j = 0; j < M; ++j)
    C.aHv[j] = new_anon_vreg(Fm, 0);
  C.bHv = new_anon_vreg(Fm, 0);
  // member vmaps (params AFTER the aH ids exist)
  for (uint32_t x = 0; x < k; ++x) {
    const Fn &F = fns[members[x]];
    const uint32_t nvx = F.nvars + (uint32_t)F.version_base.size();
    for (uint32_t v = 0; v < nvx; ++v)
      C.vmap[x][v] =
          (v < F.nparams) ? C.aHv[v] : new_anon_vreg(Fm, F.base_of(v));
  }
  // per-site emitted vregs (sel const, budget-1, eq) — in Fm space.
  // siteSEL is NOT pre-created: it is allocated at the member's Ret
  // emission (the landmine list above).
  C.siteSEL.assign(ks, NONE);
  C.siteBL.assign(ks, NONE);
  C.siteEQ.assign(ks, NONE);
  for (uint32_t j = 0; j < ks; ++j) {   // pre-create (the header phis and the
    C.siteBL[j] = new_anon_vreg(Fm, 0); // chain reference them before emission)
    C.siteEQ[j] = new_anon_vreg(Fm, 0);
  }
  // latch chain outputs: phis for [sel, a_0..a_{M-1}, budget]
  C.Mp.assign(ks + 1, std::vector<uint32_t>(1 + M, NONE));
  C.Mbv.assign(ks + 1, NONE);
  for (uint32_t j = 1; j < ks; ++j) {
    for (uint32_t q = 0; q <= M; ++q)
      C.Mp[j][q] = new_anon_vreg(Fm, 0);
    C.Mbv[j] = new_anon_vreg(Fm, 0);
  }
  // the code buffers (nbl sized to the layout's final block id)
  C.nbl.assign(C.T + 1, Block{});
}

// ---- phase 3: P0, H, and the sel dispatch chain -----------------------------
constexpr void tm_emit_header(TmCtx &C) {
  const uint32_t k = C.k;
  const uint32_t M = C.M;
  Fn &Fm = C.Fm;
  std::vector<Instr> &nc = C.nc;
  // P0
  C.bInitV = tce_emit_budget_init(Fm, nc);
  nc.push_back({.k = K::Br, .t = 1});
  tm_setblock(C, 0, 0);
  // H: phis + the dispatch terminator
  {
    uint32_t base = (uint32_t)nc.size();
    nc.push_back({.k = K::Phi,
                  .dst = C.selHv,
                  .a = 0,
                  .b2 = C.Mp[1][0],
                  .t = 0,
                  .f = C.latch});
    for (uint32_t j = 0; j < M; ++j)
      nc.push_back({.k = K::Phi,
                    .dst = C.aHv[j],
                    .a = 1 + j,
                    .b2 = C.Mp[1][1 + j],
                    .t = 0,
                    .f = C.latch});
    nc.push_back({.k = K::Phi,
                  .dst = C.bHv,
                  .a = C.bInitV,
                  .b2 = C.Mbv[1],
                  .t = 0,
                  .f = C.latch});
    // dispatch: sel == x -> member x's entry. k==2: right here.
    // k>2: a chain of dispatch blocks 2..k-1
    auto emit_dispatch = [&](uint32_t from_blk) {
      uint32_t eqv = new_anon_vreg(Fm, 0);
      uint32_t cv = new_anon_vreg(Fm, 0);
      nc.push_back({.k = K::Const, .dst = cv, .c = (val_t)from_blk});
      nc.push_back(
          {.k = K::Bin, .b = B::Eq, .dst = eqv, .a = C.selHv, .b2 = cv});
      uint32_t tt = C.bmap_base[from_blk];
      // BUGFIX (bug #2, mutual-recursion ring dispatch): the false arm
      // used to be (from_blk+1 <= k-2) ? (from_blk+2) : bmap_base[0],
      // which NEVER targets block 2: for k==3 it skipped the whole
      // dispatch chain (sel==2 fell into member 0) and for k>=4 it
      // jumped to block 3, skipping the sel==2 dispatch. The false arm
      // of the sel==(from_blk) check must go to the NEXT dispatch block,
      // which is the block that checks sel==(from_blk+1) -- block id
      // from_blk+1 in the D_i layout (block i checks sel==i) -- while
      // k==2 (no chain) still falls to member 0's entry.
      uint32_t ff = (from_blk + 1 <= k - 1) ? (from_blk + 1) : C.bmap_base[0];
      // D_i at block id i+1: compares sel == i+1; false -> D_{i+1} / E_0
      nc.push_back({.k = K::Cbr, .a = eqv, .t = tt, .f = ff});
    };
    if (k == 2) {
      emit_dispatch(1);
    } else {
      emit_dispatch(1); // H itself: sel==1 ? E_1 : D_1(block 2)
    }
    tm_setblock(C, 1, base);
  }
  // dispatch chain blocks 2..k-1 (k > 2): sel == i ? E_i : next
  for (uint32_t i = 2; i <= k - 1; ++i) {
    uint32_t base = (uint32_t)nc.size();
    uint32_t eqv = new_anon_vreg(Fm, 0);
    uint32_t cv = new_anon_vreg(Fm, 0);
    nc.push_back({.k = K::Const, .dst = cv, .c = (val_t)i});
    nc.push_back({.k = K::Bin, .b = B::Eq, .dst = eqv, .a = C.selHv, .b2 = cv});
    nc.push_back({.k = K::Cbr,
                  .a = eqv,
                  .t = C.bmap_base[i],
                  .f = (i < k - 1) ? (i + 1) : C.bmap_base[0]});
    tm_setblock(C, i, base);
  }
}

// ---- phase 4: the member bodies
// ----------------------------------------------
constexpr void tm_emit_members(const std::vector<Fn> &fns,
                               const std::vector<uint32_t> &members, TmCtx &C) {
  const uint32_t k = C.k;
  const uint32_t M = C.M;
  const uint32_t ks = C.ks;
  Fn &Fm = C.Fm;
  std::vector<Instr> &nc = C.nc;
  for (uint32_t x = 0; x < k; ++x) {
    const Fn &F = fns[members[x]];
    const uint32_t nvx = F.nvars + (uint32_t)F.version_base.size();
    auto mapv = [&](uint32_t v) -> uint32_t {
      if (v == NONE || v >= nvx)
        return v;
      return C.vmap[x][v] != NONE ? C.vmap[x][v] : v;
    };
    for (uint32_t b = 0; b < F.nblocks(); ++b) {
      uint32_t base = (uint32_t)nc.size();
      int sidx = -1;
      for (uint32_t j = 0; j < ks; ++j)
        if (C.sites[j].m == x && C.sites[j].b == b)
          sidx = (int)j;
      for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
        const Instr &I = F.code[i];
        if (sidx >= 0 && i == C.sites[(uint32_t)sidx].ci) {
          // the tail call dies; its CHAIN args are emitted at the ret below
          continue;
        }
        if (sidx >= 0 && I.k == K::Ret && i + 1 == F.blocks[b].end) {
          // sel' = Const(callee); pads; budget check; cbr T : chain-next
          TceSite &s = C.sites[(uint32_t)sidx];
          C.siteSEL[(uint32_t)sidx] = new_anon_vreg(Fm, 0);
          nc.push_back({.k = K::Const,
                        .dst = C.siteSEL[(uint32_t)sidx],
                        .c = (val_t)s.callee});
          uint32_t zp = NONE;
          for (uint32_t q = s.args.size(); q < M; ++q) { // pad unused args
            if (zp == NONE) {
              zp = new_anon_vreg(Fm, 0);
              nc.push_back({.k = K::Const, .dst = zp, .c = 0});
            }
            s.args.push_back(zp); // pad so chain phis can index uniformly
          }
          tce_emit_site_check(Fm, nc, C.bHv, C.siteBL[(uint32_t)sidx],
                              C.siteEQ[(uint32_t)sidx]);
          uint32_t next = ((uint32_t)sidx + 1 < ks) ? C.Mb[(uint32_t)sidx + 1]
                                                    : C.Mb[ks - 1];
          nc.push_back({.k = K::Cbr,
                        .a = C.siteEQ[(uint32_t)sidx],
                        .t = C.T,
                        .f = next});
          continue;
        }
        Instr c = I;
        remap_vreg_fields(c, mapv); // the shared permutation (instr.hpp;
        // mapv maps NONE to NONE, so this matches the old unguarded form)
        if (c.k == K::Br || c.k == K::Cbr || c.k == K::Phi) {
          if (c.t != NONE && c.t < F.nblocks())
            c.t = C.bmap_base[x] + c.t;
          if (c.f != NONE && c.f < F.nblocks())
            c.f = C.bmap_base[x] + c.f;
        }
        nc.push_back(c);
      }
      tm_setblock(C, C.bmap_base[x] + b, base);
    }
  }
}

// ---- phase 5: the latch chain, the trap, and Fm's commit --------------------
constexpr void tm_emit_latches(const std::vector<Fn> &fns,
                               const std::vector<uint32_t> &members, TmCtx &C) {
  const uint32_t ks = C.ks;
  const uint32_t M = C.M;
  Fn &Fm = C.Fm;
  std::vector<Instr> &nc = C.nc;
  // latch merge chain M_{ks-1}..M_1: phis for [sel, a_0.., budget]
  for (uint32_t j = ks - 1; j >= 1; --j) {
    uint32_t base = (uint32_t)nc.size();
    const TceSite &sa = C.sites[j - 1];
    uint32_t pa = C.bmap_base[sa.m] + sa.b;
    uint32_t pb = (j == ks - 1) ? (C.bmap_base[C.sites[j].m] + C.sites[j].b)
                                : C.Mb[j + 1];
    // arg list of site j-1 in Fm space: [sel'] + args (mapped)
    auto sitearg = [&](const TceSite &s, uint32_t q) -> uint32_t {
      const Fn &SF = fns[members[s.m]];
      const uint32_t nvx = SF.nvars + (uint32_t)SF.version_base.size();
      if (q == 0)
        return C.siteSEL[(uint32_t)(&s - &C.sites[0])];
      uint32_t v = s.args[q - 1];
      return (v != NONE && v < nvx) ? C.vmap[s.m][v] : v;
    };
    nc.push_back({.k = K::Phi,
                  .dst = C.Mp[j][0],
                  .a = sitearg(C.sites[j - 1], 0),
                  .b2 = (j == ks - 1 ? sitearg(C.sites[j], 0) : C.Mp[j + 1][0]),
                  .t = pa,
                  .f = pb});
    for (uint32_t q = 1; q <= M; ++q)
      nc.push_back(
          {.k = K::Phi,
           .dst = C.Mp[j][q],
           .a = sitearg(C.sites[j - 1], q),
           .b2 = (j == ks - 1 ? sitearg(C.sites[j], q) : C.Mp[j + 1][q]),
           .t = pa,
           .f = pb});
    nc.push_back({.k = K::Phi,
                  .dst = C.Mbv[j],
                  .a = C.siteBL[j - 1],
                  .b2 = (j == ks - 1 ? C.siteBL[j] : C.Mbv[j + 1]),
                  .t = pa,
                  .f = pb});
    nc.push_back({.k = K::Br, .t = (j == 1 ? 1 : C.Mb[j - 1])});
    tm_setblock(C, C.Mb[j], base);
  }
  // the trap block
  {
    uint32_t base = (uint32_t)nc.size();
    tce_emit_trap(Fm, nc);
    tm_setblock(C, C.T, base);
  }
  Fm.code = std::move(nc);
  Fm.blocks = std::move(C.nbl);
  Fm.resucc();      // succ wholesale from the fresh terminators + preds
  Fm.tce_sel = C.k; // sel in [0,k) at every call site: an invariant we made
}

// ---- phase 6: rewrite every external call site
// ------------------------------- member m -> Fm(sel=m, ...)
constexpr void tm_rewrite_callers(std::vector<Fn> &fns,
                                  const std::vector<uint32_t> &members,
                                  TmCtx &C) {
  const uint32_t k = C.k;
  const uint32_t M = C.M;
  C.FmIdx = (uint32_t)fns.size();
  const uint32_t FmIdx = C.FmIdx;
  for (uint32_t xf = 0; xf < fns.size(); ++xf) {
    bool is_member = false;
    for (uint32_t mi : members)
      is_member |= (mi == xf);
    if (is_member)
      continue;
    Fn &X = fns[xf];
    bool touched = false;
    for (uint32_t b = 0; b < X.nblocks(); ++b)
      for (uint32_t i = X.blocks[b].begin; i < X.blocks[b].end; ++i) {
        Instr &I = X.code[i];
        if (I.k != K::Call)
          continue;
        uint32_t callee = (uint32_t)I.c;
        int m = -1;
        for (uint32_t y = 0; y < k; ++y)
          if (members[y] == callee)
            m = (int)y;
        if (m < 0)
          continue;
        // insert sel const (+ pads) right before the call, then redirect
        std::vector<Instr> pre{};
        uint32_t sv = new_anon_vreg(X, 0);
        pre.push_back({.k = K::Const, .dst = sv, .c = (val_t)m});
        uint32_t zp = NONE;
        for (uint32_t q = I.args.size(); q < M; ++q) {
          if (zp == NONE) {
            zp = new_anon_vreg(X, 0);
            pre.push_back({.k = K::Const, .dst = zp, .c = 0});
          }
          I.args.push_back(zp);
        }
        I.args.insert(I.args.begin(), sv);
        I.c = (int64_t)FmIdx;
        // splice `pre` before the call instr
        X.code.insert(X.code.begin() + (long)i, pre.begin(), pre.end());
        const uint32_t add = (uint32_t)pre.size();
        for (uint32_t xb = 0; xb < X.nblocks(); ++xb) {
          if (xb == b) {
            X.blocks[xb].end += add;
            continue;
          }
          if (X.blocks[xb].begin > i)
            X.blocks[xb].begin += add;
          if (X.blocks[xb].end > i)
            X.blocks[xb].end += add;
        }
        i += add; // skip the inserted prefix; the call is next
        touched = true;
      }
    (void)touched;
  }
}
