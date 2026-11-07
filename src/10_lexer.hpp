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