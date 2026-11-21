/*MIT License

Copyright (c) 2026 Mjz86

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef MJZ_SRC_OPT_base_FILE_
#define MJZ_SRC_OPT_base_FILE_
#include "../graph/csr.hpp"

MJZ_EXPORT
//
namespace mjz::opt_ns {

// THE FAMILY. The optimization kernels, generic over index space --
// the graph_ns treatment applied to the compiler's own components
// (the founding rule: an ALGORITHM must not depend on the way any
// client works). Every family speaks the same conventions: uintlen_t
// ids, version_t-templated entry points, CSR rows and event streams
// as the only inputs, pure tables as the only outputs, MJZ_CX_FN
// noexcept, uintlen_t(-1) as NONE. The policies that drive the
// kernels (what is a root, what is eligible, what a transfer does)
// stay with the clients -- the std::sort-comparator rule: the
// algorithm is generic, the semantics is a callback.

inline constexpr uintlen_t none_v = uintlen_t(-1);

};     // namespace mjz::opt_ns
#endif // MJZ_SRC_OPT_base_FILE_
