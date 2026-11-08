// TU: the native backends' codegen — the C emitter and the x86-64 emitter
// (chordal register allocation via mjz_lib). Both are compiled exactly
// once; the rest of the program sees the light emit_c.hpp/emit_x64.hpp.
#define MJZ_MULTI_CPP
#include "minijz/back/emit_c_impl.hpp"
#include "minijz/back/emit_x64_impl.hpp"
