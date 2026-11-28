#include "xmem.h"

#include "xcptkrn/mmu.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __func__ << ':' << __LINE__ \
                      << ": check failed: " #condition << '\n'; \
            return false; \
        } \
    } while (false)

    constexpr ULONGLONG alignment_attribute(const unsigned exponent) {
        return static_cast<ULONGLONG>(exponent) << 24;
    }

    volatile std::uint64_t* value_at(const PVOID address) {
        return static_cast<volatile std::uint64_t*>(address);
    }

    bool default_allocation() {
        constexpr SIZE_T alignment = 0x10000;
        const auto address = XMemAllocDefault(257, alignment_attribute(16));
        CHECK(address);
        CHECK(!(reinterpret_cast<std::uintptr_t>(address) % alignment));
        value_at(address)[0] = 0x123456789abcdef0;
        CHECK(value_at(address)[0] == 0x123456789abcdef0);
        XMemFreeDefault(address, alignment_attribute(16));

        const auto hooked = XMemAlloc(32, 0);
        CHECK(hooked);
        XMemFree(hooked, 0);

        const auto foreign = HeapAlloc(GetProcessHeap(), 0, 32);
        CHECK(foreign);
        SetLastError(ERROR_SUCCESS);
        XMemFreeDefault(foreign, 0);
        CHECK(GetLastError() == ERROR_INVALID_ADDRESS);
        CHECK(HeapValidate(GetProcessHeap(), 0, foreign));
        CHECK(HeapFree(GetProcessHeap(), 0, foreign));
        return true;
    }

    bool physical_mapping() {
        std::array<ULONG_PTR, 2> pages{};
        ULONG_PTR count = pages.size();
        CHECK(XMemAllocatePhysicalPages(0x180, &count, pages.data()));
        CHECK(count == pages.size());
        CHECK(pages[0] && pages[1] && pages[0] != pages[1]);

        const auto region = XMemVirtualAlloc(
            nullptr, 2 * xcompat::krn::mmu::page_size,
            MEM_64K_PAGES | MEM_RESERVE, 0, PAGE_READWRITE);
        const auto alias = XMemVirtualAlloc(
            nullptr, xcompat::krn::mmu::page_size,
            MEM_64K_PAGES | MEM_RESERVE, 0, PAGE_READWRITE);
        CHECK(region && alias);

        CHECK(XMemMapPhysicalPages(region, pages.size(), pages.data()) == region);
        value_at(region)[0] = 0x1111222233334444;
        value_at(static_cast<std::uint8_t*>(region) + xcompat::krn::mmu::page_size)[0] =
            0xaaaabbbbccccdddd;

        CHECK(XMemMapPhysicalPages(alias, 1, pages.data()) == alias);
        CHECK(value_at(alias)[0] == 0x1111222233334444);
        value_at(alias)[0] = 0x5555666677778888;
        CHECK(value_at(region)[0] == 0x5555666677778888);

        CHECK(XMemMapPhysicalPages(region, 1, pages.data() + 1) == region);
        CHECK(value_at(region)[0] == 0xaaaabbbbccccdddd);
        CHECK(value_at(alias)[0] == 0x5555666677778888);

        CHECK(XMemMapPhysicalPages(region, pages.size(), nullptr) == region);
        CHECK(XMemMapPhysicalPages(alias, 1, nullptr) == alias);
        CHECK(XMemMapPhysicalPages(alias, 1, nullptr) == alias);
        CHECK(XMemFreePhysicalPages(pages.size(), pages.data()));
        CHECK(xcompat::krn::mmu::release(region));
        CHECK(xcompat::krn::mmu::release(alias));
        return true;
    }

    bool scatter_mapping() {
        std::array<ULONG_PTR, 3> pages{};
        ULONG_PTR count = pages.size();
        CHECK(XMemAllocatePhysicalPages(0x380, &count, pages.data()));

        const auto region = XMemVirtualAlloc(
            nullptr, pages.size() * xcompat::krn::mmu::page_size,
            MEM_64K_PAGES | MEM_RESERVE, 0, PAGE_READWRITE);
        CHECK(region);

        auto base = static_cast<std::uint8_t*>(region);
        std::array<PVOID, 3> addresses{
            base + 2 * xcompat::krn::mmu::page_size,
            base,
            base + xcompat::krn::mmu::page_size,
        };
        CHECK(XMemMapPhysicalPagesScatter(addresses.data(), addresses.size(), pages.data()));
        for (std::size_t index = 0; index < addresses.size(); ++index)
            value_at(addresses[index])[0] = index + 1;

        CHECK(value_at(base)[0] == 2);
        CHECK(value_at(base + xcompat::krn::mmu::page_size)[0] == 3);
        CHECK(value_at(base + 2 * xcompat::krn::mmu::page_size)[0] == 1);

        std::array<ULONG_PTR, 3> replacements{pages[0], 0, pages[2]};
        CHECK(XMemMapPhysicalPagesScatter(
            addresses.data(), addresses.size(), replacements.data()));
        CHECK(XMemMapPhysicalPagesScatter(addresses.data(), addresses.size(), nullptr));
        CHECK(XMemMapPhysicalPagesScatter(addresses.data(), addresses.size(), nullptr));

        CHECK(XMemFreePhysicalPages(pages.size(), pages.data()));
        CHECK(xcompat::krn::mmu::release(region));
        return true;
    }

    bool virtual_and_statistics() {
        const auto memory = XMemVirtualAlloc(
            nullptr, 4096, MEM_RESERVE | MEM_COMMIT, 0, PAGE_READWRITE);
        CHECK(memory);
        *static_cast<volatile std::uint32_t*>(memory) = 0x12345678;
        CHECK(*static_cast<volatile std::uint32_t*>(memory) == 0x12345678);
        CHECK(VirtualFree(memory, 0, MEM_RELEASE));

        XMEM_WORKING_SET_STATISTICS before{};
        XMEM_WORKING_SET_STATISTICS after{};
        CHECK(XMemGetWorkingSetStatistics(0, &before));
        CHECK(before.TitleLimit);
        CHECK(!before.ToolsLimit);
        CHECK(!XMemIsToolsMemAvailable());
        CHECK(XMemTransferMemory(1, 1));
        CHECK(XMemGetWorkingSetStatistics(0, &after));
        CHECK(after.TitleLimit + 2 * 1024 * 1024 == before.TitleLimit);
        CHECK(!XMemTransferMemory(2, 1));
        return true;
    }

    bool invalid_parameters() {
        ULONG_PTR count = 1;
        CHECK(!XMemAllocatePhysicalPages(0, nullptr, nullptr));
        CHECK(!XMemAllocatePhysicalPages(0, &count, nullptr));
        CHECK(!XMemFreePhysicalPages(1, nullptr));
        CHECK(!XMemMapPhysicalPages(nullptr, 1, nullptr));
        CHECK(!XMemMapPhysicalPagesScatter(nullptr, 1, nullptr));
        CHECK(!XMemGetWorkingSetStatistics(0, nullptr));

        count = 0;
        CHECK(XMemAllocatePhysicalPages(0, &count, nullptr));
        CHECK(XMemFreePhysicalPages(0, nullptr));
        CHECK(XMemMapPhysicalPagesScatter(nullptr, 0, nullptr));
        return true;
    }

    struct test_case {
        std::string_view name;
        bool (*run)();
    };

    constexpr std::array tests{
        test_case{"default-allocation", default_allocation},
        test_case{"physical-mapping", physical_mapping},
        test_case{"scatter-mapping", scatter_mapping},
        test_case{"virtual-and-statistics", virtual_and_statistics},
        test_case{"invalid-parameters", invalid_parameters},
    };
}

int main(const int argc, const char* const* argv) {
    if (argc != 2) {
        std::cerr << "usage: xcompat-xmem-tests <test-name>\n";
        return 2;
    }

    const std::string_view requested = argv[1];
    for (const auto& test : tests) {
        if (test.name != requested)
            continue;

        if (!test.run())
            return 1;

        std::cout << test.name << " passed\n";
        return 0;
    }

    std::cerr << "unknown test: " << requested << '\n';
    return 2;
}
