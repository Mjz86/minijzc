#pragma once
// === unswitch_bif.hpp ========================================
// BODY-IF unswitching (checkpoint 19, README wishlist task 3's "the
// natural next step"): the classic LLVM "trivial" unswitch of an
// invariant INNER branch. The loop's header test varies per iteration
// (a real while-condition), but a body branch bif: cbr(INV) A/B with
// INV SSA-proven loop-invariant, both arms in-loop and each entered
// only through bif, is versioned instead:
//     pre: br h   ->   pre: cbr(INV) h / h'
//     the original loop  = the INV-true  version: bif: br A (B dies)
//     the full clone     = the INV-false version: bif': br B' (A' dies)
// BOTH versions keep running the loop (the header test stays live in
// each) — sound because the original picked the SAME arm every
// iteration: INV is an SSA value, it cannot change mid-loop.
//
// Every exit edge (the header's exit arm INCLUDED — no zero-iteration
// edge is replaced here, both versions run the loop) gets a
// VERSION-MERGE block VM: preds {exit source, its clone}, one phi per
// loop-defined value read at or through the exit target —
// phi(v @ source, fresh v @ clone) — then br to the target. That
// holds the <=2-preds invariant the 2-arg phi representation needs:
// the target's preds become its original outside preds + one VM per
// exit edge, and it had at most two preds to begin with. The dead
// arms (B in the true version, A' in the clone) and their merge phis
// degenerate under unreach + copyprop in the same tier round, the
// way the checkpoint-17/18 abandoned loop bodies do.
//
// Outside reads of loop-defined values are routed to their merged
// value by the DELIVERED walk: the value that leaves the loop on some
// exit edge is merged at that edge's VM, and reads downstream get it
// by following their preds back to the VMs:
//   * a PURE ARM (an outside block whose only pred is an in-loop exit
//     source: break arms, a single-exit loop's target X) reads the
//     VM's phi directly — every path there passes that one VM;
//   * a single-pred chain off one of those passes the value up;
//   * a MERGE (2 preds: e.g. X entered by the header exit AND a break
//     arm) whose values arrive per-pred gets a fresh phi at the merge
//     itself — one arg per post-pred (the VM for an in-loop pred, the
//     recursively delivered value for an outside pred) — the SSA
//     builder left these as PLAIN reads (the header phi dominates the
//     whole post-loop region, so no phi existed), and versioning
//     breaks that dominance, so the phi is born here.
// Every value the walk touches is collected FIRST: a read at (or
// keyed to) block b collects v into vm_vals[t] for every exit target
// t that reaches b through outside blocks — the reverse-reachability
// closure, so every VM on every path carries every value it must. The
// walk itself is validated (cycle-guarded, sources checked) BEFORE
// the commit's first edit: a bail leaves the fn untouched.
// ---- ALGORITHM --------------------------------------------------------
// unswitch_body_if, in phases: dedicated `br h' preheader; growth
// cap 1..120; exit_inventory with primary NONE (the header's exit
// arm is a REGULAR exit -- both versions keep running); loopdef;
// arm_edge (pure arms: outside single-pred blocks, pred in-loop);
// rr (reverse reachability to the exit targets). The scan builds
// vm_vals[t] + the reads list; `routes' validates every read
// BEFORE the commit's first edit -- a bail leaves the fn
// untouched. Commit: the clone via alloc_clone + emit_loop_clone
// (pre, NONE); the version rewrites bif: br A / bif': br B'; one
// VM per exit edge (phi(v @ src, C.fresh[v] @ C.bmap[src]));
// retarget the exit arms; pre becomes the guard cbr(bs.cond) h /
// C.bmap[h]; the delivered walk emits merge phis; rewire; preds.
// ---- DATA STRUCTURES --------------------------------------------------
// bs (BifShape bif/A/B/cond), exits (ExitEdge), loopdef, arm_edge,
// vm_vals + reads, vis, the CloneMap C (bmap/fresh), edge_vm +
// vm_phi, edge_idx/vm_val, dcache + memo.
// ---- THEORY: the body-if scan + commit discipline ----------------------
// INV is an SSA value (cond_invariant): it cannot change mid-loop,
// so the original picked the SAME arm every iteration -- the true
// version (bif: br A) is exactly the original with B dead, and the
// clone (bif': br B') is the INV-false version; both keep running
// the loop, so nothing is skipped. Every exit edge gets a VM
// holding the <=2-preds invariant the 2-arg phi needs. The
// delivered walk restores SSA: the header phi used to dominate the
// whole post-loop region; versioning breaks that dominance, so
// phis are born at the merges, carrying every value every VM must.
// ---- WHAT IT ENABLES --------------------------------------------------
// The dead arms (B, A') degenerate under unreach + copyprop in the
// same tier round, like the abandoned checkpoint-17/18 bodies.
#include "../../base/pass_cfg.hpp"
#include "../../ir/instr.hpp"
#include "../../ir/phi_norm.hpp"
#include "../analysis/an.hpp"
#include "loop_shape.hpp"
#include "unswitch_shape.hpp"

constexpr bool unswitch_body_if(Fn &fn, const An &A, OptStats &os,
                                const LoopScan &S, const std::vector<char> &inL,
                                uint32_t h, const LoopShape &sh,
                                const BifShape &bs) {
  const uint32_t nb = S.nb, nv = S.nv;
  const uint32_t pre = sh.pre;
  // ---- the preheader must be a dedicated `br h` block ------------------
  {
    const Instr &pt = fn.code[fn.blocks[pre].end - 1];
    if (pt.k != K::Br || pt.t != h)
      return false;
    if (fn.succ0(pre) != h || fn.succ1(pre) != NONE)
      return false;
  }
  // ---- growth cap --------------------------------------------------------
  uint32_t loop_instrs = 0;
  for (uint32_t b = 0; b < nb; ++b)
    if (inL[b])
      loop_instrs += fn.blocks[b].end - fn.blocks[b].begin;
  if (loop_instrs == 0 || loop_instrs > 120)
    return false;
  // ---- exit-edge inventory: the header's exit arm INCLUDED --------------
  std::vector<ExitEdge> exits{};
  if (!exit_inventory(fn, inL, nb, NONE, NONE, pre, h, exits))
    return false;
  if (exits.empty()) // sh.ok guarantees the header's exit arm exists
    return false;
  // ---- the outside-use classification ------------------------------------
  std::vector<char> loopdef(nv, 0);
  for (uint32_t v = 0; v < nv; ++v)
    if (S.defi[v] != NONE && inL[S.defb[v]])
      loopdef[v] = 1;
  // pure arm: an outside block whose only pred is an in-loop exit source
  std::vector<uint32_t> arm_edge(nb, NONE); // pure arm -> its in-loop pred
  for (uint32_t p = 0; p < nb; ++p)
    if (!inL[p] && fn.preds[p].size() == 1 && inL[fn.preds[p][0]] &&
        fn.preds[p][0] != pre)
      arm_edge[p] = fn.preds[p][0];
  // reverse reachability: every exit target that reaches b through
  // outside blocks (the VMs whose merged values a read at b may see)
  auto rr = [&](uint32_t b, std::vector<uint32_t> &out) {
    std::vector<char> seen(nb, 0);
    std::vector<uint32_t> work{b};
    seen[b] = 1;
    while (!work.empty()) {
      const uint32_t x = work.back();
      work.pop_back();
      for (uint32_t p : fn.preds[x]) {
        if (p >= nb)
          continue;
        if (inL[p]) {
          out.push_back(x); // x is an exit target on a path to b
          continue;
        }
        if (!seen[p]) {
          seen[p] = 1;
          work.push_back(p);
        }
      }
    }
  };
  auto add_val = [](std::vector<uint32_t> &vv, uint32_t v) {
    for (uint32_t x : vv)
      if (x == v)
        return;
    vv.push_back(v);
  };
  // vm_vals[s]: the loop-defined vregs the VM for any edge into s must
  // merge. Reads recorded for the delivered walk (plain at b / the arg
  // of a phi keyed to outside pred b).
  std::vector<std::vector<uint32_t>> vm_vals(nb);
  std::vector<std::pair<uint32_t, uint32_t>> reads{}; // (block, v)
  bool bad = false;
  for (uint32_t b = 0; b < nb && !bad; ++b) {
    if (inL[b] || b == pre)
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end && !bad; ++i) {
      const Instr &I = fn.code[i];
      if (I.k == K::Phi) {
        // arg a is keyed to pred t; arg b2 to pred f. An in-loop key is
        // an exit edge: the VM entering this block merges the arg. An
        // outside key delivers the value that flowed THROUGH that pred:
        // collected over its reverse-reachability, delivered at commit.
        auto arg_src = [&](uint32_t key, uint32_t arg) {
          if (arg == NONE || arg >= nv || !loopdef[arg])
            return;
          if (key != NONE && key < nb && inL[key]) {
            add_val(vm_vals[b], arg);
            return;
          }
          if (key < nb) {
            std::vector<uint32_t> r{};
            rr(key, r);
            if (r.empty()) { // the loop's values cannot reach it
              bad = true;
              return;
            }
            for (uint32_t t : r)
              add_val(vm_vals[t], arg);
            reads.push_back({key, arg});
          }
        };
        arg_src(I.t, I.a);
        arg_src(I.f, I.b2);
      } else {
        auto plain_src = [&](uint32_t v) {
          if (v == NONE || v >= nv || !loopdef[v])
            return;
          std::vector<uint32_t> r{};
          rr(b, r);
          if (r.empty()) { // not reachable from any exit: no merge can
            bad = true;    // exist (dominance says it cannot read anyway)
            return;
          }
          for (uint32_t t : r)
            add_val(vm_vals[t], v);
          reads.push_back({b, v});
        };
        plain_src(I.a);
        plain_src(I.b2);
        plain_src(I.c2);
        if (!bad)
          for (uint32_t v : I.args) {
            plain_src(v);
            if (bad)
              break;
          }
      }
    }
  }
  if (bad)
    return false;
  // ---- validate the delivered walks (BEFORE any edit) --------------------
  // routes(b, v): can the merged value for v be provided at b's entry?
  // pure arm -> its VM carries v (v was collected into vm_vals[b]);
  // chain -> the pred routes; merge -> one arg per pred (the VM for an
  // in-loop pred needs v at this block, outside preds route further).
  // Outside cycles (sibling loops) never route: their 2-pred headers
  // stop the walk, so the visiting guard fires only on true tangles.
  {
    std::vector<char> vis(nb, 0);
    auto routes = [&](auto &&self, uint32_t b, uint32_t v) -> bool {
      if (vis[b])
        return false; // cycle: not routable
      auto has = [&](uint32_t t) {
        for (uint32_t x : vm_vals[t])
          if (x == v)
            return true;
        return false;
      };
      if (arm_edge[b] != NONE)
        return has(b); // the one VM below carries it
      if (fn.preds[b].size() == 1) {
        const uint32_t p = fn.preds[b][0];
        if (p >= nb || inL[p])
          return false;
        vis[b] = 1;
        bool ok = self(self, p, v);
        vis[b] = 0;
        return ok;
      }
      if (fn.preds[b].size() != 2)
        return false; // the phi representation is 2-arg
      vis[b] = 1;
      bool ok = true;
      for (uint32_t p : fn.preds[b]) {
        if (p < nb && inL[p]) {
          if (!has(b)) { // the VM entering b must carry v
            ok = false;
            break;
          }
        } else if (p < nb) {
          if (!self(self, p, v)) {
            ok = false;
            break;
          }
        } else {
          ok = false;
          break;
        }
      }
      vis[b] = 0;
      return ok;
    };
    for (const auto &rd : reads)
      if (!routes(routes, rd.first, rd.second))
        return false;
  }
  // ================= commit =================
  os.unswitches++;
  os.body_unswitches++;
  // -- the clone: entered through pre itself (its false arm), the
  //    header's while-test KEPT LIVE (bypass = NONE) ---------------------
  const CloneMap C = alloc_clone(fn, S, inL);
  emit_loop_clone(fn, S, inL, C, h, pre, pre, NONE);
  // -- the version rewrites: bif knows INV true, bif' knows it false --
  {
    Instr &T = fn.code[fn.blocks[bs.bif].end - 1];
    T.k = K::Br;
    T.t = bs.A;
    T.f = NONE;
    T.a = NONE;
    fn.succ[2 * bs.bif] = bs.A;
    fn.succ[2 * bs.bif + 1] = NONE;
    Instr &F = fn.code[fn.blocks[C.bmap[bs.bif]].end - 1];
    F.k = K::Br;
    F.t = C.bmap[bs.B];
    F.f = NONE;
    F.a = NONE;
    fn.succ[2 * C.bmap[bs.bif]] = C.bmap[bs.B];
    fn.succ[2 * C.bmap[bs.bif] + 1] = NONE;
  }
  // -- the version-merge block per exit edge -----------------------------
  const size_t ne = exits.size();
  std::vector<uint32_t> edge_vm(ne, NONE);
  std::vector<std::vector<uint32_t>> vm_phi(ne); // per edge, vm_vals-parallel
  for (size_t ei = 0; ei < ne; ++ei) {
    const uint32_t src = exits[ei].from, tgt = exits[ei].to;
    const uint32_t VM = fn.nblocks();
    edge_vm[ei] = VM;
    fn.blocks.push_back({});
    fn.succ.push_back(NONE);
    fn.succ.push_back(NONE);
    fn.blocks[VM].begin = (uint32_t)fn.code.size();
    const std::vector<uint32_t> &vals = vm_vals[tgt];
    vm_phi[ei].resize(vals.size());
    for (size_t j = 0; j < vals.size(); ++j) {
      const uint32_t v = vals[j];
      const uint32_t d = new_anon_vreg(fn, fn.base_of(v));
      fn.code.push_back({.k = K::Phi,
                         .dst = d,
                         .a = v,           // the true version's def
                         .b2 = C.fresh[v], // the false version's def
                         .t = src,         // keyed: the exit source
                         .f = C.bmap[src], // ... and its clone
                         .c = (int64_t)fn.base_of(v)});
      vm_phi[ei][j] = d;
    }
    fn.code.push_back({.k = K::Br, .t = tgt});
    fn.blocks[VM].end = (uint32_t)fn.code.size();
    fn.succ[2 * VM] = tgt;
    fn.succ[2 * VM + 1] = NONE;
  }
  // -- retarget: every exit arm (original AND clone) enters its VM ------
  for (size_t ei = 0; ei < ne; ++ei) {
    const uint32_t tgt = exits[ei].to, VM = edge_vm[ei];
    for (uint32_t B : {exits[ei].from, C.bmap[exits[ei].from]}) {
      Instr &T = fn.code[fn.blocks[B].end - 1];
      if (T.k == K::Br) {
        if (T.t == tgt)
          T.t = VM;
      } else if (T.k == K::Cbr) {
        if (T.t == tgt)
          T.t = VM;
        else if (T.f == tgt)
          T.f = VM;
      }
      fn.succ[2 * B] = T.t;
      fn.succ[2 * B + 1] = (T.k == K::Cbr) ? T.f : NONE;
    }
  }
  // -- the guard: pre's br becomes the version selector ------------------
  {
    Instr &pt = fn.code[fn.blocks[pre].end - 1];
    pt.k = K::Cbr;
    pt.a = bs.cond;
    pt.t = h;         // the true version: the original loop
    pt.f = C.bmap[h]; // the false version: the clone
    fn.succ[2 * pre] = h;
    fn.succ[2 * pre + 1] = C.bmap[h];
  }
  // -- the delivered walk (emission) --------------------------------------
  // the post-versioning vreg carrying loop-defined v at outside block
  // b's entry; memoized so the rewire sweep below reads the same value
  // the walk produced. Validation already proved every route exists.
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> dcache(nb);
  auto edge_idx = [&](uint32_t from, uint32_t to) -> long {
    for (size_t ei = 0; ei < ne; ++ei)
      if (exits[ei].from == from && exits[ei].to == to)
        return (long)ei;
    return -1; // cannot happen: every in-loop pred of an outside block
  };           // is an inventoried exit source
  auto vm_val = [&](long ei, uint32_t v) -> uint32_t {
    const std::vector<uint32_t> &vals = vm_vals[exits[(size_t)ei].to];
    for (size_t j = 0; j < vals.size(); ++j)
      if (vals[j] == v)
        return vm_phi[(size_t)ei][j];
    return NONE;
  };
  auto memo = [&](uint32_t b, uint32_t v) -> uint32_t {
    for (auto &kv : dcache[b])
      if (kv.first == v)
        return kv.second;
    return NONE;
  };
  {
    std::vector<char> vis(nb, 0);
    auto delivered = [&](auto &&self, uint32_t b, uint32_t v) -> uint32_t {
      const uint32_t hit = memo(b, v);
      if (hit != NONE)
        return hit;
      if (vis[b])
        return NONE; // paranoia: validation proved acyclicity
      // pure arm: the one VM below it carries the merged value
      if (arm_edge[b] != NONE) {
        const long ei = edge_idx(arm_edge[b], b);
        const uint32_t d = ei < 0 ? NONE : vm_val(ei, v);
        if (d != NONE)
          dcache[b].push_back({v, d});
        return d;
      }
      // single-pred chain: whatever the pred delivers
      if (fn.preds[b].size() == 1) {
        const uint32_t p = fn.preds[b][0];
        if (p >= nb || inL[p])
          return NONE; // pre or nonsense: no value path
        vis[b] = 1;
        const uint32_t d = self(self, p, v);
        vis[b] = 0;
        if (d != NONE)
          dcache[b].push_back({v, d});
        return d;
      }
      // merge: one arg per post-pred (the VM for an in-loop pred, the
      // delivered value for an outside pred); a fresh phi at b merges
      if (fn.preds[b].size() != 2)
        return NONE; // paranoia (validation proved 2)
      vis[b] = 1;
      uint32_t a0 = NONE, k0 = NONE, a1 = NONE, k1 = NONE;
      size_t pi = 0;
      uint32_t d = NONE;
      for (uint32_t p : fn.preds[b]) {
        uint32_t arg = NONE, key = NONE;
        if (p < nb && inL[p]) {
          const long ei = edge_idx(p, b);
          if (ei >= 0) {
            arg = vm_val(ei, v); // the VM entering b on this edge
            key = edge_vm[(size_t)ei];
          }
        } else if (p < nb) {
          arg = self(self, p, v);
          key = p; // outside preds stay preds (unretargeted)
        }
        if (arg == NONE) {
          pi = 3; // failure marker
          break;
        }
        if (pi == 0) {
          a0 = arg;
          k0 = key;
        } else {
          a1 = arg;
          k1 = key;
        }
        ++pi;
      }
      vis[b] = 0;
      if (pi == 2 && k0 != NONE && k1 != NONE && k0 != k1) {
        d = new_anon_vreg(fn, fn.base_of(v));
        const Instr np{.k = K::Phi,
                       .dst = d,
                       .a = a0,
                       .b2 = a1,
                       .t = k0,
                       .f = k1,
                       .c = (int64_t)fn.base_of(v)};
        const uint32_t at = fn.blocks[b].begin;
        fn.code.insert(fn.code.begin() + (long)at, np);
        for (uint32_t x = 0; x < fn.nblocks(); ++x) {
          if (x == b)
            continue;
          if (fn.blocks[x].begin >= at)
            fn.blocks[x].begin++;
          if (fn.blocks[x].end > at)
            fn.blocks[x].end++;
        }
        fn.blocks[b].end++;
        dcache[b].push_back({v, d});
      }
      return d;
    };
    for (const auto &rd : reads)
      (void)delivered(delivered, rd.first, rd.second);
  }
  // -- rewire the outside reads ------------------------------------------
  for (uint32_t b = 0; b < nb; ++b) {
    if (inL[b] || b == pre)
      continue;
    for (uint32_t i = fn.blocks[b].begin; i < fn.blocks[b].end; ++i) {
      Instr &I = fn.code[i];
      if (I.k == K::Phi) {
        auto rk = [&](uint32_t &key, uint32_t &arg) {
          if (arg == NONE || arg >= nv || !loopdef[arg])
            return;
          if (key != NONE && key < nb && inL[key]) {
            const long ei = edge_idx(key, b);
            if (ei < 0)
              return; // paranoia: the scan classified real edges
            key = edge_vm[(size_t)ei];
            arg = vm_val(ei, arg);
            return;
          }
          if (key < nb) { // delivered through the outside pred
            const uint32_t d = memo(key, arg);
            if (d != NONE)
              arg = d;
          }
          // (an arg that is not loop-defined is the SAME vreg on both
          // version paths: its def dominates the preheader, hence the
          // whole versioned region — nothing to do)
        };
        rk(I.t, I.a);
        rk(I.f, I.b2);
        continue;
      }
      auto rw = [&](uint32_t &v) {
        if (v == NONE || v >= nv || !loopdef[v])
          return;
        const uint32_t d = memo(b, v);
        if (d != NONE)
          v = d; // the delivered (version-merged) value
      };
      rw(I.a);
      rw(I.b2);
      rw(I.c2);
      for (uint32_t &av : I.args)
        rw(av);
    }
  }
  fn.recompute_preds();
  return true; // one loop per call: the driver re-analyzes
}
