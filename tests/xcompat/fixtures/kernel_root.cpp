#include "xcptkrn/mmu.hpp"
#include <cstdint>

extern "C" DWORD WINAPI entry(void* peb) {
    using namespace xcompat::krn::mmu;
    (void)peb;
    std::size_t count = 1;
    pfn_t page{};
    if (!alloc_phy(count, &page) || count != 1) return 91;
    const auto first = reserve(nullptr, page_size);
    const auto second = reserve(nullptr, page_size);
    if (!first || !second) return 92;
    if (!map(first, page) || !map(second, page)) return 93;
    *static_cast<volatile std::uint64_t*>(first.u) = 0x37a9c5e18b42d6f0ull;
    if (*static_cast<volatile std::uint64_t*>(second.u) != 0x37a9c5e18b42d6f0ull) return 94;
    if (!unmap(first) || !unmap(second) || !release(first) || !release(second)) return 95;
    return free_phy(1, &page) ? 0 : 96;
}
