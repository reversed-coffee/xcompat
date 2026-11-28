#include "xcptkrn/mmu.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xcompat::krn::mmu {
    static std::unordered_map<pfn_t, physical_page> physical_pages{};
    static std::map<void*, reservation> reserved_regions{};

    static std::mutex mutex{};
    static pfn_t next_pfn = 1;

    static constexpr std::size_t batch_size = 64;

    // Locate the reservation and corresponding page index
    static reservation* find_region(hy::ptr base, std::size_t& index) {
        auto it = reserved_regions.upper_bound(base.u);
        if (it == reserved_regions.begin()) return nullptr;
        --it;

        const auto off = base.i - reinterpret_cast<std::uintptr_t>(it->first);
        if (off % page_size) return nullptr;

        index = off / page_size;
        if (index >= it->second.pages.size()) return nullptr;

        return &it->second;
    }

    hy::ptr reserve(hy::ptr base, std::size_t size) {
        // Verify valid page size
        if (!size || size > SIZE_MAX - (page_size - 1))
            return nullptr;

        // Round up to page size
        size = (size + page_size - 1) & ~(page_size - 1);

        const auto requested = size;
        const auto count = size / page_size;

        // Allocate reservation metadata before allocating virtual memory
        decltype(reserved_regions) pending;

        auto node = pending.extract(pending.emplace(nullptr, reservation{
            std::vector<pfn_t>(count),
            std::vector<bool>(count + 1)
        }).first);

        node.mapped().boundaries.front() = true;
        node.mapped().boundaries.back() = true;

        if (!NT_SUCCESS(NtAllocateVirtualMemoryEx(
            NtCurrentProcess(), &base.u, &size,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
            PAGE_NOACCESS, nullptr, 0
        ))) return nullptr;

        // Register the reservation without allocating another tree node
        if (base && size == requested) {
            node.key() = base.u;

            std::scoped_lock lock(mutex);
            if (reserved_regions.insert(std::move(node)).inserted)
                return base;
        }

        // Roll back an unregistered reservation
        if (base) {
            size = 0;
            NtFreeVirtualMemory(NtCurrentProcess(), &base.u, &size, MEM_RELEASE);
        }

        return nullptr;
    }

    bool release(hy::ptr base) {
        decltype(reserved_regions)::node_type reclaimed;
        std::scoped_lock lock(mutex);

        const auto it = reserved_regions.find(base.u);
        if (it == reserved_regions.end()) return false;

        auto& region = it->second;

        // Reject reservations containing active mappings
        if (region.active) return false;

        // Coalesce split placeholders
        if (region.split) {
            auto addr = base;
            std::size_t size = region.pages.size() * page_size;

            if (!NT_SUCCESS(NtFreeVirtualMemory(
                NtCurrentProcess(), &addr.u, &size,
                MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS
            ))) return false;
        }

        // Release the entire reservation
        auto addr = base;
        std::size_t size = 0;

        if (!NT_SUCCESS(NtFreeVirtualMemory(
            NtCurrentProcess(), &addr.u, &size, MEM_RELEASE
        ))) {
            if (region.split) {
                std::ranges::fill(region.boundaries, false);

                region.boundaries.front() = true;
                region.boundaries.back() = true;
                region.split = false;
            }

            return false;
        }

        reclaimed = reserved_regions.extract(it);
        return true;
    }

    bool alloc_phy(std::size_t& count, pfn_t* pages) {
        const auto requested = count;
        count = 0;

        while (count < requested) {
            hy::nt::hnd backing[batch_size];

            const auto remaining = requested - count;
            const auto limit = remaining < batch_size ? remaining : batch_size;

            std::size_t allocated = 0;

            // Allocate backing objects outside the registry lock
            for (; allocated < limit; ++allocated) {
                LARGE_INTEGER size{ .QuadPart = page_size };

                if (!NT_SUCCESS(NtCreateSection(
                    &backing[allocated], SECTION_ALL_ACCESS, nullptr, &size,
                    PAGE_READWRITE, SEC_COMMIT, nullptr
                ))) break;
            }

            if (!allocated) return false;

            // Register the entire batch
            {
                std::scoped_lock lock(mutex);

                for (std::size_t i = 0; i < allocated; ++i) {
                    if (!next_pfn) return false;

                    const auto pfn = next_pfn;

                    if (!physical_pages.emplace(
                        pfn, physical_page{ std::move(backing[i]) }
                    ).second) return false;

                    ++next_pfn;
                    pages[count++] = pfn;
                }
            }

            if (allocated != limit) return false;
        }

        return true;
    }

    bool free_phy(const std::size_t count, const pfn_t* pages) {
        for (std::size_t off = 0; off < count;) {
            decltype(physical_pages)::node_type reclaimed[batch_size];
            std::size_t released = 0;

            const auto remaining = count - off;
            const auto limit = remaining < batch_size ? remaining : batch_size;

            {
                std::scoped_lock lock(mutex);

                for (std::size_t i = 0; i < limit; ++i) {
                    const auto it = physical_pages.find(pages[off + i]);
                    if (it == physical_pages.end() || it->second.released)
                        return false;

                    auto& page = it->second;
                    page.released = true;

                    if (!page.references)
                        reclaimed[released++] = physical_pages.extract(it);
                }
            }

            // Reclaimed backing objects close outside the registry lock
            off += limit;
        }

        return true;
    }

    bool map(hy::ptr base, pfn_t pfn) {
        std::scoped_lock lock(mutex);

        const auto it = physical_pages.find(pfn);
        if (it == physical_pages.end() || it->second.released) return false;

        auto& page = it->second;

        // Locate the virtual page
        std::size_t index;
        const auto region = find_region(base, index);

        if (!region || region->pages[index]) return false;

        // Isolate the page only if its placeholder is not already split
        if (!region->boundaries[index] || !region->boundaries[index + 1]) {
            auto addr = base;
            std::size_t size = page_size;

            if (!NT_SUCCESS(NtFreeVirtualMemory(
                NtCurrentProcess(), &addr.u, &size,
                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER
            ))) return false;

            region->boundaries[index] = true;
            region->boundaries[index + 1] = true;
            region->split = true;
        }

        // Replace the placeholder with physical backing
        auto addr = base;
        std::size_t size = page_size;
        LARGE_INTEGER offset{};

        if (!NT_SUCCESS(NtMapViewOfSectionEx(
            page.backing, NtCurrentProcess(), &addr.u, &offset, &size,
            MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0
        ))) return false;

        region->pages[index] = pfn;
        ++region->active;
        ++page.references;

        return true;
    }

    bool unmap(const hy::ptr base) {
        decltype(physical_pages)::node_type reclaimed;
        std::scoped_lock lock(mutex);

        // Locate the virtual page
        std::size_t index;
        const auto region = find_region(base, index);

        if (!region || !region->pages[index]) return false;

        const auto phy = physical_pages.find(region->pages[index]);
        if (phy == physical_pages.end()) return false;

        if (!NT_SUCCESS(NtUnmapViewOfSectionEx(
            NtCurrentProcess(), base.u, MEM_PRESERVE_PLACEHOLDER
        ))) return false;

        region->pages[index] = 0;
        --region->active;

        auto& page = phy->second;
        --page.references;

        if (page.released && !page.references)
            reclaimed = physical_pages.extract(phy);

        return true;
    }
}