// TU: the native backends' codegen — the C emitter and the x86-64 emitter
// (chordal register allocation via mjz_lib), plus the register-allocation
// audit (the allocator's read-only second consumer). All compiled exactly
// once; the rest of the program sees the light emit_c.hpp/emit_x64.hpp/
// regaudit.hpp.
#define MJZ_MULTI_CPP
#include "../minijz/back/emit_c_impl.hpp"
#include "../minijz/back/emit_x64_impl.hpp"
#include "../minijz/back/regaudit_impl.hpp"
