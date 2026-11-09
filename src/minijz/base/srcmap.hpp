#pragma once
// === srcmap.hpp ========================================
// byte offset -> line:col resolution over the source text. LIGHT header:
// usable from the printers and both native emitters. Runtime-only use is
// fine (nothing on the constexpr path constructs one).

#include "../config.hpp"

#include <cstdio>
#include <string_view>
#include <vector>

// Source-location comments in the emitted output ("a rough idea of what
// happens where"): the front end records the byte offset of the token that
// STARTS each expression/statement (Instr::src), and the backends translate
// offsets to 1-based line:column pairs through this map.
struct SrcMap {
  std::string_view src{};
  std::vector<uint32_t> starts{}; // byte offset of each line's first char
  explicit SrcMap(std::string_view s) : src(s) {
    if (!src.empty()) {
      starts.push_back(0);
      for (uint32_t i = 0; i < src.size(); ++i)
        if (src[i] == '\n')
          starts.push_back(i + 1);
    }
  }
  SrcMap() = default;
  bool ok() const { return !starts.empty(); }
  // 1-based line holding byte offset p (0 = "no source" answer)
  uint32_t line(uint32_t p) const {
    if (!ok() || p > src.size())
      return 0;
    uint32_t lo = 0, hi = (uint32_t)starts.size() - 1;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo + 1) / 2;
      if (starts[mid] <= p)
        lo = mid;
      else
        hi = mid - 1;
    }
    return lo + 1;
  }
  // 1-based column (byte count from the line start + 1)
  uint32_t col(uint32_t p) const {
    uint32_t l = line(p);
    if (l == 0)
      return 0;
    return p - starts[l - 1] + 1;
  }
  // "line:col" text for offset p into buf; false when p carries no location
  bool at(uint32_t p, char *buf, size_t cap) const {
    if (!ok() || p == NONE || p > src.size())
      return false;
    std::snprintf(buf, cap, "%u:%u", (unsigned)line(p), (unsigned)col(p));
    return true;
  }
};
