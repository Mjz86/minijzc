// TU: the runner stack -- the whole execution layer compiled ONCE (the
// program.hpp/compile_impl.hpp idiom applied to the runner layer; see the
// *_impl.hpp headers). Every app TU links against these instead of
// re-parsing, re-instantiating and re-emitting ~1.2 MB of COMDAT
// interpreter/VM/printer text apiece.
#include "../minijz/back/emit_plan_impl.hpp"
#include "../minijz/back/interp_ssa_impl.hpp"
#include "../minijz/back/vm_impl.hpp"
#include "../minijz/back/vm_step_impl.hpp"
#include "../minijz/prog/printers_impl.hpp"
#include "../minijz/runtime/astrun_impl.hpp"
