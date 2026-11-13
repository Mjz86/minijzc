#pragma once
// === tailrec.hpp ========================================
// tailrec: tce_self (multi-site self tail-recursion -> one loop) and
// tce_merge (mutual-recursion rings -> sel-dispatched function).
//
// ALGORITHM: opt_tailrec builds the call graph and walks its SCCs
// (main is never a member; an intra-SCC edge is required). Every
// site passes is_tail_call: the Call's dst is used exactly once,
// by the block's terminating Ret, after the call. One member runs
// tce_self (self tail-rec -> one loop); 2..MJZ_TCE_MAX_MEMBERS
// members, all internal edges tail, run tce_merge (mixed bails).
//
// DATA STRUCTURES: tce_self -- Site{b,ci,args}, pHv (header phis,
// one per param), bHv (budget phi), siteBL/siteEQ (bH-1 / bH==0),
// Mb/Mp/Mbv (the 2-pred latch chain), T (trap). tce_merge --
// TceSite{m,b,ci,callee,args}, vmap, bmap_base, Fn::tce_sel (the
// sel-in-[0,k) invariant at the rewritten external callers).
//
// THEORY: a tail call needs no accumulator -- its dst flows straight
// to the caller's return slot, so [Call;Ret] is exactly [args;
// jump]. The callee being the function itself, the jump targets the
// entry block, now the loop header: each param becomes a phi (pHv)
// fed by the original args on the entry edge and the site args on
// the latch -- recursion becomes a loop in place. The 512-frame cap
// is reproduced bit-for-bit: the preheader sets budget = CAP + 1 -
// Depth(), each site traps at bH==0 (the t = 1/0 idiom) else jumps
// carrying bH-1, exactly where the original chain would have. A
// pure-tail SCC is already a goto graph -- every internal edge is
// `return sibling(args)` -- so tce_merge folds it into ONE leaf
// Fm(sel, a0..a{M-1}): the header dispatches on sel, each sibling
// call becomes [sel'=callee; args; budget check; jump], Ret stays.
//
// DOWNSTREAM: the calls gone, unroll and cdfold see the recursion
// as the loop it always was; cdfold rewrites it into closed forms.

#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../analysis/an.hpp"
#include "callgraph.hpp"

// ============================================================================
// 9e4. TAIL-CALL ELIMINATION for inner-SCC tail calls
// ============================================================================
// A tail call is a Call whose result flows DIRECTLY into the enclosing Ret
// (the caller returns the callee's value unchanged): `return f(x);`. Inside
// a call-graph SCC (recursion), such a call needs no return-address: it is
// a GUMP back to the (callee's) entry with fresh arguments. Two shapes:
//
//   * single-member SCC (self tail recursion): the function becomes a LOOP
//     in place — a new entry preheader computes the EXACT remaining
//     recursion budget (MJZ_MAX_CALL_DEPTH + 1 - Depth(): the number of
//     further activations the original chain could still push before the
//     512-frame TRAP), the entry block becomes the loop header with phis
//     for the parameters, and every tail site becomes [budget check ->
//     synthesized trap; args already computed; jump back]. Binary-search
//     style functions (several tail sites) funnel through a binary merge
//     chain of latch blocks, so every phi keeps exactly 2 args.
//
//   * multi-member SCC (mutual tail recursion, e.g. is_even/is_odd): all
//     members merge into ONE function `Fm(sel, a0..a{M-1})` — sel selects
//     the entry member (callers pass the member index; tailrec rewrites
//     every call site itself, so sel in [0,k) is an invariant it records
//     in Fn::tce_sel), the header dispatches on it, and each member's tail
//     call to a sibling becomes [sel' = sibling; args; budget check; jump].
//     The merged function is a leaf w.r.t. the SCC: no recursion remains.
//
// SEMANTIC EXACTNESS: the original chain pushed one activation per tail
// call and TRAPs when the 512-activation cap would be exceeded; the budget
// counter reproduces this bit-for-bit (budget = CAP+1 - Depth() at entry,
// one decrement per tail jump, trap when a jump is needed at budget 0 —
// including the "negative n never reaches the base case" case: the budget
// runs out exactly where the original recursion cap would). The trap is
// synthesized with the existing idiom `t = 1 / 0` (div-by-constant-zero),
// which every backend already emits as `printf("TRAP\n"); exit(1);` and
// which the optimizer already treats as an observable, unfoldable, may-trap
// root. After the transformation the loop passes (unroll, cdfold) can see
// the recursion as the loop it always was.
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

// ---- single member: self tail-recursion -> loop in place --------------------
constexpr bool tce_self(Fn &fn, OptStats &os, uint32_t self_idx) {
  const uint32_t nb = fn.nblocks();
  const uint32_t P = fn.nparams;
  const uint32_t nv = fn.nvars + (uint32_t)fn.version_base.size();
  if (!nb || !nv)
    return false;
  // collect tail sites (self calls in tail position)
  struct Site {
    uint32_t b, ci;             // block + call instr index
    std::vector<uint32_t> args; // the call's argument vregs
  };
  std::vector<uint32_t> uses(nv, 0);
  for (const Instr &I : fn.code) {
    auto bump = [&](uint32_t v) {
      if (v != NONE && v < nv)
        ++uses[v];
    };
    bump(I.a);
    bump(I.b2);
    bump(I.c2);
    for (uint32_t v : I.args)
      bump(v);
  }
  std::vector<Site> sites;
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      if (fn.code[i].k != K::Call)
        continue;
      if ((uint32_t)fn.code[i].c != self_idx)
        continue; // not a self call
      if (fn.code[i].args.size() != P)
        return false;
      if (!is_tail_call(fn, b, i, uses))
        return false;
      Site s;
      s.b = b;
      s.ci = i;
      s.args = fn.code[i].args;
      sites.push_back(std::move(s));
    }
  if (sites.empty() || sites.size() > MJZ_TCE_MAX_SITES)
    return false;
  const uint32_t k = (uint32_t)sites.size();
  // ---- new block ids -------------------------------------------------------
  //   0        = P0 (the new entry preheader)
  //   ob+1     = every original block (H, the loop header, = old block 0)
  //   nb+1..   = the latch merge chain M_1..M_{k-1} (only when k > 1)
  //   T        = the synthesized trap block
  // site j (0-based) false-arm target: k==1 -> H; j+1<k -> M_{j+1}; else
  // M_{k-1}.  M_j merges (site j, M_{j+1} or site k-1) and jumps to
  // M_{j-1} / H, so the header keeps exactly ONE latch pred.
  const uint32_t H = 1;
  std::vector<uint32_t> Mb(k + 1, NONE); // M_j block id, 1-based
  for (uint32_t j = 1; j < k; ++j)
    Mb[j] = nb + j;
  const uint32_t T = (k > 1) ? nb + k : nb + 1;
  const uint32_t latch = (k == 1) ? (sites[0].b + 1) : Mb[1];
  // ---- pre-create every fresh vreg id (blocks reference each other) --------
  std::vector<uint32_t> pHv(P, NONE); // header phis for the params
  for (uint32_t p = 0; p < P; ++p)
    pHv[p] = new_anon_vreg(fn, fn.base_of(p));
  const uint32_t bHv = new_anon_vreg(fn, 0); // header phi: the budget
  std::vector<uint32_t> siteBL(k, NONE), siteEQ(k, NONE);
  for (uint32_t j = 0; j < k; ++j) {  // PRE-create: the H phis reference
    siteBL[j] = new_anon_vreg(fn, 0); // siteBL[0] before the site is emitted
    siteEQ[j] = new_anon_vreg(fn, 0);
  }
  std::vector<std::vector<uint32_t>> Mp(k + 1, std::vector<uint32_t>(P, NONE));
  std::vector<uint32_t> Mbv(k + 1, NONE);
  for (uint32_t j = 1; j < k; ++j) {
    for (uint32_t p = 0; p < P; ++p)
      Mp[j][p] = new_anon_vreg(fn, fn.base_of(p));
    Mbv[j] = new_anon_vreg(fn, 0);
  }
  // ---- build the new code --------------------------------------------------
  std::vector<Instr> nc{};
  nc.reserve(fn.code.size() + 12 * k + 4 * P + 16);
  std::vector<Block> nbl{};
  nbl.reserve(nb + k + 1);
  // P0: budget = CAP+1 - Depth(); br H
  uint32_t bInitV = NONE;
  {
    uint32_t base = (uint32_t)nc.size();
    bInitV = tce_emit_budget_init(fn, nc);
    nc.push_back({.k = K::Br, .t = H});
    nbl.push_back({base, (uint32_t)nc.size()});
  }
  // param-use rewrite: any use of param vreg p becomes the header phi pHv[p]
  auto rw = [&](uint32_t v) -> uint32_t {
    if (v != NONE && v < P)
      return pHv[v];
    return v;
  };
  // original blocks (ids shifted +1)
  for (uint32_t ob = 0; ob < nb; ++ob) {
    uint32_t base = (uint32_t)nc.size();
    if (ob == 0) { // the header: its phis come first
      // NOTE: a raw-parameter argument (gcd(b, a % b) passes the param b
      // unchanged) must enter the latch phi as the param's HEADER phi
      // pHv[p], never the original param vreg — that one still holds the
      // ENTRY argument forever, and the loop would feed it back on every
      // iteration (the same rotation scctco needed; found by the merged
      // fuzzer's rgcd vocabulary at --disable=scctco)
      for (uint32_t p = 0; p < P; ++p)
        nc.push_back({.k = K::Phi,
                      .dst = pHv[p],
                      .a = p,
                      .b2 = (k == 1 ? rw(sites[0].args[p]) : Mp[1][p]),
                      .t = 0,
                      .f = latch});
      nc.push_back({.k = K::Phi,
                    .dst = bHv,
                    .a = bInitV,
                    .b2 = (k == 1 ? siteBL[0] : Mbv[1]),
                    .t = 0,
                    .f = latch});
    }
    int sidx = -1; // which site lives in this block (>= 0: the tail site)
    for (uint32_t j = 0; j < k; ++j)
      if (sites[j].b == ob)
        sidx = (int)j;
    for (uint32_t i = fn.blocks[ob].begin; i < fn.blocks[ob].end; ++i) {
      const Instr &I = fn.code[i];
      if (sidx >= 0 && i == sites[(uint32_t)sidx].ci)
        continue; // the tail call dies
      if (sidx >= 0 && I.k == K::Ret && i + 1 == fn.blocks[ob].end) {
        // the tail ret becomes: [budget-1][bH==0?] -> trap or the chain
        tce_emit_site_check(fn, nc, bHv, siteBL[(uint32_t)sidx],
                            siteEQ[(uint32_t)sidx]);
        (void)0;
        uint32_t next =
            (k == 1)
                ? H
                : ((uint32_t)sidx + 1 < k ? Mb[(uint32_t)sidx + 1] : Mb[k - 1]);
        nc.push_back(
            {.k = K::Cbr, .a = siteEQ[(uint32_t)sidx], .t = T, .f = next});
        continue;
      }
      Instr c = I;
      c.a = rw(c.a);
      c.b2 = rw(c.b2);
      if (c.c2 != NONE)
        c.c2 = rw(c.c2);
      for (uint32_t &av : c.args)
        av = rw(av);
      if (c.dst != NONE && c.dst < P)
        c.dst = pHv[c.dst];
      if (c.k == K::Br || c.k == K::Cbr || c.k == K::Phi) {
        if (c.t != NONE && c.t < nb)
          c.t = c.t + 1;
        if (c.f != NONE && c.f < nb)
          c.f = c.f + 1;
      }
      nc.push_back(c);
    }
    nbl.push_back({base, (uint32_t)nc.size()});
  }
  // latch merge chain M_1 .. M_{k-1} (M_j: phis + br M_{j-1} / H).
  // BUGFIX (bug #1, multi-site self tail-rec): the chain used to be
  // appended in DESCENDING order (M_{k-1} first) while every block id
  // reference uses Mb[j] = nb + j, which assumes ASCENDING placement.
  // With k >= 3 tail sites every M-block id was wrong: the CFG came out
  // scrambled (an infinite 2-cycle that lost the argument update and the
  // budget check) -- e.g. a 4-site self-recursive fn looped forever.
  // Fix: emit M_1 first (j ascending) so physical position nb+j holds
  // M_j exactly as Mb[j] = nb + j promises.
  for (uint32_t j = 1; j <= k - 1; ++j) {
    uint32_t base = (uint32_t)nc.size();
    uint32_t pa = sites[j - 1].b + 1; // site j (1-based) edge
    uint32_t pb = (j == k - 1) ? (sites[j].b + 1) : Mb[j + 1];
    for (uint32_t p = 0; p < P; ++p)
      nc.push_back({.k = K::Phi,
                    .dst = Mp[j][p],
                    .a = rw(sites[j - 1].args[p]),
                    .b2 = (j == k - 1 ? rw(sites[j].args[p]) : Mp[j + 1][p]),
                    .t = pa,
                    .f = pb});
    nc.push_back({.k = K::Phi,
                  .dst = Mbv[j],
                  .a = siteBL[j - 1],
                  .b2 = (j == k - 1 ? siteBL[j] : Mbv[j + 1]),
                  .t = pa,
                  .f = pb});
    nc.push_back({.k = K::Br, .t = (j == 1 ? H : Mb[j - 1])});
    nbl.push_back({base, (uint32_t)nc.size()});
  }
  // the trap block
  {
    uint32_t base = (uint32_t)nc.size();
    tce_emit_trap(fn, nc);
    nbl.push_back({base, (uint32_t)nc.size()});
  }
  // ---- commit
  // ----------------------------------------------------------------
  fn.code = std::move(nc);
  fn.blocks = std::move(nbl);
  fn.succ.assign(2 * fn.nblocks(), NONE);
  for (uint32_t b = 0; b < fn.nblocks(); ++b) {
    if (fn.blocks[b].begin >= fn.blocks[b].end)
      continue; // empty/tombstone: reading code[end-1] would steal the
                // PREVIOUS block's terminator and forge ghost edges
    const Instr &term = fn.code[fn.blocks[b].end - 1];
    if (term.k == K::Br)
      fn.succ[2 * b] = term.t;
    else if (term.k == K::Cbr) {
      fn.succ[2 * b] = term.t;
      fn.succ[2 * b + 1] = term.f;
    }
  }
  fn.recompute_preds();
  os.tces++;
  return true;
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
constexpr bool tce_merge(std::vector<Fn> &fns,
                         std::vector<std::string_view> &names,
                         std::vector<OptStats> &ostats,
                         std::vector<uint32_t> &src_index,
                         const std::vector<uint32_t> &members) {
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
    std::vector<uint32_t> uses(nvx, 0);
    for (const Instr &I : F.code) {
      auto bump = [&](uint32_t v) {
        if (v != NONE && v < nvx)
          ++uses[v];
      };
      bump(I.a);
      bump(I.b2);
      bump(I.c2);
      for (uint32_t v : I.args)
        bump(v);
    }
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
  const uint32_t ks = (uint32_t)sites.size();
  // (a k>=2-member cycle has >= k >= 2 tail sites: every member needs an
  // outgoing intra-SCC edge, so the latch chain always exists)
  // ---- build Fm -------------------------------------------------------------
  Fn Fm{};
  Fm.nparams = 1 + M;
  Fm.nvars = 1 + M;
  Fm.var_names.push_back("sel");
  for (uint32_t j = 0; j < M; ++j)
    Fm.var_names.push_back("a");
  // per-member vreg map: param p -> the aH phi output; others -> fresh
  std::vector<std::vector<uint32_t>> vmap(k);
  for (uint32_t x = 0; x < k; ++x) {
    const Fn &F = fns[members[x]];
    const uint32_t nvx = F.nvars + (uint32_t)F.version_base.size();
    vmap[x].assign(nvx, NONE);
  }
  // block layout: 0=P0, 1=H, 2..(1+k-2)=dispatch chain (k>2), then the
  // member bodies back to back, then the latch chain M_1..M_{ks-1}, then T
  std::vector<uint32_t> bmap_base(k);
  uint32_t next_blk = 2 + (k > 2 ? k - 2 : 0);
  for (uint32_t x = 0; x < k; ++x) {
    bmap_base[x] = next_blk;
    next_blk += fns[members[x]].nblocks();
  }
  std::vector<uint32_t> Mb(ks + 1, NONE); // latch chain M_j (1-based)
  for (uint32_t j = 1; j < ks; ++j)
    Mb[j] = next_blk++;
  const uint32_t T = next_blk++;
  const uint32_t latch = Mb[1];
  // ---- pre-create vreg ids --------------------------------------------------
  const uint32_t selHv = new_anon_vreg(Fm, 0);
  std::vector<uint32_t> aHv(M, NONE);
  for (uint32_t j = 0; j < M; ++j)
    aHv[j] = new_anon_vreg(Fm, 0);
  const uint32_t bHv = new_anon_vreg(Fm, 0);
  // member vmaps (params AFTER the aH ids exist)
  for (uint32_t x = 0; x < k; ++x) {
    const Fn &F = fns[members[x]];
    const uint32_t nvx = F.nvars + (uint32_t)F.version_base.size();
    for (uint32_t v = 0; v < nvx; ++v)
      vmap[x][v] = (v < F.nparams) ? aHv[v] : new_anon_vreg(Fm, F.base_of(v));
  }
  // per-site emitted vregs (sel const, budget-1, eq) — in Fm space
  std::vector<uint32_t> siteSEL(ks, NONE), siteBL(ks, NONE), siteEQ(ks, NONE);
  for (uint32_t j = 0; j < ks; ++j) { // pre-create (the header phis and the
    siteBL[j] = new_anon_vreg(Fm, 0); // chain reference them before emission)
    siteEQ[j] = new_anon_vreg(Fm, 0);
  }
  // latch chain outputs: phis for [sel, a_0..a_{M-1}, budget]
  std::vector<std::vector<uint32_t>> Mp(ks + 1,
                                        std::vector<uint32_t>(1 + M, NONE));
  std::vector<uint32_t> Mbv(ks + 1, NONE);
  for (uint32_t j = 1; j < ks; ++j) {
    for (uint32_t q = 0; q <= M; ++q)
      Mp[j][q] = new_anon_vreg(Fm, 0);
    Mbv[j] = new_anon_vreg(Fm, 0);
  }
  // ---- build the code -------------------------------------------------------
  std::vector<Instr> nc{};
  std::vector<Block> nbl(T + 1);
  auto setblock = [&](uint32_t b, uint32_t begin) {
    nbl[b].begin = begin;
    nbl[b].end = (uint32_t)nc.size();
  };
  // P0
  uint32_t bInitV = NONE;
  {
    bInitV = tce_emit_budget_init(Fm, nc);
    nc.push_back({.k = K::Br, .t = 1});
    setblock(0, 0);
  }
  // H: phis + the dispatch terminator
  {
    uint32_t base = (uint32_t)nc.size();
    nc.push_back({.k = K::Phi,
                  .dst = selHv,
                  .a = 0,
                  .b2 = Mp[1][0],
                  .t = 0,
                  .f = latch});
    for (uint32_t j = 0; j < M; ++j)
      nc.push_back({.k = K::Phi,
                    .dst = aHv[j],
                    .a = 1 + j,
                    .b2 = Mp[1][1 + j],
                    .t = 0,
                    .f = latch});
    nc.push_back({.k = K::Phi,
                  .dst = bHv,
                  .a = bInitV,
                  .b2 = Mbv[1],
                  .t = 0,
                  .f = latch});
    // dispatch: sel == x -> member x's entry. k==2: right here.
    // k>2: a chain of dispatch blocks 2..k-1
    auto emit_dispatch = [&](uint32_t from_blk) {
      uint32_t eqv = new_anon_vreg(Fm, 0);
      uint32_t cv = new_anon_vreg(Fm, 0);
      nc.push_back({.k = K::Const, .dst = cv, .c = (val_t)from_blk});
      nc.push_back({.k = K::Bin, .b = B::Eq, .dst = eqv, .a = selHv, .b2 = cv});
      uint32_t tt = bmap_base[from_blk];
      // BUGFIX (bug #2, mutual-recursion ring dispatch): the false arm
      // used to be (from_blk+1 <= k-2) ? (from_blk+2) : bmap_base[0],
      // which NEVER targets block 2: for k==3 it skipped the whole
      // dispatch chain (sel==2 fell into member 0) and for k>=4 it
      // jumped to block 3, skipping the sel==2 dispatch. The false arm
      // of the sel==(from_blk) check must go to the NEXT dispatch block,
      // which is the block that checks sel==(from_blk+1) -- block id
      // from_blk+1 in the D_i layout (block i checks sel==i) -- while
      // k==2 (no chain) still falls to member 0's entry.
      uint32_t ff = (from_blk + 1 <= k - 1) ? (from_blk + 1) : bmap_base[0];
      // D_i at block id i+1: compares sel == i+1; false -> D_{i+1} / E_0
      nc.push_back({.k = K::Cbr, .a = eqv, .t = tt, .f = ff});
    };
    if (k == 2) {
      emit_dispatch(1);
    } else {
      emit_dispatch(1); // H itself: sel==1 ? E_1 : D_1(block 2)
    }
    setblock(1, base);
  }
  // dispatch chain blocks 2..k-1 (k > 2): sel == i ? E_i : next
  for (uint32_t i = 2; i <= k - 1; ++i) {
    uint32_t base = (uint32_t)nc.size();
    uint32_t eqv = new_anon_vreg(Fm, 0);
    uint32_t cv = new_anon_vreg(Fm, 0);
    nc.push_back({.k = K::Const, .dst = cv, .c = (val_t)i});
    nc.push_back({.k = K::Bin, .b = B::Eq, .dst = eqv, .a = selHv, .b2 = cv});
    nc.push_back({.k = K::Cbr,
                  .a = eqv,
                  .t = bmap_base[i],
                  .f = (i < k - 1) ? (i + 1) : bmap_base[0]});
    setblock(i, base);
  }
  // member bodies
  for (uint32_t x = 0; x < k; ++x) {
    const Fn &F = fns[members[x]];
    const uint32_t nvx = F.nvars + (uint32_t)F.version_base.size();
    auto mapv = [&](uint32_t v) -> uint32_t {
      if (v == NONE || v >= nvx)
        return v;
      return vmap[x][v] != NONE ? vmap[x][v] : v;
    };
    for (uint32_t b = 0; b < F.nblocks(); ++b) {
      uint32_t base = (uint32_t)nc.size();
      int sidx = -1;
      for (uint32_t j = 0; j < ks; ++j)
        if (sites[j].m == x && sites[j].b == b)
          sidx = (int)j;
      for (uint32_t i = F.blocks[b].begin; i < F.blocks[b].end; ++i) {
        const Instr &I = F.code[i];
        if (sidx >= 0 && i == sites[(uint32_t)sidx].ci) {
          // the tail call dies; its CHAIN args are emitted at the ret below
          continue;
        }
        if (sidx >= 0 && I.k == K::Ret && i + 1 == F.blocks[b].end) {
          // sel' = Const(callee); pads; budget check; cbr T : chain-next
          TceSite &s = sites[(uint32_t)sidx];
          siteSEL[(uint32_t)sidx] = new_anon_vreg(Fm, 0);
          nc.push_back({.k = K::Const,
                        .dst = siteSEL[(uint32_t)sidx],
                        .c = (val_t)s.callee});
          uint32_t zp = NONE;
          for (uint32_t q = s.args.size(); q < M; ++q) { // pad unused args
            if (zp == NONE) {
              zp = new_anon_vreg(Fm, 0);
              nc.push_back({.k = K::Const, .dst = zp, .c = 0});
            }
            s.args.push_back(zp); // pad so chain phis can index uniformly
          }
          tce_emit_site_check(Fm, nc, bHv, siteBL[(uint32_t)sidx],
                              siteEQ[(uint32_t)sidx]);
          uint32_t next =
              ((uint32_t)sidx + 1 < ks) ? Mb[(uint32_t)sidx + 1] : Mb[ks - 1];
          nc.push_back(
              {.k = K::Cbr, .a = siteEQ[(uint32_t)sidx], .t = T, .f = next});
          continue;
        }
        Instr c = I;
        if (c.dst != NONE)
          c.dst = mapv(c.dst);
        c.a = mapv(c.a);
        c.b2 = mapv(c.b2);
        if (c.c2 != NONE)
          c.c2 = mapv(c.c2);
        for (uint32_t &av : c.args)
          av = mapv(av);
        if (c.k == K::Br || c.k == K::Cbr || c.k == K::Phi) {
          if (c.t != NONE && c.t < F.nblocks())
            c.t = bmap_base[x] + c.t;
          if (c.f != NONE && c.f < F.nblocks())
            c.f = bmap_base[x] + c.f;
        }
        nc.push_back(c);
      }
      setblock(bmap_base[x] + b, base);
    }
  }
  // latch merge chain M_{ks-1}..M_1: phis for [sel, a_0.., budget]
  for (uint32_t j = ks - 1; j >= 1; --j) {
    uint32_t base = (uint32_t)nc.size();
    const TceSite &sa = sites[j - 1];
    uint32_t pa = bmap_base[sa.m] + sa.b;
    uint32_t pb =
        (j == ks - 1) ? (bmap_base[sites[j].m] + sites[j].b) : Mb[j + 1];
    // arg list of site j-1 in Fm space: [sel'] + args (mapped)
    auto sitearg = [&](const TceSite &s, uint32_t q) -> uint32_t {
      const Fn &SF = fns[members[s.m]];
      const uint32_t nvx = SF.nvars + (uint32_t)SF.version_base.size();
      if (q == 0)
        return siteSEL[(uint32_t)(&s - &sites[0])];
      uint32_t v = s.args[q - 1];
      return (v != NONE && v < nvx) ? vmap[s.m][v] : v;
    };
    nc.push_back({.k = K::Phi,
                  .dst = Mp[j][0],
                  .a = sitearg(sites[j - 1], 0),
                  .b2 = (j == ks - 1 ? sitearg(sites[j], 0) : Mp[j + 1][0]),
                  .t = pa,
                  .f = pb});
    for (uint32_t q = 1; q <= M; ++q)
      nc.push_back({.k = K::Phi,
                    .dst = Mp[j][q],
                    .a = sitearg(sites[j - 1], q),
                    .b2 = (j == ks - 1 ? sitearg(sites[j], q) : Mp[j + 1][q]),
                    .t = pa,
                    .f = pb});
    nc.push_back({.k = K::Phi,
                  .dst = Mbv[j],
                  .a = siteBL[j - 1],
                  .b2 = (j == ks - 1 ? siteBL[j] : Mbv[j + 1]),
                  .t = pa,
                  .f = pb});
    nc.push_back({.k = K::Br, .t = (j == 1 ? 1 : Mb[j - 1])});
    setblock(Mb[j], base);
  }
  // the trap block
  {
    uint32_t base = (uint32_t)nc.size();
    tce_emit_trap(Fm, nc);
    setblock(T, base);
  }
  Fm.code = std::move(nc);
  Fm.blocks = std::move(nbl);
  Fm.succ.assign(2 * Fm.nblocks(), NONE);
  for (uint32_t b = 0; b < Fm.nblocks(); ++b) {
    if (Fm.blocks[b].begin >= Fm.blocks[b].end)
      continue; // empty/tombstone: never read code[end-1] of an empty range
    const Instr &term = Fm.code[Fm.blocks[b].end - 1];
    if (term.k == K::Br)
      Fm.succ[2 * b] = term.t;
    else if (term.k == K::Cbr) {
      Fm.succ[2 * b] = term.t;
      Fm.succ[2 * b + 1] = term.f;
    }
  }
  Fm.recompute_preds();
  Fm.tce_sel = k; // sel in [0,k) at every call site: an invariant we made
  // ---- rewrite every external call site: member m -> Fm(sel=m, ...) ------
  const uint32_t FmIdx = (uint32_t)fns.size();
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
  // ---- append Fm; bookkeeping
  // ------------------------------------------------
  fns.push_back(std::move(Fm));
  names.push_back("tce_merged");
  ostats.push_back(OptStats{});
  // count on main too: the merged fn is usually inlined and then removed by
  // dead-fn elimination, which would take its (interesting) stats with it
  ostats[FmIdx].tces++;
  ostats[FmIdx].tce_merges++;
  if (!ostats.empty()) {
    ostats[0].tces++;
    ostats[0].tce_merges++;
  }
  src_index.push_back(src_index[members[0]]); // display placeholder
  return true;
}

// ---- the pass driver
// ----------------------------------------------------------
constexpr bool opt_tailrec(std::vector<Fn> &fns,
                           std::vector<std::string_view> &names,
                           std::vector<OptStats> &ostats,
                           std::vector<uint32_t> &src_index) {
  if (fns.size() < 2)
    return false;
  bool changed = false;
  CallGraph G = build_callgraph(fns);
  for (uint32_t s = 0; s < G.nscc; ++s) {
    uint32_t e =
        (s + 1 < G.nscc) ? G.scc_begin[s + 1] : (uint32_t)G.members.size();
    std::vector<uint32_t> members(G.members.begin() + G.scc_begin[s],
                                  G.members.begin() + e);
    if (members.empty())
      continue;
    bool has_main = false;
    for (uint32_t m : members)
      has_main |= (m == 0);
    if (has_main)
      continue; // main is never a callee
    // must actually recurse: an intra-SCC edge
    bool intra = false;
    for (uint32_t m : members)
      for (const Instr &I : fns[m].code)
        if (I.k == K::Call) {
          uint32_t c = (uint32_t)I.c;
          for (uint32_t m2 : members)
            intra |= (m2 == c);
        }
    if (!intra)
      continue;
    if (members.size() == 1) {
      if (fns[members[0]].code.size() > MJZ_TCE_MAX_FNSIZE)
        continue;
      if (tce_self(fns[members[0]], ostats[members[0]], members[0]))
        changed = true;
      continue;
    }
    bool big = false;
    for (uint32_t m : members)
      big |= fns[m].code.size() > MJZ_TCE_MAX_FNSIZE;
    if (big)
      continue;
    if (tce_merge(fns, names, ostats, src_index, members))
      changed = true;
  }
  return changed;
}
