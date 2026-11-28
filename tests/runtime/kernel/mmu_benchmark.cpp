#include "xcptkrn/mmu.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

namespace {
    using namespace xcompat::krn;
    using clock_type = std::chrono::steady_clock;

    struct benchmark_settings {
        std::string_view mode;
        double target_seconds;
        std::size_t samples;
    };

    volatile std::uint64_t* value_at(const hy::ptr address) {
        return static_cast<volatile std::uint64_t*>(address.u);
    }

    template <typename Work>
    bool measure(
        const benchmark_settings& settings,
        const std::string_view name,
        const std::size_t threads,
        const std::size_t batch_pages,
        const std::size_t reservation_pages,
        const std::size_t operations_per_iteration,
        Work&& work
    ) {
        constexpr double minimum_probe_seconds = 0.005;
        constexpr std::size_t maximum_iterations = 1'000'000;

        std::size_t probe_iterations = 1;
        double probe_seconds{};

        for (;;) {
            const auto start = clock_type::now();
            if (!work(probe_iterations)) {
                std::cerr << name << ": calibration failed\n";
                return false;
            }
            probe_seconds = std::chrono::duration<double>(clock_type::now() - start).count();

            if (probe_seconds >= minimum_probe_seconds
                || probe_iterations == maximum_iterations) {
                break;
            }

            probe_iterations = std::min(maximum_iterations, probe_iterations * 2);
        }

        const auto estimated = static_cast<std::size_t>(
            static_cast<double>(probe_iterations) * settings.target_seconds
            / std::max(probe_seconds, std::numeric_limits<double>::min())
        );
        const auto iterations = std::clamp<std::size_t>(
            estimated, 1, maximum_iterations
        );

        std::vector<double> durations(settings.samples);
        for (auto& duration : durations) {
            const auto start = clock_type::now();
            if (!work(iterations)) {
                std::cerr << name << ": measured run failed\n";
                return false;
            }
            duration = std::chrono::duration<double>(clock_type::now() - start).count();
        }

        std::ranges::sort(durations);
        const auto median = durations[durations.size() / 2];
        const auto operations = static_cast<double>(iterations)
            * static_cast<double>(operations_per_iteration);
        const auto nanoseconds_per_operation = median * 1'000'000'000.0 / operations;
        const auto operations_per_second = operations / median;

        std::cout << name << ',' << settings.mode << ',' << threads << ','
                  << batch_pages << ',' << reservation_pages << ','
                  << iterations << ',' << operations_per_iteration << ','
                  << std::fixed << std::setprecision(9) << median << ','
                  << std::setprecision(3) << nanoseconds_per_operation << ','
                  << std::setprecision(3) << operations_per_second << '\n';
        return true;
    }

    bool measure_physical_cycles(const benchmark_settings& settings) {
        constexpr std::array batches{1uz, 16uz, 64uz, 256uz};

        for (const auto batch : batches) {
            std::vector<mmu::pfn_t> pages(batch);
            const auto work = [&](const std::size_t iterations) {
                for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                    std::size_t count = batch;
                    if (!mmu::alloc_phy(count, pages.data()) || count != batch)
                        return false;
                    if (!mmu::free_phy(batch, pages.data()))
                        return false;
                }
                return true;
            };

            if (!measure(
                settings, "physical_alloc_free", 1, batch, 0,
                batch, work
            )) return false;
        }

        return true;
    }

    bool measure_reservation_cycles(const benchmark_settings& settings) {
        constexpr std::array page_counts{1uz, 16uz, 256uz, 4096uz};

        for (const auto pages : page_counts) {
            const auto work = [pages](const std::size_t iterations) {
                for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                    const auto region = mmu::reserve(nullptr, pages * mmu::page_size);
                    if (!region || !mmu::release(region))
                        return false;
                }
                return true;
            };

            if (!measure(
                settings, "reserve_release", 1, 0, pages,
                1, work
            )) return false;
        }

        return true;
    }

    bool measure_map_cycles(
        const benchmark_settings& settings,
        const bool touch
    ) {
        std::size_t count = 1;
        mmu::pfn_t page{};
        if (!mmu::alloc_phy(count, &page))
            return false;

        const auto region = mmu::reserve(nullptr, mmu::page_size);
        if (!region || !mmu::map(region, page) || !mmu::unmap(region))
            return false;

        std::uint64_t checksum{};
        const auto work = [&](const std::size_t iterations) {
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                if (!mmu::map(region, page))
                    return false;
                if (touch) {
                    *value_at(region) = iteration;
                    checksum ^= *value_at(region);
                }
                if (!mmu::unmap(region))
                    return false;
            }
            return true;
        };

        const auto name = touch ? "map_touch_unmap" : "map_unmap";
        const auto measured = measure(settings, name, 1, 1, 1, 1, work);
        const auto cleaned = mmu::free_phy(1, &page) && mmu::release(region);

        if (checksum == std::numeric_limits<std::uint64_t>::max())
            std::cerr << "unreachable checksum: " << checksum << '\n';
        return measured && cleaned;
    }

    bool measure_range_mapping(const benchmark_settings& settings) {
        constexpr std::size_t page_count = 16;
        std::array<mmu::pfn_t, page_count> pages{};
        std::size_t count = pages.size();
        if (!mmu::alloc_phy(count, pages.data()))
            return false;

        const auto region = mmu::reserve(nullptr, page_count * mmu::page_size);
        if (!region)
            return false;

        for (std::size_t i = 0; i < page_count; ++i) {
            const auto address = region + i * mmu::page_size;
            if (!mmu::map(address, pages[i]) || !mmu::unmap(address))
                return false;
        }

        const auto work = [&](const std::size_t iterations) {
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                for (std::size_t i = 0; i < page_count; ++i) {
                    if (!mmu::map(region + i * mmu::page_size, pages[i]))
                        return false;
                }
                for (std::size_t i = 0; i < page_count; ++i) {
                    if (!mmu::unmap(region + i * mmu::page_size))
                        return false;
                }
            }
            return true;
        };

        const auto measured = measure(
            settings, "map_range", 1, page_count, page_count,
            page_count, work
        );
        const auto cleaned = mmu::free_phy(pages.size(), pages.data())
            && mmu::release(region);
        return measured && cleaned;
    }

    bool measure_alias_round_trip(const benchmark_settings& settings) {
        std::size_t count = 1;
        mmu::pfn_t page{};
        if (!mmu::alloc_phy(count, &page))
            return false;

        const auto region = mmu::reserve(nullptr, 2 * mmu::page_size);
        if (!region)
            return false;
        const auto alias = region + mmu::page_size;

        if (!mmu::map(region, page) || !mmu::map(alias, page)
            || !mmu::unmap(region) || !mmu::unmap(alias)) {
            return false;
        }

        std::uint64_t checksum{};
        const auto work = [&](const std::size_t iterations) {
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                if (!mmu::map(region, page) || !mmu::map(alias, page))
                    return false;
                *value_at(region) = iteration;
                checksum ^= *value_at(alias);
                *value_at(alias) = ~iteration;
                checksum ^= *value_at(region);
                if (!mmu::unmap(region) || !mmu::unmap(alias))
                    return false;
            }
            return true;
        };

        const auto measured = measure(
            settings, "alias_round_trip", 1, 1, 2, 1, work
        );
        const auto cleaned = mmu::free_phy(1, &page) && mmu::release(region);

        if (checksum == 0x9e3779b97f4a7c15ull)
            std::cerr << "checksum: " << checksum << '\n';
        return measured && cleaned;
    }

    bool measure_full_lifecycle(const benchmark_settings& settings) {
        const auto work = [](const std::size_t iterations) {
            std::uint64_t checksum{};
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                std::size_t count = 1;
                mmu::pfn_t page{};
                if (!mmu::alloc_phy(count, &page))
                    return false;

                const auto region = mmu::reserve(nullptr, mmu::page_size);
                if (!region || !mmu::map(region, page))
                    return false;

                *value_at(region) = iteration;
                checksum ^= *value_at(region);

                if (!mmu::unmap(region) || !mmu::release(region)
                    || !mmu::free_phy(1, &page)) {
                    return false;
                }
            }

            if (checksum == std::numeric_limits<std::uint64_t>::max())
                std::cerr << "unreachable checksum: " << checksum << '\n';
            return true;
        };

        return measure(
            settings, "full_lifecycle", 1, 1, 1,
            1, work
        );
    }

    bool measure_sparse_lifecycle(const benchmark_settings& settings) {
        constexpr std::size_t mapped_pages = 8;
        constexpr std::size_t reservation_pages = 128;
        constexpr std::array<std::size_t, mapped_pages> indices{
            0, 1, 17, 63, 64, 95, 126, 127
        };

        std::array<mmu::pfn_t, mapped_pages> pages{};
        const auto work = [&](const std::size_t iterations) {
            std::uint64_t checksum{};
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                std::size_t count = pages.size();
                if (!mmu::alloc_phy(count, pages.data()))
                    return false;

                const auto region = mmu::reserve(
                    nullptr, reservation_pages * mmu::page_size
                );
                if (!region)
                    return false;

                for (std::size_t i = 0; i < mapped_pages; ++i) {
                    const auto address = region + indices[i] * mmu::page_size;
                    if (!mmu::map(address, pages[i]))
                        return false;
                    *value_at(address) = iteration + i;
                    checksum ^= *value_at(address);
                }

                for (const auto index : indices) {
                    if (!mmu::unmap(region + index * mmu::page_size))
                        return false;
                }

                if (!mmu::release(region)
                    || !mmu::free_phy(pages.size(), pages.data())) {
                    return false;
                }
            }

            if (checksum == std::numeric_limits<std::uint64_t>::max())
                std::cerr << "unreachable checksum: " << checksum << '\n';
            return true;
        };

        return measure(
            settings, "sparse_lifecycle", 1, mapped_pages, reservation_pages,
            mapped_pages, work
        );
    }

    bool measure_concurrent_physical(
        const benchmark_settings& settings,
        const std::size_t thread_count
    ) {
        const auto work = [thread_count](const std::size_t iterations) {
            std::atomic<bool> successful{true};
            std::barrier start(static_cast<std::ptrdiff_t>(thread_count));
            std::vector<std::thread> threads;
            threads.reserve(thread_count);

            for (std::size_t thread = 0; thread < thread_count; ++thread) {
                threads.emplace_back([&] {
                    start.arrive_and_wait();
                    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                        std::size_t count = 1;
                        mmu::pfn_t page{};
                        if (!mmu::alloc_phy(count, &page)
                            || !mmu::free_phy(1, &page)) {
                            successful.store(false, std::memory_order_relaxed);
                            return;
                        }
                    }
                });
            }

            for (auto& thread : threads)
                thread.join();
            return successful.load(std::memory_order_relaxed);
        };

        return measure(
            settings, "concurrent_physical", thread_count, 1, 0,
            thread_count, work
        );
    }

    bool measure_concurrent_lifecycle(
        const benchmark_settings& settings,
        const std::size_t thread_count
    ) {
        const auto work = [thread_count](const std::size_t iterations) {
            std::atomic<bool> successful{true};
            std::barrier start(static_cast<std::ptrdiff_t>(thread_count));
            std::vector<std::thread> threads;
            threads.reserve(thread_count);

            for (std::size_t thread = 0; thread < thread_count; ++thread) {
                threads.emplace_back([&, thread] {
                    start.arrive_and_wait();
                    std::uint64_t checksum{};
                    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                        std::size_t count = 1;
                        mmu::pfn_t page{};
                        if (!mmu::alloc_phy(count, &page)) {
                            successful.store(false, std::memory_order_relaxed);
                            return;
                        }

                        const auto region = mmu::reserve(nullptr, mmu::page_size);
                        if (!region || !mmu::map(region, page)) {
                            successful.store(false, std::memory_order_relaxed);
                            return;
                        }

                        *value_at(region) = (thread << 32) | iteration;
                        checksum ^= *value_at(region);

                        if (!mmu::unmap(region) || !mmu::release(region)
                            || !mmu::free_phy(1, &page)) {
                            successful.store(false, std::memory_order_relaxed);
                            return;
                        }
                    }

                    if (checksum == std::numeric_limits<std::uint64_t>::max())
                        successful.store(false, std::memory_order_relaxed);
                });
            }

            for (auto& thread : threads)
                thread.join();
            return successful.load(std::memory_order_relaxed);
        };

        return measure(
            settings, "concurrent_lifecycle", thread_count, 1, 1,
            thread_count, work
        );
    }
}

int main(const int argc, const char* const* argv) {
    benchmark_settings settings{"full", 0.150, 7};
    if (argc == 2 && std::string_view(argv[1]) == "--quick")
        settings = {"quick", 0.020, 3};
    else if (argc != 1) {
        std::cerr << "usage: xcompat-kernel-mmu-benchmark [--quick]\n";
        return 2;
    }

    std::cout << "benchmark,mode,threads,batch_pages,reservation_pages,iterations,"
                 "operations_per_iteration,median_seconds,ns_per_operation,"
                 "operations_per_second\n";

    if (!measure_physical_cycles(settings)
        || !measure_reservation_cycles(settings)
        || !measure_map_cycles(settings, false)
        || !measure_map_cycles(settings, true)
        || !measure_range_mapping(settings)
        || !measure_alias_round_trip(settings)
        || !measure_full_lifecycle(settings)
        || !measure_sparse_lifecycle(settings)) {
        return 1;
    }

    constexpr std::array thread_counts{1uz, 2uz, 4uz, 8uz};
    for (const auto threads : thread_counts) {
        if (!measure_concurrent_physical(settings, threads))
            return 1;
    }
    for (const auto threads : thread_counts) {
        if (!measure_concurrent_lifecycle(settings, threads))
            return 1;
    }

    return 0;
}
