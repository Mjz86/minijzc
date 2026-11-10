#pragma once
// === diag.hpp ========================================
// located diagnostics: line:col + caret + source context for compile
// errors, and located runtime traps. LIGHT (config only) so every app
// surface (CLI, debugger, repl) renders errors through ONE code path.
//
//   prog.mjz:3:9: error: use of undeclared variable 'zzz'
//      3 |   print zzz + 1;
//        |         ^~~

#include "../config.hpp"

#include <cstdio>
#include <string>
#include <string_view>

// the token starting at `pos` (identifier/number chars, else one char);
// used for the caret width and to quote what was actually found
inline std::string_view diag_token(std::string_view src, uint32_t pos) {
  if (pos >= src.size())
    return {};
  auto alpha = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           (c >= '0' && c <= '9');
  };
  uint32_t n = 0;
  while (pos + n < src.size() && alpha(src[pos + n]))
    ++n;
  if (n == 0)
    n = 1; // punctuation / operator: one caret
  return src.substr(pos, n);
}

// enrich the parser's generic category strings with the offending token
inline std::string diag_message(std::string_view src, const char *what,
                                uint32_t pos) {
  std::string m = what ? what : "";
  std::string_view tok = diag_token(src, pos);
  auto has = [&](const char *needle) {
    return m.find(needle) != std::string::npos;
  };
  if (m == "use of undeclared variable" && !tok.empty())
    m += " '" + std::string(tok) + "'";
  else if (m == "not a function (variables cannot be called)" && !tok.empty())
    m += " '" + std::string(tok) + "'";
  else if ((m == "duplicate declaration" || m == "duplicate parameter name" ||
            m == "duplicate function name") &&
           !tok.empty())
    m += " '" + std::string(tok) + "'";
  else if (m == "unknown character" && !tok.empty())
    m += " '" + std::string(tok) + "'";
  else if (m.rfind("expected", 0) == 0) { // "expected X" -> ", found Y"
    if (tok.empty() || pos >= src.size())
      m += " before end of input";
    else
      m += ", found '" + std::string(tok) + "'";
  }
  return m;
}

// the full located render. `file` may be empty (stdin / -e source). `pos`
// may be NONE/0-with-no-location (internal errors): plain one-liner then.
inline void print_compile_error(std::string_view src, const char *what,
                                uint32_t pos, const char *file) {
  const char *at = (file && *file) ? file : "<input>";
  std::string msg = diag_message(src, what, pos);
  if (pos == NONE || pos == 0 || pos > src.size()) {
    std::printf("%s: error: %s\n", at, msg.c_str());
    return;
  }
  // line containing pos (1-based)
  uint32_t lo = 0, line = 1;
  for (uint32_t i = 0; i < pos && i < src.size(); ++i)
    if (src[i] == '\n') {
      lo = i + 1;
      ++line;
    }
  uint32_t hi = lo;
  while (hi < src.size() && src[hi] != '\n')
    ++hi;
  uint32_t col = pos - lo + 1;
  std::string_view row = src.substr(lo, hi - lo);
  std::string_view tok = diag_token(src, pos);
  uint32_t width = tok.empty() ? 1 : (uint32_t)tok.size();
  std::printf("%s:%u:%u: error: %s\n", at, (unsigned)line, (unsigned)col,
              msg.c_str());
  std::printf("%5u | %.*s\n", (unsigned)line, (int)row.size(), row.data());
  std::printf("      | ");
  for (uint32_t k = 1; k < col; ++k)
    std::printf(" ");
  for (uint32_t k = 0; k < width; ++k)
    std::printf("^");
  std::printf("\n");
}

// located runtime trap: "division by zero at 7:3" style. Returns the same
// verdict as plain TRAP printing so callers can keep their contract.
inline void print_trap_at(std::string_view src, uint32_t pos,
                          const char *kind) {
  if (pos == NONE || pos == 0 || pos > src.size()) {
    std::printf("TRAP (%s)\n", kind);
    return;
  }
  uint32_t lo = 0, line = 1;
  for (uint32_t i = 0; i < pos && i < src.size(); ++i)
    if (src[i] == '\n') {
      lo = i + 1;
      ++line;
    }
  uint32_t col = pos - lo + 1;
  std::printf("TRAP (%s at %u:%u)\n", kind, (unsigned)line, (unsigned)col);
}
