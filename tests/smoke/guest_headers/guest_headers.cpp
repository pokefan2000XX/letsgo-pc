// Compile-only check that guest (decomp) code can use the Nintendo SDK headers
// from nnheaders together with the C and C++ standard libraries, on the same
// target and ABI as the original game. Built with cmake/guest-aarch64.cmake.
#include <nn/account.h>
#include <nn/fs.h>
#include <nn/hid.h>
#include <nn/mem.h>
#include <nn/oe.h>
#include <nn/os.h>
#include <nn/socket.h>
#include <nn/time.h>
#include <nn/util.h>
#include <nn/vi.h>
#include <nvn/nvn.h>
#include <nvn/nvn_FuncPtrBase.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

static_assert(sizeof(long) == 8, "guest is LP64");
static_assert(sizeof(std::string) == 24, "libc++ (std::__1) string layout, same as the game's");
static_assert(sizeof(std::vector<int>) == 24, "libc++ vector layout");
static_assert(std::atomic<int>::is_always_lock_free, "inline atomics");

extern "C" int guest_headers_probe(const char* s, nn::os::MutexType* m) {
    nn::os::LockMutex(m);
    int n = static_cast<int>(std::strlen(s));
    nn::os::UnlockMutex(m);
    return n;
}
