#pragma once
// === instr.hpp ========================================
// IR core: vregs, versions, Instr/Block/Fn, enums K/B, helpers.

#include "../config.hpp"

// ============================================================================
// 5. IR: three-address code in basic blocks
// ============================================================================
enum class K : uint8_t {
  Const,
  Copy,
  Bin,
  Br,
  Cbr,
  Print,
  Phi,
  Exit,
  Alloc,
  Load,
  Store,
  Len,
  Input, // arrays + stdin (arena handles)
  Depth, // dst = current call depth (frames incl
         // main: 1 in main, k+1 in the k-th
         // callee) — created by tail-call
         // elimination to materialize the exact
         // recursion budget; pure within a fn
  Call,
  Ret, // functions: c = callee fn index (args in
       // Instr::args); Ret never appears in main
  Bkpt // `breakpoint x, y;` — a debugging observation point. args = the
       // observed vregs (scalars or array handles): real USES, so the
       // optimizer keeps the values alive (DCE roots; observed arrays stay
       // in the arena, un-promoted by mem2reg). Executing it is a NO-OP for
       // program output in every runner (parity unchanged); the debugger
       // VM stops there and shows the observed values. dst is always NONE
       // and src carries the statement's source offset
};
enum class B : uint8_t {
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  Lt,
  Le,
  Gt,
  Ge,
  Eq,
  Ne,
  And,
  Or,
  Xor,
  Shl,
  LShr,
  AShr,
  MulHi // bitwise/shift/mulhi: created by
        // the optimizer (div-by-const magic);
        // the surface language has no syntax
        // for them (yet)
};
constexpr const char *bname(B b) {
  switch (b) {
  case B::Add:
    return "+";
  case B::Sub:
    return "-";
  case B::Mul:
    return "*";
  case B::Div:
    return "/";
  case B::Mod:
    return "%";
  case B::Lt:
    return "<";
  case B::Le:
    return "<=";
  case B::Gt:
    return ">";
  case B::Ge:
    return ">=";
  case B::Eq:
    return "==";
  case B::Ne:
    return "!=";
  case B::And:
    return "&";
  case B::Or:
    return "|";
  case B::Xor:
    return "^";
  case B::Shl:
    return "shl";
  case B::LShr:
    return "lshr";
  case B::AShr:
    return "ashr";
  case B::MulHi:
    return "mulhi";
  }
  return "?";
}

// one B op on values (used by the compile-time loop simulator)
constexpr Rv w_bin(B b, val_t x, val_t y) {
  switch (b) {
  case B::Add:
    return w_add(x, y);
  case B::Sub:
    return w_sub(x, y);
  case B::Mul:
    return w_mul(x, y);
  case B::Div:
    return w_div(x, y);
  case B::Mod:
    return w_mod(x, y);
  case B::Lt:
    return {x < y, false};
  case B::Le:
    return {x <= y, false};
  case B::Gt:
    return {x > y, false};
  case B::Ge:
    return {x >= y, false};
  case B::Eq:
    return {x == y, false};
  case B::Ne:
    return {x != y, false};
  case B::And:
    return w_and(x, y);
  case B::Or:
    return w_or(x, y);
  case B::Xor:
    return w_xor(x, y);
  case B::Shl:
    return w_shl(x, y);
  case B::LShr:
    return w_lshr(x, y);
  case B::AShr:
    return w_ashr(x, y);
  case B::MulHi:
    return w_mulhi(x, y);
  }
  return {0, true};
}

struct Instr {
  K k{};
  B b{};                      // Bin opcode
  uint32_t dst{NONE};         // defined vreg (NONE if none)
  uint32_t a{NONE}, b2{NONE}; // operands (NONE = unused; 0 is a VALID vreg!)
  uint32_t t{NONE}, f{NONE};  // Br target / Cbr targets; Phi: pred ids of args
  int64_t c{};       // Const value; Phi: base var id; Call: callee fn idx
  uint32_t c2{NONE}; // third operand (Store: the stored value vreg)
  // nt = "no trap PROVEN": a range/alias analysis proved this Div/Mod
  // cannot trap (divisor excludes 0, and excludes -1 or the dividend
  // excludes INT64_MIN) or this Load/Store is provably in bounds (handle
  // provably one allocation + constant index in range). Backends may then
  // skip the guard; the pass pipeline may treat the op as pure/hoistable.
  bool nt{};
  std::vector<uint32_t> args{}; // Call: argument vregs, parameter order
  // source location: byte offset of the AST token that produced this
  // instruction (NONE = synthesized by a pass or a builder helper).
  // Survives every in-place rewrite (passes copy Instr wholesale);
  // instructions CREATED by passes keep NONE. The C/asm backends and the
  // IR printers translate it into line:col comments via SrcMap.
  uint32_t src{NONE};
};
struct Block {
  uint32_t begin{}, end{}; // instrs [begin,end)
};
struct Fn {
  std::vector<Instr> code{};
  std::vector<Block> blocks{};
  std::vector<uint32_t> succ{};               // 2 per block
  std::vector<std::vector<uint32_t>> preds{}; // ascending source order
  uint32_t nvars{};                           // original (pre-SSA) vregs
  uint32_t nparams{};                         // vregs 0..nparams-1 = params
  std::vector<std::string_view> var_names{};  // base names per vreg
  std::vector<uint32_t> version_base{};       // SSA versions: base(v)
  std::vector<uint32_t> version_no{};         // SSA versions: subscript
  // Arrays provably local to this function (C backend): one entry per
  // non-escaping constant-length Alloc — `root` is the Alloc's dst (the C
  // array is named after it), `len` the element count, `vregs` every vreg
  // holding that handle (the Alloc dst + transitive Copies; destruction's
  // copy fusion may rename the defining instr to any of them). Filled by
  // analyze_stack_allocs on the SSA form; vreg ids survive destruction.
  struct StkArr {
    uint32_t root{};
    val_t len{};
    std::vector<uint32_t> vregs{};
  };
  std::vector<StkArr> stk_arrays{};

  // set by tail-call elimination when THIS fn is a merged pure-tail SCC:
  // the number of merged members. Param 0 (sel) then provably holds a
  // member index in [0, tce_sel) at every call site (tailrec rewrites them
  // all itself), which is what lets the countdown fold prove sel in {0,1}
  // for two members. 0 = not a tailrec product.
  uint32_t tce_sel{};

  // ---- register allocation (C + bytecode backends) ------------------
  // Physical home of each vreg AFTER SSA destruction. Empty = identity
  // (one slot per vreg, the "SSA-like" emission). When register allocation
  // is on, slot_of maps every vreg to a color of the interference graph
  // (vregs with disjoint live ranges share a slot), nslots is the color
  // count, and the first nparams slots are the parameters (the VM writes
  // call arguments into slots 0..nargs-1). No spilling is ever needed:
  // the C backend has unlimited locals and the VM has unlimited slots.
  std::vector<uint32_t> slot_of{};
  uint32_t nslots{};

  constexpr uint32_t nblocks() const { return (uint32_t)blocks.size(); }
  constexpr uint32_t succ0(uint32_t b) const { return succ[2 * b]; }
  constexpr uint32_t succ1(uint32_t b) const { return succ[2 * b + 1]; }
  constexpr uint32_t base_of(uint32_t v) const {
    return v < nvars ? v : version_base[v - nvars];
  }
  constexpr uint32_t vern_of(uint32_t v) const {
    return v < nvars ? 0 : version_no[v - nvars];
  }
  constexpr void recompute_preds() {
    preds.assign(nblocks(), {});
    for (uint32_t b = 0; b < nblocks(); ++b) {
      if (succ0(b) != NONE)
        preds[succ0(b)].push_back(b);
      if (succ1(b) != NONE)
        preds[succ1(b)].push_back(b);
    }
  }
};

// Slot view used by the C + bytecode backends. Identity (empty slot_of,
// i.e. --no-regalloc or a never-run allocator) keeps one named local per
// vreg; a filled slot_of is the interference-coloring home (see 11z).
constexpr uint32_t slot_id(const Fn &fn, uint32_t v) {
  if (v == NONE)
    return 0;
  if (fn.slot_of.empty())
    return v;
  return v < fn.slot_of.size() ? fn.slot_of[v] : 0;
}
constexpr uint32_t slot_count(const Fn &fn) {
  if (fn.slot_of.empty())
    return fn.nvars + (uint32_t)fn.version_base.size();
  return fn.nslots ? fn.nslots : (fn.nvars + (uint32_t)fn.version_base.size());
}

// fresh anonymous vreg (used by destruction temps, unroll copies, inlining)
constexpr uint32_t new_anon_vreg(Fn &fn, uint32_t base) {
  uint32_t v = fn.nvars + (uint32_t)fn.version_base.size();
  fn.version_base.push_back(base);
  fn.version_no.push_back(0x10000 + (uint32_t)fn.version_base.size());
  return v;
}
