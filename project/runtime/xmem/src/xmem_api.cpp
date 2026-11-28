#include "xmem.h"

#include "xcptkrn/mmu.hpp"

#include <Psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>

namespace {
    using alloc_routine = PVOID (WINAPI*)(SIZE_T, ULONGLONG);
    using free_routine = VOID (WINAPI*)(PVOID, ULONGLONG);

    struct allocation_hooks {
        alloc_routine alloc;
        free_routine free;
    };

    struct allocation_header {
        std::uintptr_t signature;
        void* base;
    };

    constexpr std::uintptr_t allocation_cookie = 0xd12c'583a'a76e'941bull;
    constexpr ULONGLONG alignment_shift = 24;
    constexpr ULONGLONG alignment_mask = 0x1f;

    constexpr DWORD mem_64kb_pages = 0x20400000;
    constexpr DWORD host_allocation_mask =
        MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN |
        MEM_WRITE_WATCH | MEM_RESET_UNDO;
    constexpr DWORD host_protection_mask = 0x7ff;
    constexpr SIZE_T transfer_granularity = 2 * 1024 * 1024;
    constexpr UINT32 title_to_system = 1;

    static_assert(sizeof(ULONG_PTR) == sizeof(xcompat::krn::mmu::pfn_t));

    std::atomic_size_t allocated_physical_pages{};
    std::atomic_size_t transferred_title_memory{};
    std::atomic<const allocation_hooks*> cached_hooks{};

    bool is_in_image(const void* pointer, const std::uintptr_t base, const std::size_t size) {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        return address >= base && address - base < size;
    }

    const allocation_hooks* find_allocation_hooks() {
        if (const auto hooks = cached_hooks.load(std::memory_order_acquire))
            return hooks;

        const auto module = GetModuleHandleW(nullptr);
        if (!module) return nullptr;

        const auto base = reinterpret_cast<std::uintptr_t>(module);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

        const auto size = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
        const auto symbol = GetProcAddress(module, "__XMemAllocationHooks__");
        const auto hooks = reinterpret_cast<const allocation_hooks*>(symbol);
        if (!hooks || !is_in_image(hooks, base, size) ||
            !is_in_image(reinterpret_cast<const void*>(hooks->alloc), base, size) ||
            !is_in_image(reinterpret_cast<const void*>(hooks->free), base, size))
            return nullptr;

        cached_hooks.store(hooks, std::memory_order_release);
        return hooks;
    }

    std::size_t allocation_alignment(const ULONGLONG attributes) {
        const auto exponent = (attributes >> alignment_shift) & alignment_mask;
        return std::max<std::size_t>(alignof(std::max_align_t), std::size_t{1} << exponent);
    }

    bool readable_header(const allocation_header* header) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(header, &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;

        const auto address = reinterpret_cast<std::uintptr_t>(header);
        const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        return address <= end && end - address >= sizeof(*header);
    }

    bool valid_empty_page(const PVOID address) {
        MEMORY_BASIC_INFORMATION info{};
        return VirtualQuery(address, &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_RESERVE;
    }

    bool unmap_page(const PVOID address) {
        if (xcompat::krn::mmu::unmap(address)) return true;
        return valid_empty_page(address);
    }

    bool map_page(const PVOID address, const xcompat::krn::mmu::pfn_t page) {
        if (xcompat::krn::mmu::map(address, page)) return true;
        return xcompat::krn::mmu::unmap(address) &&
            xcompat::krn::mmu::map(address, page);
    }

    bool valid_page_range(const PVOID base, const ULONG_PTR count) {
        const auto address = reinterpret_cast<std::uintptr_t>(base);
        return base && !(address % xcompat::krn::mmu::page_size) &&
            count <= (std::numeric_limits<std::uintptr_t>::max() - address) /
                xcompat::krn::mmu::page_size;
    }

    SIZE_T host_title_limit() {
        MEMORYSTATUSEX status{ .dwLength = sizeof(status) };
        return GlobalMemoryStatusEx(&status)
            ? static_cast<SIZE_T>(std::min<ULONGLONG>(
                status.ullTotalPageFile, std::numeric_limits<SIZE_T>::max()))
            : 0;
    }
}

PVOID WINAPI XMemAlloc(const SIZE_T Size, const ULONGLONG Attributes) {
    if (const auto hooks = find_allocation_hooks())
        return hooks->alloc(Size, Attributes);
    return XMemAllocDefault(Size, Attributes);
}

PVOID WINAPI XMemAllocDefault(const SIZE_T Size, const ULONGLONG Attributes) {
    const auto alignment = allocation_alignment(Attributes);
    const auto requested = std::max<SIZE_T>(Size, 1);
    if (requested > std::numeric_limits<SIZE_T>::max() -
        sizeof(allocation_header) - (alignment - 1)) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    const auto total = requested + sizeof(allocation_header) + alignment - 1;
    const auto base = HeapAlloc(GetProcessHeap(), 0, total);
    if (!base) return nullptr;

    const auto start = reinterpret_cast<std::uintptr_t>(base) + sizeof(allocation_header);
    const auto address = (start + alignment - 1) & ~(alignment - 1);
    auto header = reinterpret_cast<allocation_header*>(address) - 1;
    header->base = base;
    header->signature = allocation_cookie ^ address ^ reinterpret_cast<std::uintptr_t>(base);
    return reinterpret_cast<PVOID>(address);
}

BOOL WINAPI XMemAllocatePhysicalPages(
    const ULONG_PTR Flags,
    const PULONG_PTR NumberOfPages,
    const PULONG_PTR PageArray
) {
    static_cast<void>(Flags);
    if (!NumberOfPages || (*NumberOfPages && !PageArray)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    std::size_t count = *NumberOfPages;
    const auto result = xcompat::krn::mmu::alloc_phy(
        count, reinterpret_cast<xcompat::krn::mmu::pfn_t*>(PageArray));
    *NumberOfPages = count;
    allocated_physical_pages.fetch_add(count, std::memory_order_relaxed);
    if (!result) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return result;
}

VOID WINAPI XMemFree(const PVOID Address, const ULONGLONG Attributes) {
    if (const auto hooks = find_allocation_hooks()) {
        hooks->free(Address, Attributes);
        return;
    }
    XMemFreeDefault(Address, Attributes);
}

VOID WINAPI XMemFreeDefault(const PVOID Address, const ULONGLONG Attributes) {
    static_cast<void>(Attributes);
    if (!Address) return;

    const auto header = reinterpret_cast<const allocation_header*>(Address) - 1;
    if (!readable_header(header)) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(Address);
    const auto base = header->base;
    if (header->signature !=
        (allocation_cookie ^ address ^ reinterpret_cast<std::uintptr_t>(base)) ||
        !HeapValidate(GetProcessHeap(), 0, base)) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return;
    }

    HeapFree(GetProcessHeap(), 0, base);
}

BOOL WINAPI XMemFreePhysicalPages(
    const ULONG_PTR NumberOfPages,
    const PULONG_PTR PageArray
) {
    if (NumberOfPages && !PageArray) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    const auto result = xcompat::krn::mmu::free_phy(
        NumberOfPages,
        reinterpret_cast<const xcompat::krn::mmu::pfn_t*>(PageArray));
    if (result) {
        allocated_physical_pages.fetch_sub(NumberOfPages, std::memory_order_relaxed);
    } else {
        SetLastError(ERROR_INVALID_PARAMETER);
    }
    return result;
}

BOOL WINAPI XMemGetWorkingSetStatistics(
    const DWORD WorkingSetFlags,
    const PXMEM_WORKING_SET_STATISTICS Statistics
) {
    static_cast<void>(WorkingSetFlags);
    if (!Statistics) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    PROCESS_MEMORY_COUNTERS_EX counters{
        .cb = static_cast<DWORD>(sizeof(counters)),
    };
    if (!K32GetProcessMemoryInfo(
        GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
        sizeof(counters)))
        return FALSE;

    const auto limit = host_title_limit();
    if (!limit) return FALSE;

    const auto transferred = transferred_title_memory.load(std::memory_order_relaxed);
    *Statistics = {
        .TitleLimit = transferred < limit ? limit - transferred : 0,
        .TitleUsed = counters.PrivateUsage,
        .ToolsLimit = 0,
        .ToolsUsed = 0,
        .StackUsed = 0,
        .PhysicalPages = static_cast<ULONG>(std::min<std::size_t>(
            allocated_physical_pages.load(std::memory_order_relaxed), MAXULONG)),
        .GpuOptimalLimit = 0,
        .GpuOptimalUsed = 0,
        .ToolsGpuOptimalLimit = 0,
        .ToolsGpuOptimalUsed = 0,
        .KernelPoolUsed = 0,
        .KernelPoolLimit = 0,
    };
    return TRUE;
}

BOOL WINAPI XMemIsToolsMemAvailable(VOID) {
    return FALSE;
}

PVOID WINAPI XMemMapPhysicalPages(
    const PVOID VirtualAddress,
    const ULONG_PTR NumberOfPages,
    const PULONG_PTR PageArray
) {
    if (!valid_page_range(VirtualAddress, NumberOfPages)) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }

    auto address = reinterpret_cast<std::uintptr_t>(VirtualAddress);
    for (ULONG_PTR index = 0; index < NumberOfPages; ++index) {
        const auto target = reinterpret_cast<PVOID>(
            address + index * xcompat::krn::mmu::page_size);
        const auto success = PageArray
            ? map_page(target, PageArray[index])
            : unmap_page(target);
        if (!success) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
    }
    return VirtualAddress;
}

BOOL WINAPI XMemMapPhysicalPagesScatter(
    PVOID* const VirtualAddressArray,
    const ULONG_PTR NumberOfPages,
    const PULONG_PTR PageArray
) {
    if (NumberOfPages && !VirtualAddressArray) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    for (ULONG_PTR index = 0; index < NumberOfPages; ++index) {
        const auto address = VirtualAddressArray[index];
        if (!valid_page_range(address, 1)) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }

        const auto page = PageArray ? PageArray[index] : 0;
        const auto success = page ? map_page(address, page) : unmap_page(address);
        if (!success) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
    }
    return TRUE;
}

BOOL WINAPI XMemTransferMemory(const UINT32 Direction, const SIZE_T Size) {
    if (Direction != title_to_system) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!Size) return TRUE;
    if (Size > std::numeric_limits<SIZE_T>::max() - (transfer_granularity - 1)) {
        SetLastError(ERROR_ARITHMETIC_OVERFLOW);
        return FALSE;
    }

    const auto rounded = (Size + transfer_granularity - 1) & ~(transfer_granularity - 1);
    const auto limit = host_title_limit();
    auto transferred = transferred_title_memory.load(std::memory_order_relaxed);
    while (transferred <= limit && rounded <= limit - transferred) {
        if (transferred_title_memory.compare_exchange_weak(
            transferred, transferred + rounded,
            std::memory_order_relaxed, std::memory_order_relaxed))
            return TRUE;
    }

    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
}

PVOID WINAPI XMemVirtualAlloc(
    const PVOID Address,
    const SIZE_T Size,
    const DWORD AllocationType,
    const ULONG64 Attributes,
    const DWORD Protect
) {
    static_cast<void>(Attributes);

    const auto reserves_mappable_pages =
        (AllocationType & mem_64kb_pages) == mem_64kb_pages &&
        (AllocationType & MEM_RESERVE) && !(AllocationType & MEM_COMMIT);
    if (reserves_mappable_pages) {
        const auto reserved = xcompat::krn::mmu::reserve(Address, Size);
        if (!reserved) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return reserved;
    }

    const auto host_type = AllocationType & host_allocation_mask;
    if (!host_type) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    auto host_protect = Protect & host_protection_mask;
    if (!host_protect)
        host_protect = host_type & MEM_COMMIT ? PAGE_READWRITE : PAGE_NOACCESS;

    return VirtualAlloc(Address, Size, host_type, host_protect);
}
