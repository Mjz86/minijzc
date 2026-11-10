// TU: the composite pipeline — parse/build/SSA/optimize/destruct. The
// HEAVY TU (mjz_lib graphs + every pass); everything else links to it
// through the light declaration in prog/program.hpp.
#define MJZ_MULTI_CPP // (also set by CMake; keeps the TU self-sufficient)
#include "../minijz/prog/compile_impl.hpp"
