#include "xmem.h"

PVOID WINAPI XMemAlloc(
    SIZE_T Size,
    ULONGLONG Attributes
) {
    return nullptr;
}

PVOID WINAPI XMemAllocDefault(
    SIZE_T Size,
    ULONGLONG Attributes
) {
    return nullptr;
}

BOOL WINAPI XMemAllocatePhysicalPages(
    ULONG_PTR Flags,
    PULONG_PTR NumberOfPages,
    PULONG_PTR PageArray
) {
    return FALSE;
}

VOID WINAPI XMemFree(
    PVOID Address,
    ULONGLONG Attributes
) {
    return;
}

VOID WINAPI XMemFreeDefault(
    PVOID Address,
    ULONGLONG Attributes
) {
}

BOOL WINAPI XMemFreePhysicalPages(
    ULONG_PTR NumberOfPages,
    PULONG_PTR PageArray
) {
    return FALSE;
}

BOOL WINAPI XMemGetWorkingSetStatistics(
    DWORD WorkingSetFlags,
    PXMEM_WORKING_SET_STATISTICS Statistics
) {
    return FALSE;
}

BOOL WINAPI XMemIsToolsMemAvailable(
    VOID
) {
    return FALSE;
}

PVOID WINAPI XMemMapPhysicalPages(
    PVOID VirtualAddress,
    ULONG_PTR NumberOfPages,
    PULONG_PTR PageArray
) {
    return nullptr;
}

BOOL WINAPI XMemMapPhysicalPagesScatter(
    PVOID* VirtualAddressArray,
    ULONG_PTR NumberOfPages,
    PULONG_PTR PageArray
) {
    return FALSE;
}

BOOL WINAPI XMemTransferMemory(
    UINT32 Direction,
    SIZE_T Size
) {
    return FALSE;
}

PVOID WINAPI XMemVirtualAlloc(
    PVOID Address,
    SIZE_T Size,
    DWORD AllocationType,
    ULONG64 Attributes,
    DWORD Protect
) {
    return nullptr;
}
