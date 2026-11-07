// ============================================================================
// MiniJZ compiler — §2 lexer, §3 AST, §4 recursive-descent parser
// ----------------------------------------------------------------------------
// Ordered-include header: this file is a slice of the original single
// translation unit and relies on being included after its predecessors
// (see src/minijz.hpp for the canonical order). Do not include it
// directly in an arbitrary order.
// ============================================================================
#pragma once

// ============================================================================
// 2. lexer
// ============================================================================
enum class Tk : uint8_t {
  End, Id, Num,
  KwLet, KwPrint, KwIf, KwElse, KwWhile, KwFn, KwRet, KwBreak, KwCont,
  Plus, Minus, Star, Slash, Percent, Comma,
  Lt, Le, Gt, Ge, EqEq, Ne, AndAnd, OrOr, Not,
  Assign, LP, RP, LB, RB, LBr, RBr, Semi
};
struct Tok {
  Tk k{};
  uint32_t pos{};     // offset into source (for diagnostics)
  val_t num{};
  uint32_t lo{}, hi{}; // identifier slice [lo,hi)
};

struct LexError {
  uint32_t pos{};
  const char *what{};
};

struct Lexed {
  std::vector<Tok> toks{};
  uint32_t nvars{};                       // vars discovered (parse assigns ids)
  bool ok{};
  LexError err{};
};

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool is_alpha_(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

constexpr Lexed lex(std::string_view src) {
  Lexed L{};
  uint32_t i = 0;
  const uint32_t n = (uint32_t)src.size();
  auto err = [&](uint32_t p, const char *w) {
    L.ok = false;
    L.err = {p, w};
    return L;
  };
  while (i < n) {
    char c = src[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
    if (c == '/' && i + 1 < n && src[i + 1] == '/') {   // line comment
      while (i < n && src[i] != '\n') ++i;
      continue;
    }
    uint32_t p = i;
    if (is_digit(c)) {
      val_t v = 0;
      while (i < n && is_digit(src[i])) {
        v = (val_t)((uint64_t)v * 10u + (uint64_t)(src[i] - '0'));
        ++i;
      }
      L.toks.push_back({Tk::Num, p, v, p, i});
      continue;
    }
    if (is_alpha_(c)) {
      while (i < n && (is_alpha_(src[i]) || is_digit(src[i]))) ++i;
      std::string_view w = src.substr(p, i - p);
      Tk k = Tk::Id;
      if (w == "let") k = Tk::KwLet;
      else if (w == "print") k = Tk::KwPrint;
      else if (w == "if") k = Tk::KwIf;
      else if (w == "else") k = Tk::KwElse;
      else if (w == "while") k = Tk::KwWhile;
      else if (w == "fn") k = Tk::KwFn;
      else if (w == "return") k = Tk::KwRet;
      else if (w == "break") k = Tk::KwBreak;
      else if (w == "continue") k = Tk::KwCont;
      L.toks.push_back({k, p, 0, p, i});
      continue;
    }
    ++i;
    Tk k{};
    if (c == '+') k = Tk::Plus;
    else if (c == '-') k = Tk::Minus;
    else if (c == '*') k = Tk::Star;
    else if (c == '/') k = Tk::Slash;
    else if (c == '%') k = Tk::Percent;
    else if (c == '=') {
      if (i < n && src[i] == '=') { ++i; k = Tk::EqEq; }
      else k = Tk::Assign;
    }
    else if (c == '<') {
      if (i < n && src[i] == '=') { ++i; k = Tk::Le; }
      else k = Tk::Lt;
    }
    else if (c == '>') {
      if (i < n && src[i] == '=') { ++i; k = Tk::Ge; }
      else k = Tk::Gt;
    }
    else if (c == '!' && i < n && src[i] == '=') { ++i; k = Tk::Ne; }
    else if (c == '!' && !(i < n && src[i] == '=')) k = Tk::Not;
    else if (c == '&' && i < n && src[i] == '&') { ++i; k = Tk::AndAnd; }
    else if (c == '|' && i < n && src[i] == '|') { ++i; k = Tk::OrOr; }
    else if (c == '(') k = Tk::LP;
    else if (c == ')') k = Tk::RP;
    else if (c == '{') k = Tk::LB;
    else if (c == '}') k = Tk::RB;
    else if (c == '[') k = Tk::LBr;
    else if (c == ']') k = Tk::RBr;
    else if (c == ';') k = Tk::Semi;
    else if (c == ',') k = Tk::Comma;
    else return err(p, "unknown character");
    L.toks.push_back({k, p, 0, p, i});
  }
  L.toks.push_back({Tk::End, n, 0, n, n});
  L.ok = true;
  return L;
}

// ============================================================================
// 3. AST (arena of nodes, indices instead of pointers — constexpr friendly)
// ============================================================================
enum class Ek : uint8_t { Num, Var, Neg, Not, Bin, And, Or, Alloc, Load, Len, Input, Call };
enum class Bk : uint8_t { Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne };

struct Expr {
  Ek k{};
  Bk b{};
  val_t num{};
  uint32_t var{};
  uint32_t l{}, r{};   // child indices into exprs arena
};
enum class Sk : uint8_t { Let, Assign, Print, If, While, Seq, StoreA, Ret, Brk, Cont };
struct Stmt {
  Sk k{};
  uint32_t var{};
  uint32_t e{};                    // primary expr
  uint32_t a{}, b{};               // Seq: seq_kids index in a; If/While bodies
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
  std::vector<FnDef> fns{};                   // parsed defs (hoist order)
  std::vector<std::string_view> hoisted_fns{}; // names, pre-scanned for calls
  // each Seq owns its OWN children list (a shared arena would interleave
  // nested blocks' children into the parent's [begin,begin+count) window)
  std::vector<std::vector<uint32_t>> seq_kids;
  std::vector<std::string_view> names; // var id -> name
  // scope table for the parser
  struct Scp { std::string_view name{}; uint32_t id{}; };
  std::vector<Scp> scope;
  std::vector<uint32_t> scope_marks;
  constexpr uint32_t new_var(std::string_view n) {
    // duplicate only within the CURRENT block scope
    uint32_t start = scope_marks.empty() ? 0 : scope_marks.back();
    for (uint32_t i = (uint32_t)scope.size(); i-- > start;)
      if (scope[i].name == n) return 0xDEAD'FFFF;
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
  uint32_t root{}; // Seq index
  bool uses_input{}; // the program contains input()
};

// ============================================================================
// 4. recursive-descent parser
// ============================================================================
struct Parser {
  const Lexed &L;
  std::string_view src;
  Ast &a;
  uint32_t ti{}; // token index
  ParseError err{};
  bool ok{true};
  bool in_fn{false}; // 'return' is only legal inside a fn body
  int loop_depth{};  // 'break'/'continue' are only legal inside a loop body

  constexpr Parser(const Lexed &L_, std::string_view s, Ast &a_)
      : L(L_), src(s), a(a_) {}

  constexpr void fail(uint32_t pos, const char *w) {
    if (ok) { ok = false; err = {pos, w}; }
  }
  constexpr const Tok &peek() const { return L.toks[ti]; }
  constexpr const Tok &eat() { return L.toks[ti++]; }
  constexpr bool accept(Tk k) {
    if (peek().k == k) { ++ti; return true; }
    return false;
  }
  constexpr bool expect(Tk k, const char *w) {
    if (!accept(k)) { fail(peek().pos, w); return false; }
    return true;
  }

  constexpr uint32_t find_var(std::string_view n, uint32_t pos) {
    for (auto it = a.scope.rbegin(); it != a.scope.rend(); ++it)
      if (it->name == n) return it->id;
    fail(pos, "use of undeclared variable");
    return 0xFFFF'FFFF;
  }

  // expr := logic
  constexpr uint32_t expr() {
    uint32_t lhs = rel();
    if (!ok) return 0;
    for (;;) {
      if (peek().k == Tk::AndAnd) {
        ++ti;
        uint32_t rhs = rel();
        if (!ok) return 0;
        uint32_t idx = (uint32_t)a.es.size();
        a.es.push_back({Ek::And, {}, 0, 0, lhs, rhs});
        lhs = idx;
      } else if (peek().k == Tk::OrOr) {
        ++ti;
        uint32_t rhs = rel();
        if (!ok) return 0;
        uint32_t idx = (uint32_t)a.es.size();
        a.es.push_back({Ek::Or, {}, 0, 0, lhs, rhs});
        lhs = idx;
      } else return lhs;
    }
  }
  // rel := add (cmp add)?
  constexpr uint32_t rel() {
    uint32_t lhs = add();
    if (!ok) return 0;
    Bk b{};
    switch (peek().k) {
    case Tk::Lt: b = Bk::Lt; break;
    case Tk::Le: b = Bk::Le; break;
    case Tk::Gt: b = Bk::Gt; break;
    case Tk::Ge: b = Bk::Ge; break;
    case Tk::EqEq: b = Bk::Eq; break;
    case Tk::Ne: b = Bk::Ne; break;
    default: return lhs;
    }
    ++ti;
    uint32_t rhs = add();
    if (!ok) return 0;
    uint32_t i = (uint32_t)a.es.size();
    a.es.push_back({Ek::Bin, b, 0, 0, lhs, rhs});
    return i;
  }
  // add := mul (('+'|'-') mul)*
  constexpr uint32_t add() {
    uint32_t lhs = mul();
    if (!ok) return 0;
    for (;;) {
      Bk b{};
      if (peek().k == Tk::Plus) b = Bk::Add;
      else if (peek().k == Tk::Minus) b = Bk::Sub;
      else return lhs;
      ++ti;
      uint32_t rhs = mul();
      if (!ok) return 0;
      uint32_t idx = (uint32_t)a.es.size();
      a.es.push_back({Ek::Bin, b, 0, 0, lhs, rhs});
      lhs = idx;
    }
  }
  // mul := unary (('*'|'/'|'%') unary)*
  constexpr uint32_t mul() {
    uint32_t lhs = unary();
    if (!ok) return 0;
    for (;;) {
      Bk b{};
      if (peek().k == Tk::Star) b = Bk::Mul;
      else if (peek().k == Tk::Slash) b = Bk::Div;
      else if (peek().k == Tk::Percent) b = Bk::Mod;
      else return lhs;
      ++ti;
      uint32_t rhs = unary();
      if (!ok) return 0;
      uint32_t idx = (uint32_t)a.es.size();
      a.es.push_back({Ek::Bin, b, 0, 0, lhs, rhs});
      lhs = idx;
    }
  }
  constexpr uint32_t unary() {
    if (accept(Tk::Minus)) {
      uint32_t c = unary();
      uint32_t i = (uint32_t)a.es.size();
      a.es.push_back({Ek::Neg, {}, 0, 0, c, 0});
      return i;
    }
    if (accept(Tk::Not)) {
      uint32_t c = unary();
      uint32_t i = (uint32_t)a.es.size();
      a.es.push_back({Ek::Not, {}, 0, 0, c, 0});
      return i;
    }
    return primary();
  }
  // postfix `[expr]` indexing on any primary: a[i], a[i][j], array(3)[0]...
  // each index is Ek::Load(base = handle expr, idx = index expr)
  constexpr uint32_t postfix(uint32_t base) {
    uint32_t lhs = base;
    while (peek().k == Tk::LBr) {
      ++ti;
      uint32_t ix = expr();
      if (!ok) return 0;
      if (!expect(Tk::RBr, "expected ']'")) return 0;
      uint32_t i = (uint32_t)a.es.size();
      a.es.push_back({Ek::Load, {}, 0, 0, lhs, ix});
      lhs = i;
    }
    return lhs;
  }
  constexpr uint32_t primary() {
    if (peek().k == Tk::Num) {
      val_t v = eat().num;
      uint32_t i = (uint32_t)a.es.size();
      a.es.push_back({Ek::Num, {}, v, 0, 0, 0});
      return postfix(i);
    }
    if (peek().k == Tk::Id) {
      Tok t = eat();
      std::string_view w = src.substr(t.lo, t.hi - t.lo);
      // contextual builtins: only special when followed by '('. a variable
      // named array/len/input stays usable (`let array = 3; print array;`)
      if (w == "array" && peek().k == Tk::LP) { // array(size)
        ++ti;
        uint32_t sz = expr();
        if (!ok) return 0;
        if (!expect(Tk::RP, "expected ')' after array size")) return 0;
        uint32_t i = (uint32_t)a.es.size();
        a.es.push_back({Ek::Alloc, {}, 0, 0, sz, 0});
        return postfix(i);
      }
      if (w == "len" && peek().k == Tk::LP) { // len(handle)
        ++ti;
        uint32_t h = expr();
        if (!ok) return 0;
        if (!expect(Tk::RP, "expected ')' after len argument")) return 0;
        uint32_t i = (uint32_t)a.es.size();
        a.es.push_back({Ek::Len, {}, 0, 0, h, 0});
        return postfix(i);
      }
      if (w == "input" && peek().k == Tk::LP) { // input()
        ++ti;
        if (!expect(Tk::RP, "expected '()'")) return 0;
        uint32_t i = (uint32_t)a.es.size();
        a.es.push_back({Ek::Input, {}, 0, 0, 0, 0});
        return postfix(i);
      }
      if (peek().k == Tk::LP) { // user function call: name(args...)
        uint32_t fi = find_hoisted(w);
        if (fi != NONE) {
          ++ti;
          std::vector<uint32_t> kids{};
          if (peek().k != Tk::RP) {
            for (;;) {
              uint32_t ae = expr();
              if (!ok) return 0;
              kids.push_back(ae);
              if (accept(Tk::Comma)) continue;
              break;
            }
          }
          if (!expect(Tk::RP, "expected ')' after arguments")) return 0;
          if (kids.size() > 6) {
            fail(t.pos, "too many arguments (max 6: SysV integer regs)");
            return 0;
          }
          a.seq_kids.push_back(std::move(kids));
          uint32_t i = (uint32_t)a.es.size();
          a.es.push_back({Ek::Call, {}, 0, fi,
                          (uint32_t)(a.seq_kids.size() - 1), 0});
          return postfix(i);
        }
      }
      uint32_t v = find_var(w, t.pos);
      if (!ok) return 0;
      if (peek().k == Tk::LP && v != 0xFFFF'FFFF) {
        fail(t.pos, "not a function (variables cannot be called)");
        return 0;
      }
      uint32_t i = (uint32_t)a.es.size();
      a.es.push_back({Ek::Var, {}, 0, v, 0, 0});
      return postfix(i);
    }
    if (accept(Tk::LP)) {
      uint32_t e = expr();
      if (!ok) return 0;
      if (!expect(Tk::RP, "expected ')'")) return 0;
      return postfix(e);
    }
    fail(peek().pos, "expected expression");
    return 0;
  }

  // block := '{' stmt* '}'  (child Seq)
  constexpr uint32_t block() {
    if (!expect(Tk::LB, "expected '{'")) return 0;
    a.scope_marks.push_back((uint32_t)a.scope.size());
    uint32_t s = stmts_until_rb();
    a.scope.resize(a.scope_marks.back());
    a.scope_marks.pop_back();
    if (!ok) return 0;
    if (!expect(Tk::RB, "expected '}'")) return 0;
    return s;
  }
  constexpr uint32_t stmts_until_rb() {
    std::vector<uint32_t> kids{};
    while (peek().k != Tk::RB && peek().k != Tk::End && ok) {
      uint32_t s = stmt();
      if (!ok) return 0;
      kids.push_back(s);
    }
    a.seq_kids.push_back(std::move(kids));
    uint32_t i = (uint32_t)a.ss.size();
    a.ss.push_back(
        {Sk::Seq, 0, 0, (uint32_t)(a.seq_kids.size() - 1), 0});
    return i;
  }
  constexpr uint32_t stmt() {
    switch (peek().k) {
    case Tk::KwRet: {
      ++ti;
      if (!in_fn) {
        fail(peek().pos, "'return' outside of a function");
        return 0;
      }
      uint32_t e = expr();
      if (!ok) return 0;
      if (!expect(Tk::Semi, "expected ';'")) return 0;
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({Sk::Ret, 0, e, 0, 0});
      return i;
    }
    case Tk::KwLet: {
      ++ti;
      if (peek().k != Tk::Id) { fail(peek().pos, "expected name after let"); return 0; }
      Tok t = eat();
      std::string_view n = src.substr(t.lo, t.hi - t.lo);
      uint32_t id = a.new_var(n);
      if (id == 0xDEAD'FFFF) { fail(t.pos, "duplicate declaration"); return 0; }
      if (!expect(Tk::Assign, "expected '='")) return 0;
      uint32_t e = expr();
      if (!ok) return 0;
      if (!expect(Tk::Semi, "expected ';'")) return 0;
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({Sk::Let, id, e, 0, 0});
      return i;
    }
    case Tk::KwPrint: {
      ++ti;
      uint32_t e = expr();
      if (!ok) return 0;
      if (!expect(Tk::Semi, "expected ';'")) return 0;
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({Sk::Print, 0, e, 0, 0});
      return i;
    }
    case Tk::KwIf: {
      ++ti;
      if (!expect(Tk::LP, "expected '('")) return 0;
      uint32_t c = expr();
      if (!ok) return 0;
      if (!expect(Tk::RP, "expected ')'")) return 0;
      uint32_t then = block();
      if (!ok) return 0;
      uint32_t els = 0;
      if (accept(Tk::KwElse)) {
        els = block();
        if (!ok) return 0;
      } else {
        // no else clause: an EMPTY Seq arm (must not alias the then arm!)
        a.seq_kids.push_back({});
        els = (uint32_t)a.ss.size();
        a.ss.push_back(
            {Sk::Seq, 0, 0, (uint32_t)(a.seq_kids.size() - 1), 0});
      }
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({Sk::If, 0, c, then, els});
      return i;
    }
    case Tk::KwBreak:
    case Tk::KwCont: {
      bool is_break = (peek().k == Tk::KwBreak);
      ++ti;
      if (loop_depth <= 0) {
        fail(peek().pos, is_break ? "'break' outside of a loop"
                                  : "'continue' outside of a loop");
        return 0;
      }
      if (!expect(Tk::Semi, "expected ';'")) return 0;
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({is_break ? Sk::Brk : Sk::Cont, 0, 0, 0, 0});
      return i;
    }
    case Tk::KwWhile: {
      ++ti;
      if (!expect(Tk::LP, "expected '('")) return 0;
      uint32_t c = expr();
      if (!ok) return 0;
      if (!expect(Tk::RP, "expected ')'")) return 0;
      ++loop_depth;
      uint32_t body = block();
      --loop_depth;
      if (!ok) return 0;
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({Sk::While, 0, c, body, 0});
      return i;
    }
    case Tk::LB:
      return block();
    case Tk::Id: {
      Tok t = eat();
      std::string_view w = src.substr(t.lo, t.hi - t.lo);
      uint32_t v = find_var(w, t.pos);
      if (!ok) return 0;
      if (peek().k == Tk::LBr) { // array store: a[i]...[k] = expr;
        std::vector<uint32_t> idx{};
        while (accept(Tk::LBr)) {
          uint32_t e = expr();
          if (!ok) return 0;
          if (!expect(Tk::RBr, "expected ']'")) return 0;
          idx.push_back(e);
        }
        if (!expect(Tk::Assign, "expected '=' in array store")) return 0;
        uint32_t val = expr();
        if (!ok) return 0;
        if (!expect(Tk::Semi, "expected ';'")) return 0;
        a.seq_kids.push_back(std::move(idx));
        uint32_t i = (uint32_t)a.ss.size();
        // var = base array var; a = seq_kids slot of index exprs; b = value
        a.ss.push_back(
            {Sk::StoreA, v, 0, (uint32_t)(a.seq_kids.size() - 1), val});
        return i;
      }
      if (!expect(Tk::Assign, "expected '=' in assignment")) return 0;
      uint32_t e = expr();
      if (!ok) return 0;
      if (!expect(Tk::Semi, "expected ';'")) return 0;
      uint32_t i = (uint32_t)a.ss.size();
      a.ss.push_back({Sk::Assign, v, e, 0, 0});
      return i;
    }
    default:
      fail(peek().pos, "expected statement");
      return 0;
    }
  }
  // pre-scan: collect top-level fn names so calls may reference functions
  // defined LATER in the file (mutual recursion needs it). Braces only occur
  // as block delimiters in this language, so brace depth finds "top level".
  constexpr void hoist_fns() {
    uint32_t depth = 0;
    for (size_t k = 0; k < L.toks.size(); ++k) {
      Tk t = L.toks[k].k;
      if (t == Tk::LB) ++depth;
      else if (t == Tk::RB && depth) --depth;
      else if (t == Tk::KwFn && depth == 0) {
        if (k + 1 < L.toks.size() && L.toks[k + 1].k == Tk::Id) {
          const Tok &nt = L.toks[k + 1];
          std::string_view n = src.substr(nt.lo, nt.hi - nt.lo);
          for (const auto &h : a.hoisted_fns)
            if (h == n) { fail(nt.pos, "duplicate function name"); return; }
          a.hoisted_fns.push_back(n);
        }
      }
    }
  }
  constexpr uint32_t find_hoisted(std::string_view n) const {
    for (size_t i = 0; i < a.hoisted_fns.size(); ++i)
      if (a.hoisted_fns[i] == n) return (uint32_t)i;
    return NONE;
  }
  // fn name(params) { body } -- params form one scope level (body may shadow)
  constexpr bool parse_fn() {
    (void)eat(); // 'fn'
    if (peek().k != Tk::Id) {
      fail(peek().pos, "expected function name after 'fn'");
      return false;
    }
    Tok t = eat();
    std::string_view n = src.substr(t.lo, t.hi - t.lo);
    uint32_t idx = find_hoisted(n);
    if (idx == NONE) { fail(t.pos, "internal: fn not hoisted"); return false; }
    if (a.fns[idx].defined) {
      fail(t.pos, "duplicate function definition");
      return false;
    }
    if (!expect(Tk::LP, "expected '(' after function name")) return false;
    std::vector<uint32_t> params{};
    a.scope_marks.push_back((uint32_t)a.scope.size());
    if (!accept(Tk::RP)) {
      for (;;) {
        if (peek().k != Tk::Id) {
          fail(peek().pos, "expected parameter name");
          return false;
        }
        Tok pt = eat();
        std::string_view pn = src.substr(pt.lo, pt.hi - pt.lo);
        uint32_t id = a.new_var(pn);
        if (id == 0xDEAD'FFFF) {
          fail(pt.pos, "duplicate parameter name");
          return false;
        }
        params.push_back(id);
        if (accept(Tk::Comma)) continue;
        break;
      }
      if (!expect(Tk::RP, "expected ')' after parameters")) return false;
    }
    bool saved = in_fn;
    in_fn = true;
    uint32_t body = block();
    in_fn = saved;
    a.scope.resize(a.scope_marks.back());
    a.scope_marks.pop_back();
    if (!ok) return false;
    a.fns[idx] = FnDef{n, params, body, true};
    return true;
  }
  constexpr uint32_t program() {
    hoist_fns();
    if (!ok) return 0;
    a.fns.assign(a.hoisted_fns.size(), FnDef{});
    std::vector<uint32_t> kids{};
    while (peek().k != Tk::End && ok) {
      if (peek().k == Tk::KwFn) {
        if (!parse_fn()) return 0;
        continue;
      }
      uint32_t s = stmt();
      if (!ok) return 0;
      kids.push_back(s);
    }
    if (!ok) return 0;
    for (const FnDef &f : a.fns)
      if (!f.defined) { fail(0, "internal: hoisted fn never defined"); return 0; }
    if (peek().k != Tk::End) { fail(peek().pos, "unexpected token"); return 0; }
    a.seq_kids.push_back(std::move(kids));
    uint32_t i = (uint32_t)a.ss.size();
    a.ss.push_back(
        {Sk::Seq, 0, 0, (uint32_t)(a.seq_kids.size() - 1), 0});
    return i;
  }
};

constexpr Parsed parse(std::string_view src) {
  Lexed L = lex(src);
  Parsed P{};
  if (!L.ok) { P.err = {L.err.pos, L.err.what}; return P; }
  P.ast = Ast{};
  Parser ps(L, src, P.ast);
  P.ok = ps.ok;
  P.root = ps.program();
  if (!ps.ok) { P.ok = false; P.err = ps.err; }
  if (P.ast.names.empty() && P.ok) {
    // no variables at all is fine (print-only programs)
  }
  for (const Expr &e : P.ast.es)
    if (e.k == Ek::Input) { P.uses_input = true; break; }
  return P;
}
