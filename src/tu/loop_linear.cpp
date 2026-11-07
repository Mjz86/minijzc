// TU 4/7: loop passes + linear forms (divmagic brings mjz_lib bignums)
#include "minijz/opt/loop/licm.hpp"
#include "minijz/opt/loop/unroll.hpp"
#include "minijz/opt/loop/lcf.hpp"
#include "minijz/opt/loop/lcf_rt.hpp"
#include "minijz/opt/linear/reassoc.hpp"
#include "minijz/opt/linear/divmagic.hpp"
namespace minijz_tu_marker { int loop_linear = 0; }
