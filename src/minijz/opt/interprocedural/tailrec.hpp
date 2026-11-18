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
// TceSite{m,b,ci,callee,args}, TmCtx (the per-merge context: vmap,
// bmap_base, the pre-created site/latch vregs) in tce_merge_impl.hpp
// (the support layer, the code-quality continuation round eleven);
// Fn::tce_sel (the sel-in-[0,k) invariant at the rewritten callers).
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
#include "tce_merge_impl.hpp"

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
// (the shared tce vocabulary -- the caps, is_tail_call, the budget /
// site-check / trap emitters -- lives in tce_merge_impl.hpp, included
// above; tce_self and the tce_merge machinery both speak it)

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
  std::vector<uint32_t> uses{};
  count_operand_uses(fn, nv, uses);
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
  fn.resucc(); // succ wholesale from the fresh terminators + preds
  os.tces++;
  return true;
}

// ---- multi member: merge a pure-tail SCC into one sel-dispatched function --
// the slim driver -- the phase list (the machinery, the per-merge context
// TmCtx, and the landmine ledger live in tce_merge_impl.hpp; every phase
// runs in exactly the monolith's order: the vreg allocation order is
// observable through the audit table):
//   collect sites -> alloc Fm's shell/layout/vregs -> emit P0+H+dispatch ->
//   emit the member bodies -> emit the latch chain + trap + commit Fm ->
//   rewrite every external caller; then the bookkeeping below.
constexpr bool tce_merge(std::vector<Fn> &fns,
                         std::vector<std::string_view> &names,
                         std::vector<OptStats> &ostats,
                         std::vector<uint32_t> &src_index,
                         const std::vector<uint32_t> &members) {
  TmCtx C{};
  if (!tm_collect_sites(fns, members, C))
    return false;
  tm_alloc(fns, members, C);
  tm_emit_header(C);
  tm_emit_members(fns, members, C);
  tm_emit_latches(fns, members, C);
  tm_rewrite_callers(fns, members, C);
  // ---- append Fm; bookkeeping
  // ------------------------------------------------
  const uint32_t FmIdx = C.FmIdx;
  fns.push_back(std::move(C.Fm));
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
