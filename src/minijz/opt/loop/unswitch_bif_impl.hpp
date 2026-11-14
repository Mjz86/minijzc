#pragma once
// === unswitch_bif_impl.hpp ========================================
// the BODY-IF UNSWITCH SUPPORT layer, split out of unswitch_bif.hpp
// (the code-quality continuation, round five -- the cdfold/lcf/
// unroll/fold treatment): the pass context (the scan tables + the
// commit state the former monolith held in locals), the gates (the
// dedicated-preheader check, the growth cap), the outside-use scan
// (loopdef/arm_edge classification, reverse reachability, the
// vm_vals/reads collection), the routes validator, and the commit
// tail (the clone, the two version branch rewrites, the
// version-merge blocks, the exit retargets, the guard, the
// delivered walk, the rewire sweep). The slim driver -- the phase
// list -- stays in unswitch_bif.hpp.
//
// ALGORITHM (the phase list; every order below is the original's):
//   gates    dedicated `br h' preheader (bif_preheader_ok: Br to h,
//            succ0 == h, succ1 == NONE); growth cap: 1..120 in-loop
//            instrs (bif_growth_ok); exit_inventory with primary
//            NONE -- the header's exit arm is a REGULAR exit, both
//            versions keep running the loop; exits must be non-empty.
//   classify bif_mark_loopdefs: loopdef[v] = defined in the loop
//            (S.defi/defb, the SSA one-def scan). bif_mark_arms:
//            arm_edge[p] = the in-loop pred of a PURE ARM (an
//            outside block whose only pred is in-loop and not pre).
//   scan     per outside block (inL/pre skipped), per instr:
//            a phi arg keyed to an IN-LOOP pred is an exit edge --
//            the VM entering this block must merge it (vm_vals[b]);
//            a phi arg keyed to an OUTSIDE pred and every plain
//            operand of a loop-defined vreg are READS -- collected
//            over reverse reachability (bif_rr: every exit target t
//            that reaches the read through outside blocks) into
//            vm_vals[t] + reads. A read no exit reaches sets bad
//            (dominance says it cannot execute anyway: bail).
//   routes   every read is validated BEFORE the commit's first
//            edit: the merged value must be providable at the
//            read's block entry -- pure arm -> the VM below carries
//            v; single-pred chain -> the pred routes; merge -> the
//            in-loop preds' VM carries v, outside preds route
//            further. A cycle or a 3+-pred merge is not routable.
//            A bail anywhere above leaves the fn untouched.
//   commit   the clone (alloc_clone + emit_loop_clone, bypass NONE:
//            the while-test stays live in BOTH versions); the
//            version rewrites (bif: br A -- INV true; bif': br B' --
//            INV false); one VM block per exit edge (phi(v @ src,
//            C.fresh[v] @ C.bmap[src]) per vm_vals entry, keyed to
//            {exit source, its clone}); the exit arms retargeted to
//            the VMs (original AND clone sides); pre's Br becomes
//            the guard cbr(bs.cond) h / C.bmap[h]; the delivered
//            walk (bif_delivered, memoized in dcache) emits the
//            fresh phis at outside merges; the rewire sweep rekeys
//            phi args (in-loop keys -> the VM) and rewrites plain
//            reads to the delivered values; recompute_preds.
//
// DATA STRUCTURES (BifCtx, what the former monolith held in locals):
//   fn/S/inL/bs   the rewrite target, the one-pass scan tables
//                 (defi/defb/kc/kv), the loop-membership flags, the
//                 matched shape (bif/A/B/cond)
//   h/pre/nb/nv   the header, the dedicated preheader, the block /
//                 vreg universe at pass entry
//   exits         ExitEdge {from, to} per in-loop -> outside edge
//   loopdef       vreg -> defined inside the loop (SSA: one def)
//   arm_edge      block -> its in-loop pred for pure arms, else NONE
//   vm_vals[t]    the loop-defined vregs the VM entering t must
//                 merge (the reverse-reachability closure's target)
//   reads         (block, v) pairs the delivered walk must provide
//   C             CloneMap {bmap, fresh} -- the versioned copy's
//                 block/vreg naming, allocated before any VM block
//                 so later ids stay stable
//   edge_vm/vm_phi  per exit edge: the VM block id and, parallel to
//                 vm_vals[to], the emitted phi dsts
//   dcache        (v -> dst) memo per outside block, shared by the
//                 delivered walk and the rewire sweep so the sweep
//                 reads exactly what the walk produced
//
// THEORY (the delivered walk, and why validation precedes the edit):
//   * WHY A WALK AT ALL. Before versioning, a loop-defined value's
//   def (the header phi) DOMINATED every outside read: the SSA
//   builder never needed a phi there. Versioning breaks that
//   dominance -- after the guard, the value that flows past the
//   loop is the true version's v on one path and the clone's
//   C.fresh[v] on the other -- so a phi must be BORN at every
//   merge of those paths. The VM blocks hold the loop-exit merges;
//   outside merges (e.g. X entered by the header exit AND a break
//   arm) need fresh phis of the delivered values per pred.
//   * THE CLOSURE FIRST. vm_vals is collected over reverse
//   reachability BEFORE any read is rewritten, so every VM on every
//   path to a read carries every value that read may see -- a VM
//   that merged only the values one reader wanted would strand the
//   other readers behind a merge that never happened.
//   * VALIDATE, THEN EDIT. routes() proves every read reachable
//   from its providing VM/chain/merge BEFORE alloc_clone runs: a
//   mid-commit bail would leave a half-versioned fn (the guard's
//   false arm pointing at a clone whose merge phis never landed).
//   The commit therefore never fails: every route it walks was
//   proven, and its `arg == NONE' / cycle arms are paranoia.
//   * WHY THE ORIGINAL'S `pi = 3' IS GONE. The monolith marked a
//   failed merge-arg walk with a sentinel (pi = 3) so the final
//   `pi == 2' gate would fail; the split returns NONE directly --
//   same observable result (no phi, no dcache entry), one less
//   state to carry. The ONLY behavioral shape preserved to the
//   character: every other order, bound, and asymmetry is the
//   original's.
// ---- WHAT IT ENABLES --------------------------------------------------
// The dead arms (B in the true version, A' in the clone) and their
// merge phis degenerate under unreach + copyprop in the same tier
// round, the way the checkpoint-17/18 abandoned loop bodies do;
// what remains is two loop versions, each with the invariant branch
// resolved -- branch-free bodies for unroll/lcf/rotate to consume.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "loop_shape.hpp"
#include "unswitch_shape.hpp"

// ---- the pass context (the former monolith's locals) --------------------
struct BifCtx {
  Fn &fn;                           // the function under rewrite
  const LoopScan &S;                // the one-pass scan tables
  const std::vector<char> &inL;     // the loop-membership flags
  const BifShape &bs;               // bif/A/B/cond (matched upstream)
  uint32_t h{}, pre{};              // the header, its dedicated preheader
  uint32_t nb{}, nv{};              // block / vreg universe at entry
  std::vector<ExitEdge> exits{};    // every in-loop -> outside edge
  std::vector<char> loopdef{};      // vreg -> defined inside the loop
  std::vector<uint32_t> arm_edge{}; // pure arm -> its in-loop pred
  // per exit target: the loop-defined vregs its VMs must merge
  std::vector<std::vector<uint32_t>> vm_vals{};
  std::vector<std::pair<uint32_t, uint32_t>> reads{}; // (block, v)
  CloneMap C{};                                // the versioned copy's naming
  std::vector<uint32_t> edge_vm{};             // exit idx -> its VM block
  std::vector<std::vector<uint32_t>> vm_phi{}; // ... -> phi dsts
  // per outside block: (v -> delivered dst), walk + rewire shared
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> dcache{};
};

// ---- the gates -----------------------------------------------------------

// the preheader must be a dedicated `br h' block (no fall-through
// join, no second successor): the guard cbr replaces it wholesale.
constexpr bool bif_preheader_ok(const Fn &fn, uint32_t pre, uint32_t h) {
  const Instr &pt = fn.code[fn.blocks[pre].end - 1];
  if (pt.k != K::Br || pt.t != h)
    return false;
  return fn.succ0(pre) == h && fn.succ1(pre) == NONE;
}

constexpr uint32_t bif_loop_instrs(const Fn &fn, const std::vector<char> &inL,
                                   uint32_t nb) {
  uint32_t n = 0;
  for (uint32_t b = 0; b < nb; ++b)
    if (inL[b])
      n += fn.blocks[b].end - fn.blocks[b].begin;
  return n;
}

constexpr bool bif_growth_ok(const Fn &fn, const std::vector<char> &inL,
                             uint32_t nb) {
  const uint32_t n = bif_loop_instrs(fn, inL, nb);
  return n != 0 && n <= 120; // the versioning doubles the loop
}

// ---- the classification (loopdef + pure arms) ----------------------------

constexpr void bif_mark_loopdefs(BifCtx &X) {
  X.loopdef.assign(X.nv, 0);
  for (uint32_t v = 0; v < X.nv; ++v)
    if (X.S.defi[v] != NONE && X.inL[X.S.defb[v]])
      X.loopdef[v] = 1;
}

// a pure arm: an outside block whose ONLY pred is an in-loop exit
// source (and not the preheader -- pre is a source's target's
// sibling, not an arm): the VM below it is its value provider.
constexpr bool bif_is_pure_arm(const BifCtx &X, uint32_t p) {
  if (X.inL[p] || X.fn.preds[p].size() != 1)
    return false;
  if (!X.inL[X.fn.preds[p][0]])
    return false;
  return X.fn.preds[p][0] != X.pre;
}

constexpr void bif_mark_arms(BifCtx &X) {
  X.arm_edge.assign(X.nb, NONE);
  for (uint32_t p = 0; p < X.nb; ++p)
    if (bif_is_pure_arm(X, p))
      X.arm_edge[p] = X.fn.preds[p][0];
}

// ---- reverse reachability -------------------------------------------------

// every exit target that reaches b through outside blocks: the VMs
// whose merged values a read at b may see. Worklist flood over the
// PREDECESSOR edges; an in-loop pred means x is itself an exit
// target (recorded -- x, not p: the edge p->x leaves the loop).
constexpr void bif_rr_step(const BifCtx &X, std::vector<char> &seen,
                           std::vector<uint32_t> &work,
                           std::vector<uint32_t> &out) {
  const uint32_t x = work.back();
  work.pop_back();
  for (uint32_t p : X.fn.preds[x]) {
    if (p >= X.nb)
      continue;
    if (X.inL[p]) {
      out.push_back(x); // x is an exit target on a path to b
      continue;
    }
    if (!seen[p]) {
      seen[p] = 1;
      work.push_back(p);
    }
  }
}

constexpr void bif_rr(const BifCtx &X, uint32_t b, std::vector<uint32_t> &out) {
  std::vector<char> seen(X.nb, 0);
  std::vector<uint32_t> work{b};
  seen[b] = 1;
  while (!work.empty())
    bif_rr_step(X, seen, work, out);
}

// ---- the outside-use scan (vm_vals + reads) -------------------------------

constexpr void bif_add_val(std::vector<uint32_t> &vv, uint32_t v) {
  for (uint32_t x : vv)
    if (x == v)
      return;
  vv.push_back(v);
}

// record the read and collect v into every reachable target's VM
constexpr void bif_collect_read(BifCtx &X, const std::vector<uint32_t> &r,
                                uint32_t b, uint32_t v) {
  for (uint32_t t : r)
    bif_add_val(X.vm_vals[t], v);
  X.reads.push_back({b, v});
}

// a phi arg keyed to an OUTSIDE pred: the value flowed through that
// pred -- delivered at commit over the pred's reverse reachability.
constexpr void bif_outside_arg_src(BifCtx &X, uint32_t key, uint32_t arg,
                                   bool &bad) {
  if (key >= X.nb)
    return; // NONE or out of range: not a key the walk can follow
  std::vector<uint32_t> r{};
  bif_rr(X, key, r);
  if (r.empty()) { // the loop's values cannot reach it
    bad = true;
    return;
  }
  bif_collect_read(X, r, key, arg);
}

// a phi arg at block b: an in-loop key is an exit edge (the VM
// entering b merges the arg); an outside key delivers it later.
constexpr void bif_phi_arg_src(BifCtx &X, uint32_t b, uint32_t key,
                               uint32_t arg, bool &bad) {
  if (arg == NONE || arg >= X.nv || !X.loopdef[arg])
    return;
  if (key != NONE && key < X.nb && X.inL[key]) {
    bif_add_val(X.vm_vals[b], arg);
    return;
  }
  bif_outside_arg_src(X, key, arg, bad);
}

// a plain operand at b: a read over b's reverse reachability
constexpr void bif_plain_src(BifCtx &X, uint32_t b, uint32_t v, bool &bad) {
  if (v == NONE || v >= X.nv || !X.loopdef[v])
    return;
  std::vector<uint32_t> r{};
  bif_rr(X, b, r);
  if (r.empty()) { // not reachable from any exit: no merge can
    bad = true;    // exist (dominance says it cannot read anyway)
    return;
  }
  bif_collect_read(X, r, b, v);
}

constexpr void bif_scan_plain_operands(BifCtx &X, uint32_t b, const Instr &I,
                                       bool &bad) {
  bif_plain_src(X, b, I.a, bad);
  bif_plain_src(X, b, I.b2, bad);
  bif_plain_src(X, b, I.c2, bad);
  if (bad)
    return;
  for (uint32_t v : I.args) {
    bif_plain_src(X, b, v, bad);
    if (bad)
      break;
  }
}

constexpr void bif_scan_instr(BifCtx &X, uint32_t b, uint32_t i, bool &bad) {
  const Instr &I = X.fn.code[i];
  if (I.k == K::Phi) {
    bif_phi_arg_src(X, b, I.t, I.a, bad);
    bif_phi_arg_src(X, b, I.f, I.b2, bad);
  } else {
    bif_scan_plain_operands(X, b, I, bad);
  }
}

constexpr void bif_scan_block(BifCtx &X, uint32_t b, bool &bad) {
  for (uint32_t i = X.fn.blocks[b].begin; i < X.fn.blocks[b].end && !bad; ++i)
    bif_scan_instr(X, b, i, bad);
}

constexpr bool bif_scan_outside_uses(BifCtx &X) {
  X.vm_vals.assign(X.nb, {});
  X.reads.clear();
  bool bad = false;
  for (uint32_t b = 0; b < X.nb && !bad; ++b) {
    if (X.inL[b] || b == X.pre)
      continue;
    bif_scan_block(X, b, bad);
  }
  return !bad;
}

// ---- the routes validator (before the commit's first edit) ----------------

// forward: the chain/merge validators below recurse back into it
constexpr bool bif_routes(BifCtx &X, std::vector<char> &vis, uint32_t b,
                          uint32_t v);

constexpr bool bif_vm_has(const BifCtx &X, uint32_t t, uint32_t v) {
  for (uint32_t x : X.vm_vals[t])
    if (x == v)
      return true;
  return false;
}

constexpr bool bif_routes_chain(BifCtx &X, std::vector<char> &vis, uint32_t b,
                                uint32_t v) {
  const uint32_t p = X.fn.preds[b][0];
  if (p >= X.nb || X.inL[p])
    return false; // pre or nonsense: no value path
  vis[b] = 1;
  bool ok = bif_routes(X, vis, p, v);
  vis[b] = 0;
  return ok;
}

// one pred of a 2-pred merge: an in-loop pred needs the VM entering
// b to carry v; an outside pred routes recursively; OOB never does.
constexpr bool bif_routes_pred_ok(BifCtx &X, std::vector<char> &vis, uint32_t b,
                                  uint32_t v, uint32_t p) {
  if (p < X.nb && X.inL[p])
    return bif_vm_has(X, b, v);
  if (p < X.nb)
    return bif_routes(X, vis, p, v);
  return false;
}

constexpr bool bif_routes_merge(BifCtx &X, std::vector<char> &vis, uint32_t b,
                                uint32_t v) {
  vis[b] = 1;
  bool ok = true;
  for (uint32_t p : X.fn.preds[b])
    if (!bif_routes_pred_ok(X, vis, b, v, p)) {
      ok = false;
      break;
    }
  vis[b] = 0;
  return ok;
}

// routes(b, v): can the merged value for v be provided at b's entry?
// pure arm -> its VM carries v; chain -> the pred routes; merge ->
// one arg per pred. Outside cycles (sibling loops) never route:
// their 2-pred headers stop the walk, so the visiting guard fires
// only on true tangles.
constexpr bool bif_routes(BifCtx &X, std::vector<char> &vis, uint32_t b,
                          uint32_t v) {
  if (vis[b])
    return false; // cycle: not routable
  if (X.arm_edge[b] != NONE)
    return bif_vm_has(X, b, v); // the one VM below carries it
  if (X.fn.preds[b].size() == 1)
    return bif_routes_chain(X, vis, b, v);
  if (X.fn.preds[b].size() != 2)
    return false; // the phi representation is 2-arg
  return bif_routes_merge(X, vis, b, v);
}

constexpr bool bif_routes_all_ok(BifCtx &X) {
  std::vector<char> vis(X.nb, 0);
  for (const auto &rd : X.reads)
    if (!bif_routes(X, vis, rd.first, rd.second))
      return false;
  return true;
}

// ---- the commit: clone + version rewrites --------------------------------

// entered through pre itself (its false arm), the header's
// while-test KEPT LIVE (bypass = NONE) -- both versions run.
constexpr void bif_clone_loop(BifCtx &X) {
  X.C = alloc_clone(X.fn, X.S, X.inL);
  emit_loop_clone(X.fn, X.S, X.inL, X.C, X.h, X.pre, X.pre, NONE);
}

// bif knows INV true (br A), bif' knows it false (br B')
constexpr void bif_retarget_bif(BifCtx &X, uint32_t bif, uint32_t tgt) {
  Instr &T = X.fn.code[X.fn.blocks[bif].end - 1];
  T.k = K::Br;
  T.t = tgt;
  T.f = NONE;
  T.a = NONE;
  X.fn.succ[2 * bif] = tgt;
  X.fn.succ[2 * bif + 1] = NONE;
}

constexpr void bif_rewrite_branches(BifCtx &X) {
  bif_retarget_bif(X, X.bs.bif, X.bs.A); // the true version: B dies
  bif_retarget_bif(X, X.C.bmap[X.bs.bif],
                   X.C.bmap[X.bs.B]); // the clone: A' dies
}

// ---- the commit: the version-merge blocks --------------------------------

constexpr void bif_vm_open(BifCtx &X, uint32_t VM) {
  X.fn.blocks.push_back({});
  X.fn.succ.push_back(NONE);
  X.fn.succ.push_back(NONE);
  X.fn.blocks[VM].begin = (uint32_t)X.fn.code.size();
}

constexpr void bif_vm_close(BifCtx &X, uint32_t VM, uint32_t tgt) {
  X.fn.code.push_back({.k = K::Br, .t = tgt});
  X.fn.blocks[VM].end = (uint32_t)X.fn.code.size();
  X.fn.succ[2 * VM] = tgt;
  X.fn.succ[2 * VM + 1] = NONE;
}

// one phi per loop-defined value the exit target's VMs must merge,
// keyed: the true version's def from the exit source, the false
// version's from its clone.
constexpr uint32_t bif_emit_vm_phi(BifCtx &X, size_t ei, uint32_t v) {
  const uint32_t d = new_anon_vreg(X.fn, X.fn.base_of(v));
  X.fn.code.push_back({.k = K::Phi,
                       .dst = d,
                       .a = v,                // the true version's def
                       .b2 = X.C.fresh[v],    // the false version's def
                       .t = X.exits[ei].from, // keyed: the exit source
                       .f = X.C.bmap[X.exits[ei].from], // ... and clone
                       .c = (int64_t)X.fn.base_of(v)});
  return d;
}

constexpr void bif_emit_vm_phis(BifCtx &X, size_t ei) {
  const std::vector<uint32_t> &vals = X.vm_vals[X.exits[ei].to];
  X.vm_phi[ei].resize(vals.size());
  for (size_t j = 0; j < vals.size(); ++j)
    X.vm_phi[ei][j] = bif_emit_vm_phi(X, ei, vals[j]);
}

constexpr void bif_emit_vm(BifCtx &X, size_t ei) {
  const uint32_t VM = X.fn.nblocks();
  X.edge_vm[ei] = VM;
  bif_vm_open(X, VM);
  bif_emit_vm_phis(X, ei);
  bif_vm_close(X, VM, X.exits[ei].to);
}

constexpr void bif_emit_vms(BifCtx &X) {
  const size_t ne = X.exits.size();
  X.edge_vm.assign(ne, NONE);
  X.vm_phi.assign(ne, {});
  for (size_t ei = 0; ei < ne; ++ei)
    bif_emit_vm(X, ei);
}

// ---- the commit: retarget + guard ----------------------------------------

constexpr void bif_retarget_term(BifCtx &X, uint32_t B, uint32_t tgt,
                                 uint32_t VM) {
  Instr &T = X.fn.code[X.fn.blocks[B].end - 1];
  if (T.k == K::Br) {
    if (T.t == tgt)
      T.t = VM;
  } else if (T.k == K::Cbr) {
    if (T.t == tgt)
      T.t = VM;
    else if (T.f == tgt)
      T.f = VM;
  }
  X.fn.succ[2 * B] = T.t;
  X.fn.succ[2 * B + 1] = (T.k == K::Cbr) ? T.f : NONE;
}

// every exit arm (original AND clone) enters its VM
constexpr void bif_retarget_edge(BifCtx &X, size_t ei) {
  const uint32_t tgt = X.exits[ei].to, VM = X.edge_vm[ei];
  for (uint32_t B : {X.exits[ei].from, X.C.bmap[X.exits[ei].from]})
    bif_retarget_term(X, B, tgt, VM);
}

constexpr void bif_retarget_exits(BifCtx &X) {
  for (size_t ei = 0; ei < X.exits.size(); ++ei)
    bif_retarget_edge(X, ei);
}

// pre's br becomes the version selector
constexpr void bif_emit_guard(BifCtx &X) {
  Instr &pt = X.fn.code[X.fn.blocks[X.pre].end - 1];
  pt.k = K::Cbr;
  pt.a = X.bs.cond;
  pt.t = X.h;           // the true version: the original loop
  pt.f = X.C.bmap[X.h]; // the false version: the clone
  X.fn.succ[2 * X.pre] = X.h;
  X.fn.succ[2 * X.pre + 1] = X.C.bmap[X.h];
}

// ---- the delivered walk (emission) ----------------------------------------

// forward: the arm/chain/merge walkers below recurse back into it
constexpr uint32_t bif_delivered(BifCtx &X, std::vector<char> &vis, uint32_t b,
                                 uint32_t v);

constexpr long bif_edge_idx(const BifCtx &X, uint32_t from, uint32_t to) {
  for (size_t ei = 0; ei < X.exits.size(); ++ei)
    if (X.exits[ei].from == from && X.exits[ei].to == to)
      return (long)ei;
  return -1; // cannot happen: every in-loop pred of an outside block
} // is an inventoried exit source

constexpr uint32_t bif_vm_val(const BifCtx &X, long ei, uint32_t v) {
  const std::vector<uint32_t> &vals = X.vm_vals[X.exits[(size_t)ei].to];
  for (size_t j = 0; j < vals.size(); ++j)
    if (vals[j] == v)
      return X.vm_phi[(size_t)ei][j];
  return NONE;
}

constexpr uint32_t bif_memo(const BifCtx &X, uint32_t b, uint32_t v) {
  for (const auto &kv : X.dcache[b])
    if (kv.first == v)
      return kv.second;
  return NONE;
}

// pure arm: the one VM below it carries the merged value
constexpr uint32_t bif_delivered_arm(BifCtx &X, uint32_t b, uint32_t v) {
  const long ei = bif_edge_idx(X, X.arm_edge[b], b);
  const uint32_t d = ei < 0 ? NONE : bif_vm_val(X, ei, v);
  if (d != NONE)
    X.dcache[b].push_back({v, d});
  return d;
}

// single-pred chain: whatever the pred delivers
constexpr uint32_t bif_delivered_chain(BifCtx &X, std::vector<char> &vis,
                                       uint32_t b, uint32_t v) {
  const uint32_t p = X.fn.preds[b][0];
  if (p >= X.nb || X.inL[p])
    return NONE; // pre or nonsense: no value path
  vis[b] = 1;
  const uint32_t d = bif_delivered(X, vis, p, v);
  vis[b] = 0;
  if (d != NONE)
    X.dcache[b].push_back({v, d});
  return d;
}

// the phi's block ranges after an insert at `at': every other
// block's begin/end shifts past it (>= at for begin, > at for end).
constexpr void bif_shift_ranges_after(BifCtx &X, uint32_t b, uint32_t at) {
  for (uint32_t x = 0; x < X.fn.nblocks(); ++x) {
    if (x == b)
      continue;
    if (X.fn.blocks[x].begin >= at)
      X.fn.blocks[x].begin++;
    if (X.fn.blocks[x].end > at)
      X.fn.blocks[x].end++;
  }
}

// a fresh phi at b's head merges the delivered args (keyed to the
// VM for an in-loop pred, the outside pred itself otherwise)
constexpr uint32_t bif_emit_merge_phi(BifCtx &X, uint32_t b, uint32_t v,
                                      uint32_t a0, uint32_t a1, uint32_t k0,
                                      uint32_t k1) {
  const uint32_t d = new_anon_vreg(X.fn, X.fn.base_of(v));
  const Instr np{.k = K::Phi,
                 .dst = d,
                 .a = a0,
                 .b2 = a1,
                 .t = k0,
                 .f = k1,
                 .c = (int64_t)X.fn.base_of(v)};
  const uint32_t at = X.fn.blocks[b].begin;
  X.fn.code.insert(X.fn.code.begin() + (long)at, np);
  bif_shift_ranges_after(X, b, at);
  X.fn.blocks[b].end++;
  X.dcache[b].push_back({v, d});
  return d;
}

// one arg per post-pred: the VM for an in-loop pred (its phi for v
// plus the VM block as key), the delivered value for an outside
// pred (the pred itself as key -- outside preds stay preds).
constexpr void bif_merge_pred_arg(BifCtx &X, std::vector<char> &vis, uint32_t p,
                                  uint32_t b, uint32_t v, uint32_t &arg,
                                  uint32_t &key) {
  if (p < X.nb && X.inL[p]) {
    const long ei = bif_edge_idx(X, p, b);
    if (ei >= 0) {
      arg = bif_vm_val(X, ei, v); // the VM entering b on this edge
      key = X.edge_vm[(size_t)ei];
    }
  } else if (p < X.nb) {
    arg = bif_delivered(X, vis, p, v);
    key = p; // outside preds stay preds (unretargeted)
  }
}

constexpr void bif_pack_merge_arg(size_t &pi, uint32_t &a0, uint32_t &k0,
                                  uint32_t &a1, uint32_t &k1, uint32_t arg,
                                  uint32_t key) {
  if (pi == 0) {
    a0 = arg;
    k0 = key;
  } else {
    a1 = arg;
    k1 = key;
  }
  ++pi;
}

constexpr uint32_t bif_merge_walk_preds(BifCtx &X, std::vector<char> &vis,
                                        uint32_t b, uint32_t v) {
  uint32_t a0 = NONE, k0 = NONE, a1 = NONE, k1 = NONE;
  size_t pi = 0;
  for (uint32_t p : X.fn.preds[b]) {
    uint32_t arg = NONE, key = NONE;
    bif_merge_pred_arg(X, vis, p, b, v, arg, key);
    if (arg == NONE)
      return NONE; // the original's pi=3 marker: no phi is built
    bif_pack_merge_arg(pi, a0, k0, a1, k1, arg, key);
  }
  if (pi != 2 || k0 == NONE || k1 == NONE || k0 == k1)
    return NONE;
  return bif_emit_merge_phi(X, b, v, a0, a1, k0, k1);
}

constexpr uint32_t bif_delivered_merge(BifCtx &X, std::vector<char> &vis,
                                       uint32_t b, uint32_t v) {
  vis[b] = 1;
  const uint32_t d = bif_merge_walk_preds(X, vis, b, v);
  vis[b] = 0;
  return d;
}

// the post-versioning vreg carrying loop-defined v at outside block
// b's entry; memoized (dcache) so the rewire sweep below reads the
// same value the walk produced. Validation already proved every
// route exists -- the failure arms are paranoia.
constexpr uint32_t bif_delivered(BifCtx &X, std::vector<char> &vis, uint32_t b,
                                 uint32_t v) {
  const uint32_t hit = bif_memo(X, b, v);
  if (hit != NONE)
    return hit;
  if (vis[b])
    return NONE; // paranoia: validation proved acyclicity
  if (X.arm_edge[b] != NONE)
    return bif_delivered_arm(X, b, v);
  if (X.fn.preds[b].size() == 1)
    return bif_delivered_chain(X, vis, b, v);
  if (X.fn.preds[b].size() != 2)
    return NONE; // paranoia (validation proved 2)
  return bif_delivered_merge(X, vis, b, v);
}

constexpr void bif_deliver_reads(BifCtx &X) {
  X.dcache.assign(X.nb, {});
  std::vector<char> vis(X.nb, 0);
  for (const auto &rd : X.reads)
    (void)bif_delivered(X, vis, rd.first, rd.second);
}

// ---- the rewire sweep -----------------------------------------------------

// in-loop phi key -> the VM entering this block; the arg becomes
// the VM's phi for it
constexpr void bif_apply_vm_arg(BifCtx &X, uint32_t b, uint32_t &key,
                                uint32_t &arg) {
  const long ei = bif_edge_idx(X, key, b);
  if (ei < 0)
    return; // paranoia: the scan classified real edges
  key = X.edge_vm[(size_t)ei];
  arg = bif_vm_val(X, ei, arg);
}

// rekey one phi arg: an in-loop key is an exit edge (the VM merges
// the arg); an outside key delivered the value (the memo holds it).
// An arg that is not loop-defined is the SAME vreg on both version
// paths: its def dominates the preheader, hence the whole versioned
// region -- nothing to do.
constexpr void bif_rekey_arg(BifCtx &X, uint32_t b, uint32_t &key,
                             uint32_t &arg) {
  if (arg == NONE || arg >= X.nv || !X.loopdef[arg])
    return;
  if (key != NONE && key < X.nb && X.inL[key]) {
    bif_apply_vm_arg(X, b, key, arg);
    return;
  }
  if (key < X.nb) { // delivered through the outside pred
    const uint32_t d = bif_memo(X, key, arg);
    if (d != NONE)
      arg = d;
  }
}

constexpr void bif_rekey_phi(BifCtx &X, uint32_t b, Instr &I) {
  bif_rekey_arg(X, b, I.t, I.a);
  bif_rekey_arg(X, b, I.f, I.b2);
}

constexpr void bif_rewrite_use(BifCtx &X, uint32_t b, uint32_t &v) {
  if (v == NONE || v >= X.nv || !X.loopdef[v])
    return;
  const uint32_t d = bif_memo(X, b, v);
  if (d != NONE)
    v = d; // the delivered (version-merged) value
}

constexpr void bif_rewrite_plain_uses(BifCtx &X, uint32_t b, Instr &I) {
  bif_rewrite_use(X, b, I.a);
  bif_rewrite_use(X, b, I.b2);
  bif_rewrite_use(X, b, I.c2);
  for (uint32_t &av : I.args)
    bif_rewrite_use(X, b, av);
}

constexpr void bif_rewire_block(BifCtx &X, uint32_t b) {
  for (uint32_t i = X.fn.blocks[b].begin; i < X.fn.blocks[b].end; ++i) {
    Instr &I = X.fn.code[i];
    if (I.k == K::Phi)
      bif_rekey_phi(X, b, I);
    else
      bif_rewrite_plain_uses(X, b, I);
  }
}

constexpr void bif_rewire_outside(BifCtx &X) {
  for (uint32_t b = 0; b < X.nb; ++b) {
    if (X.inL[b] || b == X.pre)
      continue;
    bif_rewire_block(X, b);
  }
}

// ---- the commit assembly --------------------------------------------------

constexpr void bif_commit(BifCtx &X, OptStats &os) {
  os.unswitches++;
  os.body_unswitches++;
  bif_clone_loop(X);       // the versioned copy (while-test kept)
  bif_rewrite_branches(X); // bif: br A / bif': br B'
  bif_emit_vms(X);         // one VM per exit edge
  bif_retarget_exits(X);   // exit arms enter their VMs
  bif_emit_guard(X);       // pre: cbr(INV) h / h'
  bif_deliver_reads(X);    // the merge phis are born
  bif_rewire_outside(X);   // reads see the delivered values
  X.fn.recompute_preds();
}
