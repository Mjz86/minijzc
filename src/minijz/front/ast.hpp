#pragma once
// === ast.hpp ========================================
// the AST.

#include "../config.hpp"

// ============================================================================
// 3. AST (arena of nodes, indices instead of pointers — constexpr friendly)
// ============================================================================
enum class Ek : uint8_t {
  Num,
  Var,
  Neg,
  Not,
  Bin,
  And,
  Or,
  Alloc,
  Load,
  Len,
  Input,
  Call
};
enum class Bk : uint8_t { Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne };

struct Expr {
  Ek k{};
  Bk b{};
  val_t num{};
  uint32_t var{};
  uint32_t l{}, r{}; // child indices into exprs arena
  uint32_t pos{};    // byte offset of the token starting this expression
                     // (0 = unset; the parser always sets it)
};
enum class Sk : uint8_t {
  Let,
  Assign,
  Print,
  If,
  While,
  Seq,
  StoreA,
  Ret,
  Brk,
  Cont
};
struct Stmt {
  Sk k{};
  uint32_t var{};
  uint32_t e{};      // primary expr
  uint32_t a{}, b{}; // Seq: seq_kids index in a; If/While bodies
  uint32_t pos{};    // byte offset of the token starting this statement
};

struct FnDef { // a top-level `fn name(params) { body }`
  std::string_view name{};
  std::vector<uint32_t> params{}; // AST var ids (one scope level)
  uint32_t body{};                // Seq stmt index of the body block
  bool defined{false};
};
struct Ast {
  std::vector<Expr> es;
  std::vector<Stmt> ss;
  std::vector<FnDef> fns{};                    // parsed defs (hoist order)
  std::vector<std::string_view> hoisted_fns{}; // names, pre-scanned for calls
  // each Seq owns its OWN children list (a shared arena would interleave
  // nested blocks' children into the parent's [begin,begin+count) window)
  std::vector<std::vector<uint32_t>> seq_kids;
  std::vector<std::string_view> names; // var id -> name
  // scope table for the parser
  struct Scp {
    std::string_view name{};
    uint32_t id{};
  };
  std::vector<Scp> scope;
  std::vector<uint32_t> scope_marks;
  constexpr uint32_t new_var(std::string_view n) {
    // duplicate only within the CURRENT block scope
    uint32_t start = scope_marks.empty() ? 0 : scope_marks.back();
    for (uint32_t i = (uint32_t)scope.size(); i-- > start;)
      if (scope[i].name == n)
        return 0xDEAD'FFFF;
    // shadowing of an outer variable creates a FRESH id (distinct storage)
    names.push_back(n);
    scope.push_back({n, (uint32_t)(names.size() - 1)});
    return (uint32_t)(names.size() - 1);
  }
};

struct ParseError {
  uint32_t pos{};
  const char *what{};
};
struct Parsed {
  bool ok{};
  ParseError err{};
  Ast ast{};
  uint32_t root{};   // Seq index
  bool uses_input{}; // the program contains input()
};
