#pragma once

#include <iostream>
#include <vector>

#include <hydra/ost.hpp>
#include <hydra/ptr.hpp>
#include <hydra/nt/hnd.hpp>

namespace xcompat::krn::mmu {
    using pfn_t = std::uintptr_t;

    inline constexpr std::size_t page_size = 0x10000; // 64 KiB
    inline constexpr pfn_t bad_pfn = 0;

    struct physical_page {
        hy::nt::hnd backing;
        std::size_t references{};
        bool released{};
    };

    struct reservation {
        std::vector<pfn_t> pages;
        std::vector<bool> boundaries;

        std::size_t active{};
        bool split{};
    };

    hy::ptr reserve(hy::ptr base, std::size_t size);
    bool release(hy::ptr base);

    bool alloc_phy(std::size_t& count, pfn_t* pages);
    bool free_phy(std::size_t count, const pfn_t* pages);

    bool map(hy::ptr base, pfn_t pfn);
    bool unmap(hy::ptr base);
}
