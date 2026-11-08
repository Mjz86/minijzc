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
#if __has_include("../../mjz_lib/alllib_import.hpp")
#include "../../mjz_lib/alllib_import.hpp"
#else
#include "../../mjz_lib/mjz_lib/alllib_import.hpp"
#endif
