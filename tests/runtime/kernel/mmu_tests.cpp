#include "xcptkrn/mmu.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
    using namespace xcompat::krn;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __func__ << ':' << __LINE__ \
                      << ": check failed: " #condition << '\n'; \
            return false; \
        } \
    } while (false)

    volatile std::uint64_t* value_at(const hy::ptr address) {
        return static_cast<volatile std::uint64_t*>(address.u);
    }

    bool query_memory(const hy::ptr address, MEMORY_BASIC_INFORMATION& info) {
        SIZE_T returned{};
        const auto status = NtQueryVirtualMemory(
            NtCurrentProcess(), address.u, MemoryBasicInformation,
            &info, sizeof(info), &returned
        );

        if (!NT_SUCCESS(status)) {
            std::cerr << "NtQueryVirtualMemory failed for " << address.u
                      << " with status 0x" << std::hex
                      << static_cast<unsigned long>(status) << std::dec << '\n';
            return false;
        }

        return returned >= sizeof(info);
    }

    bool physical_single() {
        std::size_t count = 1;
        mmu::pfn_t page{};

        CHECK(mmu::alloc_phy(count, &page));
        CHECK(count == 1);
        CHECK(page != mmu::bad_pfn);
        CHECK(mmu::free_phy(1, &page));

        const auto region = mmu::reserve(nullptr, mmu::page_size);
        CHECK(region);
        CHECK(!mmu::map(region, page));
        CHECK(mmu::release(region));
        return true;
    }

    bool physical_batch() {
        constexpr std::size_t requested = 130;
        std::vector<mmu::pfn_t> pages(requested);
        std::size_t count = requested;

        CHECK(mmu::alloc_phy(count, pages.data()));
        CHECK(count == requested);

        std::unordered_set<mmu::pfn_t> unique;
        unique.reserve(pages.size());
        for (const auto page : pages) {
            CHECK(page != mmu::bad_pfn);
            CHECK(unique.insert(page).second);
        }

        CHECK(mmu::free_phy(pages.size(), pages.data()));
        return true;
    }

    bool physical_count_semantics() {
        std::size_t count{};
        CHECK(mmu::alloc_phy(count, nullptr));
        CHECK(count == 0);
        CHECK(mmu::free_phy(0, nullptr));

        count = 3;
        std::array<mmu::pfn_t, 3> pages{};
        CHECK(mmu::alloc_phy(count, pages.data()));
        CHECK(count == pages.size());
        CHECK(mmu::free_phy(count, pages.data()));
        return true;
    }

    bool physical_repeated() {
        for (std::size_t iteration = 0; iteration < 128; ++iteration) {
            std::size_t count = 4;
            std::array<mmu::pfn_t, 4> pages{};
            CHECK(mmu::alloc_phy(count, pages.data()));
            CHECK(count == pages.size());
            CHECK(mmu::free_phy(count, pages.data()));
        }

        return true;
    }

    bool physical_double_free() {
        std::size_t count = 1;
        mmu::pfn_t page{};
        CHECK(mmu::alloc_phy(count, &page));
        CHECK(mmu::free_phy(1, &page));
        CHECK(!mmu::free_phy(1, &page));

        count = 1;
        CHECK(mmu::alloc_phy(count, &page));
        const std::array duplicate{page, page};
        CHECK(!mmu::free_phy(duplicate.size(), duplicate.data()));
        return true;
    }

    bool reservation_basic() {
        const auto single = mmu::reserve(nullptr, mmu::page_size);
        CHECK(single);
        CHECK(single.i % mmu::page_size == 0);
        CHECK(mmu::release(single));

        const auto multiple = mmu::reserve(nullptr, 4 * mmu::page_size);
        CHECK(multiple);
        CHECK(mmu::release(multiple));
        return true;
    }

    bool reservation_rounding() {
        constexpr std::size_t expected = 3 * mmu::page_size;
        const auto region = mmu::reserve(nullptr, 2 * mmu::page_size + 1);
        CHECK(region);

        MEMORY_BASIC_INFORMATION info{};
        CHECK(query_memory(region, info));
        CHECK(info.BaseAddress == region.u);
        CHECK(info.AllocationBase == region.u);
        CHECK(info.State == MEM_RESERVE);
        CHECK(info.RegionSize == expected);

        CHECK(mmu::release(region));
        return true;
    }

    bool reservation_explicit_conflict() {
        const auto region = mmu::reserve(nullptr, 2 * mmu::page_size);
        CHECK(region);
        CHECK(!mmu::reserve(region, mmu::page_size));
        CHECK(mmu::release(region));

        const auto explicit_region = mmu::reserve(region, 2 * mmu::page_size);
        CHECK(explicit_region == region);
        CHECK(mmu::release(explicit_region));
        return true;
    }

    bool reservation_invalid() {
        CHECK(!mmu::reserve(nullptr, 0));
        CHECK(!mmu::reserve(nullptr, std::numeric_limits<std::size_t>::max()));
        CHECK(!mmu::release(nullptr));

        const auto region = mmu::reserve(nullptr, 2 * mmu::page_size);
        CHECK(region);
        CHECK(!mmu::release(region + mmu::page_size));
        CHECK(mmu::release(region));
        return true;
    }

    bool mapping_single() {
        std::size_t count = 1;
        mmu::pfn_t page{};
        CHECK(mmu::alloc_phy(count, &page));

        const auto region = mmu::reserve(nullptr, mmu::page_size);
        CHECK(region);
        CHECK(mmu::map(region, page));

        auto* const first = reinterpret_cast<volatile std::uint8_t*>(region.u);
        first[0] = 0x5a;
        first[mmu::page_size - 1] = 0xa5;
        CHECK(first[0] == 0x5a);
        CHECK(first[mmu::page_size - 1] == 0xa5);

        CHECK(mmu::unmap(region));
        CHECK(mmu::free_phy(1, &page));
        CHECK(mmu::release(region));
        return true;
    }

    bool mapping_range() {
        constexpr std::size_t page_count = 5;
        std::array<mmu::pfn_t, page_count> pages{};
        std::size_t count = pages.size();
        CHECK(mmu::alloc_phy(count, pages.data()));

        const auto region = mmu::reserve(nullptr, page_count * mmu::page_size);
        CHECK(region);

        for (std::size_t i = 0; i < page_count; ++i)
            CHECK(mmu::map(region + i * mmu::page_size, pages[i]));

        for (std::size_t i = 0; i < page_count; ++i)
            *value_at(region + i * mmu::page_size) = 0x1000 + i;

        for (std::size_t i = 0; i < page_count; ++i)
            CHECK(*value_at(region + i * mmu::page_size) == 0x1000 + i);

        *value_at(region) = 0xfedcba9876543210ull;
        for (std::size_t i = 1; i < page_count; ++i)
            CHECK(*value_at(region + i * mmu::page_size) == 0x1000 + i);

        for (std::size_t i = 0; i < page_count; ++i)
            CHECK(mmu::unmap(region + i * mmu::page_size));

        CHECK(mmu::free_phy(pages.size(), pages.data()));
        CHECK(mmu::release(region));
        return true;
    }

    bool mapping_validation() {
        std::size_t count = 2;
        std::array<mmu::pfn_t, 2> pages{};
        CHECK(mmu::alloc_phy(count, pages.data()));

        const auto region = mmu::reserve(nullptr, mmu::page_size);
        CHECK(region);
        CHECK(!mmu::map(region + 1, pages[0]));
        CHECK(!mmu::map(region + mmu::page_size, pages[0]));
        CHECK(!mmu::map(nullptr, pages[0]));
        CHECK(!mmu::map(region, mmu::bad_pfn));
        CHECK(!mmu::unmap(region));

        CHECK(mmu::map(region, pages[0]));
        *value_at(region) = 0x1111222233334444ull;
        CHECK(!mmu::map(region, pages[1]));
        CHECK(*value_at(region) == 0x1111222233334444ull);
        CHECK(!mmu::unmap(region + 1));
        CHECK(mmu::unmap(region));

        CHECK(mmu::map(region, pages[1]));
        CHECK(*value_at(region) == 0);
        CHECK(mmu::unmap(region));

        CHECK(mmu::free_phy(pages.size(), pages.data()));
        CHECK(mmu::release(region));
        return true;
    }

    bool aliasing_persistence() {
        std::size_t count = 1;
        mmu::pfn_t page{};
        CHECK(mmu::alloc_phy(count, &page));

        const auto region = mmu::reserve(nullptr, 2 * mmu::page_size);
        CHECK(region);
        const auto alias = region + mmu::page_size;

        CHECK(mmu::map(region, page));
        CHECK(mmu::map(alias, page));

        *value_at(region) = 0x0123456789abcdefull;
        CHECK(*value_at(alias) == 0x0123456789abcdefull);
        *value_at(alias) = 0xfedcba9876543210ull;
        CHECK(*value_at(region) == 0xfedcba9876543210ull);

        CHECK(mmu::unmap(region));
        CHECK(*value_at(alias) == 0xfedcba9876543210ull);
        CHECK(mmu::map(region, page));
        CHECK(*value_at(region) == 0xfedcba9876543210ull);

        CHECK(mmu::unmap(region));
        CHECK(mmu::unmap(alias));
        CHECK(mmu::map(region, page));
        CHECK(*value_at(region) == 0xfedcba9876543210ull);

        CHECK(mmu::unmap(region));
        CHECK(mmu::free_phy(1, &page));
        CHECK(mmu::release(region));
        return true;
    }

    bool physical_lifetime() {
        std::size_t count = 1;
        mmu::pfn_t unmapped{};
        CHECK(mmu::alloc_phy(count, &unmapped));
        CHECK(mmu::free_phy(1, &unmapped));

        const auto rejected = mmu::reserve(nullptr, mmu::page_size);
        CHECK(rejected);
        CHECK(!mmu::map(rejected, unmapped));
        CHECK(mmu::release(rejected));

        count = 1;
        mmu::pfn_t mapped{};
        CHECK(mmu::alloc_phy(count, &mapped));
        const auto region = mmu::reserve(nullptr, 2 * mmu::page_size);
        CHECK(region);
        CHECK(mmu::map(region, mapped));
        *value_at(region) = 0x55aa55aa55aa55aaull;

        CHECK(mmu::free_phy(1, &mapped));
        CHECK(*value_at(region) == 0x55aa55aa55aa55aaull);
        CHECK(!mmu::map(region + mmu::page_size, mapped));
        CHECK(mmu::unmap(region));
        CHECK(!mmu::map(region, mapped));
        CHECK(mmu::release(region));
        return true;
    }

    bool placeholder_subdivision() {
        constexpr std::size_t page_count = 6;
        const auto region = mmu::reserve(nullptr, page_count * mmu::page_size);
        CHECK(region);

        std::size_t count = 1;
        mmu::pfn_t page{};
        CHECK(mmu::alloc_phy(count, &page));

        constexpr std::array<std::size_t, 7> indices{2, 1, 4, 3, 0, 5, 2};
        for (const auto index : indices) {
            const auto address = region + index * mmu::page_size;
            CHECK(mmu::map(address, page));
            *value_at(address) = index;
            CHECK(*value_at(address) == index);
            CHECK(mmu::unmap(address));
        }

        CHECK(mmu::release(region));
        for (std::size_t i = 0; i < page_count; ++i) {
            MEMORY_BASIC_INFORMATION info{};
            CHECK(query_memory(region + i * mmu::page_size, info));
            CHECK(info.State == MEM_FREE);
        }

        CHECK(mmu::free_phy(1, &page));
        return true;
    }

    bool placeholder_active_release() {
        std::size_t count = 1;
        mmu::pfn_t page{};
        CHECK(mmu::alloc_phy(count, &page));

        const auto region = mmu::reserve(nullptr, 3 * mmu::page_size);
        CHECK(region);
        const auto middle = region + mmu::page_size;
        CHECK(mmu::map(middle, page));
        CHECK(!mmu::release(region));

        *value_at(middle) = 0xaabbccddeeff0011ull;
        CHECK(*value_at(middle) == 0xaabbccddeeff0011ull);
        CHECK(mmu::unmap(middle));
        CHECK(mmu::release(region));
        CHECK(mmu::free_phy(1, &page));
        return true;
    }

    bool bulk_contracts() {
        std::size_t count{};
        CHECK(mmu::alloc_phy(count, nullptr));
        CHECK(mmu::free_phy(0, nullptr));

        const mmu::pfn_t zero = mmu::bad_pfn;
        CHECK(!mmu::free_phy(1, &zero));

        count = 1;
        mmu::pfn_t page{};
        CHECK(mmu::alloc_phy(count, &page));
        const std::array pages{page, mmu::bad_pfn};
        CHECK(!mmu::free_phy(pages.size(), pages.data()));

        const auto region = mmu::reserve(nullptr, mmu::page_size);
        CHECK(region);
        CHECK(!mmu::map(region, page));
        CHECK(mmu::release(region));
        return true;
    }

    bool concurrent_physical() {
        constexpr std::size_t thread_count = 8;
        constexpr std::size_t iterations = 64;
        std::atomic<bool> successful{true};
        std::array<std::thread, thread_count> threads;

        for (std::size_t thread = 0; thread < thread_count; ++thread) {
            threads[thread] = std::thread([&successful] {
                for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                    std::size_t count = 3;
                    std::array<mmu::pfn_t, 3> pages{};
                    if (!mmu::alloc_phy(count, pages.data()) || count != pages.size()) {
                        successful.store(false, std::memory_order_relaxed);
                        return;
                    }

                    if (!mmu::free_phy(pages.size(), pages.data())) {
                        successful.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }

        for (auto& thread : threads)
            thread.join();

        CHECK(successful.load(std::memory_order_relaxed));
        return true;
    }

    bool concurrent_reservations() {
        constexpr std::size_t thread_count = 8;
        constexpr std::size_t iterations = 32;
        std::atomic<bool> successful{true};
        std::array<std::thread, thread_count> threads;

        for (std::size_t thread = 0; thread < thread_count; ++thread) {
            threads[thread] = std::thread([thread, &successful] {
                for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                    std::size_t count = 1;
                    mmu::pfn_t page{};
                    if (!mmu::alloc_phy(count, &page)) {
                        successful.store(false, std::memory_order_relaxed);
                        return;
                    }

                    const auto region = mmu::reserve(nullptr, 3 * mmu::page_size);
                    if (!region) {
                        mmu::free_phy(1, &page);
                        successful.store(false, std::memory_order_relaxed);
                        return;
                    }

                    const auto address = region + (iteration % 3) * mmu::page_size;
                    const auto expected = (thread << 32) | iteration;
                    const bool mapped = mmu::map(address, page);
                    if (mapped) {
                        *value_at(address) = expected;
                        if (*value_at(address) != expected || !mmu::unmap(address))
                            successful.store(false, std::memory_order_relaxed);
                    } else {
                        successful.store(false, std::memory_order_relaxed);
                    }

                    if (!mmu::release(region) || !mmu::free_phy(1, &page)) {
                        successful.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }

        for (auto& thread : threads)
            thread.join();

        CHECK(successful.load(std::memory_order_relaxed));
        return true;
    }

    bool concurrent_aliases() {
        constexpr std::size_t thread_count = 8;
        std::size_t count = 1;
        mmu::pfn_t page{};
        CHECK(mmu::alloc_phy(count, &page));

        std::array<hy::ptr, thread_count> regions{};
        for (auto& region : regions) {
            region = mmu::reserve(nullptr, mmu::page_size);
            CHECK(region);
        }

        std::array<bool, thread_count> mapped{};
        std::atomic<bool> successful{true};
        std::barrier sync(static_cast<std::ptrdiff_t>(thread_count));
        std::array<std::thread, thread_count> threads;
        constexpr std::uint64_t expected = 0x0ddc0ffeebadf00dull;

        for (std::size_t index = 0; index < thread_count; ++index) {
            threads[index] = std::thread([&, index] {
                mapped[index] = mmu::map(regions[index], page);
                if (!mapped[index])
                    successful.store(false, std::memory_order_relaxed);
                sync.arrive_and_wait();

                if (successful.load(std::memory_order_relaxed) && index == 0)
                    *value_at(regions[index]) = expected;
                sync.arrive_and_wait();

                if (successful.load(std::memory_order_relaxed)
                    && *value_at(regions[index]) != expected) {
                    successful.store(false, std::memory_order_relaxed);
                }
                sync.arrive_and_wait();

                if (mapped[index] && !mmu::unmap(regions[index]))
                    successful.store(false, std::memory_order_relaxed);
            });
        }

        for (auto& thread : threads)
            thread.join();

        for (const auto region : regions)
            CHECK(mmu::release(region));
        CHECK(mmu::free_phy(1, &page));
        CHECK(successful.load(std::memory_order_relaxed));
        return true;
    }

    bool large_batch() {
        constexpr std::size_t requested = 512;
        std::vector<mmu::pfn_t> pages(requested);
        std::size_t count = pages.size();
        CHECK(mmu::alloc_phy(count, pages.data()));
        CHECK(count == requested);

        std::unordered_set<mmu::pfn_t> unique;
        unique.reserve(pages.size());
        for (const auto page : pages)
            CHECK(page && unique.insert(page).second);

        CHECK(mmu::free_phy(pages.size(), pages.data()));
        return true;
    }

    bool repeated_cycles() {
        for (std::size_t iteration = 0; iteration < 128; ++iteration) {
            std::size_t count = 1;
            mmu::pfn_t page{};
            CHECK(mmu::alloc_phy(count, &page));

            const auto region = mmu::reserve(nullptr, 4 * mmu::page_size);
            CHECK(region);
            const auto address = region + (iteration % 4) * mmu::page_size;
            CHECK(mmu::map(address, page));
            *value_at(address) = iteration;
            CHECK(*value_at(address) == iteration);
            CHECK(mmu::unmap(address));
            CHECK(mmu::release(region));
            CHECK(mmu::free_phy(1, &page));
        }

        return true;
    }

    bool sparse_mappings() {
        constexpr std::size_t reservation_pages = 128;
        constexpr std::array<std::size_t, 8> indices{0, 1, 17, 63, 64, 95, 126, 127};
        std::array<mmu::pfn_t, indices.size()> pages{};
        std::size_t count = pages.size();
        CHECK(mmu::alloc_phy(count, pages.data()));

        const auto region = mmu::reserve(nullptr, reservation_pages * mmu::page_size);
        CHECK(region);
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const auto address = region + indices[i] * mmu::page_size;
            CHECK(mmu::map(address, pages[i]));
            *value_at(address) = indices[i] + 1;
        }

        for (std::size_t i = 0; i < indices.size(); ++i) {
            const auto address = region + indices[i] * mmu::page_size;
            CHECK(*value_at(address) == indices[i] + 1);
            CHECK(mmu::unmap(address));
        }

        CHECK(mmu::release(region));
        CHECK(mmu::free_phy(pages.size(), pages.data()));
        return true;
    }

    struct test_case {
        std::string_view name;
        bool (*run)();
    };

    constexpr std::array tests{
        test_case{"physical-single", physical_single},
        test_case{"physical-batch", physical_batch},
        test_case{"physical-count-semantics", physical_count_semantics},
        test_case{"physical-repeated", physical_repeated},
        test_case{"physical-double-free", physical_double_free},
        test_case{"reservation-basic", reservation_basic},
        test_case{"reservation-rounding", reservation_rounding},
        test_case{"reservation-explicit-conflict", reservation_explicit_conflict},
        test_case{"reservation-invalid", reservation_invalid},
        test_case{"mapping-single", mapping_single},
        test_case{"mapping-range", mapping_range},
        test_case{"mapping-validation", mapping_validation},
        test_case{"aliasing-persistence", aliasing_persistence},
        test_case{"physical-lifetime", physical_lifetime},
        test_case{"placeholder-subdivision", placeholder_subdivision},
        test_case{"placeholder-active-release", placeholder_active_release},
        test_case{"bulk-contracts", bulk_contracts},
        test_case{"concurrent-physical", concurrent_physical},
        test_case{"concurrent-reservations", concurrent_reservations},
        test_case{"concurrent-aliases", concurrent_aliases},
        test_case{"large-batch", large_batch},
        test_case{"repeated-cycles", repeated_cycles},
        test_case{"sparse-mappings", sparse_mappings},
    };
}

int main(const int argc, const char* const* argv) {
    if (argc != 2) {
        std::cerr << "usage: xcompat-kernel-mmu-tests <test-name>\n";
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
