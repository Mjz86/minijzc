// TU 3/7: scalar passes
#include "minijz/opt/scalar/fold.hpp"
#include "minijz/opt/scalar/copyprop.hpp"
#include "minijz/opt/scalar/canon.hpp"
#include "minijz/opt/scalar/cse.hpp"
#include "minijz/opt/scalar/unreach.hpp"
#include "minijz/opt/scalar/splice.hpp"
#include "minijz/opt/scalar/dce.hpp"
#include "minijz/opt/scalar/ifconv.hpp"
namespace minijz_tu_marker { int scalar = 0; }
