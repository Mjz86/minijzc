// TU 6/7: backends (regalloc coloring: mjz_lib) + runtime + prog/runners
#include "minijz/back/vm.hpp"
#include "minijz/back/emit_c.hpp"
#include "minijz/back/emit_x64.hpp"
#include "minijz/back/bytecode.hpp"
#include "minijz/back/stack_alloc.hpp"
#include "minijz/runtime/astrun.hpp"
#include "minijz/prog/runners.hpp"
#include "minijz/base/native_run.hpp"
namespace minijz_tu_back {
[[maybe_unused]] auto _a1 = &print_fn; // link anchor (defined in prog/runners.hpp)
}
