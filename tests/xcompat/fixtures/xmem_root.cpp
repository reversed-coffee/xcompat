#include "xmem.h"
#include <cstdint>

extern "C" DWORD WINAPI entry(void* peb) {
    (void)peb;
    auto* memory = static_cast<volatile std::uint64_t*>(XMemAlloc(4096, 0));
    if (!memory) return 81;
    *memory = 0x55aa33cc77ee1199ull;
    if (*memory != 0x55aa33cc77ee1199ull) return 82;
    XMemFree(const_cast<std::uint64_t*>(memory), 0);
    return 0;
}
