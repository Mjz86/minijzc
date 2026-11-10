// === src/mjz_lib/alllib_import.hpp =========================================
// The bridge to the String repo (mjz_lib/), for BOTH of its layouts:
//
//   old:  mjz_lib/alllib_import.hpp           (clone root)
//   new:  mjz_lib/mjz_lib/alllib_import.hpp   (nested one level)
//
// graph_lib.hpp reaches this file with the quoted include
// "../mjz_lib/alllib_import.hpp", which standard search rule #1 resolves
// from the INCLUDER'S directory (src/minijz/) to this file -- no -I flags,
// no MSVC include-stack rule, no -Wmicrosoft-include. The self-adapting
// __has_include below then picks whichever layout the clone actually has,
// so a fresh `git clone https://github.com/Mjz86/String mjz_lib` works with
// plain g++, CMake, clang++ and clang-cl alike (checked in on purpose: no
// configure-time generation, no symlinks -- Windows-unfriendly).
#pragma once
#include <atomic> // the shim below needs std::memory_order declared first
// --- the libc++ std::atomic_ref shim ---------------------------------------
// std::atomic_ref was REMOVED from the final C++20 text: libstdc++ and the
// MSVC STL ship it anyway, libc++ never has. Upstream mjz_lib's threads
// module references it unconditionally (and the dependency chain
// graph -> byte_str -> threads drags it into every import), so ANY
// libc++-based toolchain -- LLVM-MinGW, MSYS2 CLANG64, clang++
// -stdlib=libc++ -- failed to compile the import at all (the "it doesn't
// build on Windows" report). This shim stands in ONLY for libc++ without
// the real thing, with plain non-atomic semantics: the same illusion
// upstream's cx_atomic_ref_t itself plays for compile time. It is sound
// here because the compiler never shares one of those refcounted objects
// across threads (each compile / each fuzz worker is single-threaded and
// owns its instances). The gate turns itself off if libc++ ever ships the
// real one (__cpp_lib_atomic_ref).
#if defined(_LIBCPP_VERSION) && !defined(__cpp_lib_atomic_ref)
namespace std {
template <class T> class atomic_ref {
  T *p_;

public:
  using value_type = T;
  static constexpr size_t required_alignment = alignof(T);
  static constexpr bool is_always_lock_free = true;
  explicit atomic_ref(T &t) noexcept : p_(&t) {}
  atomic_ref(const atomic_ref &) noexcept = default;
  atomic_ref &operator=(const atomic_ref &) noexcept = default;
  void operator=(T t) const noexcept { *p_ = t; }
  operator T() const noexcept { return *p_; }
  bool is_lock_free() const noexcept { return true; }
  T load(memory_order = memory_order_seq_cst) const noexcept { return *p_; }
  void store(T t, memory_order = memory_order_seq_cst) const noexcept {
    *p_ = t;
  }
  T exchange(T t, memory_order = memory_order_seq_cst) const noexcept {
    T r = *p_;
    *p_ = t;
    return r;
  }
  bool compare_exchange_weak(T &e, T d, memory_order,
                             memory_order) const noexcept {
    if (*p_ == e) {
      *p_ = d;
      return true;
    }
    e = *p_;
    return false;
  }
  bool compare_exchange_strong(T &e, T d, memory_order m1,
                               memory_order m2) const noexcept {
    return compare_exchange_weak(e, d, m1, m2);
  }
  T fetch_add(T d, memory_order = memory_order_seq_cst) const noexcept {
    T r = *p_;
    *p_ = T(r + d);
    return r;
  }
  T fetch_sub(T d, memory_order = memory_order_seq_cst) const noexcept {
    T r = *p_;
    *p_ = T(r - d);
    return r;
  }
  T fetch_and(T d, memory_order = memory_order_seq_cst) const noexcept {
    T r = *p_;
    *p_ = T(r & d);
    return r;
  }
  T fetch_or(T d, memory_order = memory_order_seq_cst) const noexcept {
    T r = *p_;
    *p_ = T(r | d);
    return r;
  }
  T fetch_xor(T d, memory_order = memory_order_seq_cst) const noexcept {
    T r = *p_;
    *p_ = T(r ^ d);
    return r;
  }
  // upstream's mutex support surfaces these; the compiler never calls them
  // (single-threaded compiles), so they observe-and-return instead of block
  void wait(T expected, memory_order = memory_order_seq_cst) const noexcept {
    (void)expected;
  }
  void notify_one() const noexcept {}
  void notify_all() const noexcept {}
};
} // namespace std
#endif // _LIBCPP_VERSION && !__cpp_lib_atomic_ref
#if __has_include("../../mjz_lib/alllib_import.hpp")
#include "../../mjz_lib/alllib_import.hpp"
#else
#include "../../mjz_lib/mjz_lib/alllib_import.hpp"
#endif
