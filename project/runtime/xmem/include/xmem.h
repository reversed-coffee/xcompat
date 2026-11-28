#pragma once

#include <Windows.h>

#ifndef XMEM_LIBRARY
#   define XMEM_API __declspec(dllimport)
#else
#   define XMEM_API __declspec(dllexport)
#endif

typedef struct _XMEM_WORKING_SET_STATISTICS  
XMEM_WORKING_SET_STATISTICS, *PXMEM_WORKING_SET_STATISTICS;

#ifdef __cplusplus
extern "C" {
#endif

XMEM_API PVOID WINAPI XMemAlloc(
    SIZE_T Size,
    ULONGLONG Attributes
);

XMEM_API PVOID WINAPI XMemAllocDefault(
    SIZE_T Size,
    ULONGLONG Attributes
);

XMEM_API BOOL WINAPI XMemAllocatePhysicalPages(
    ULONG_PTR Flags,
    PULONG_PTR NumberOfPages,
    PULONG_PTR PageArray
);

XMEM_API VOID WINAPI XMemFree(
    PVOID Address,
    ULONGLONG Attributes
);

XMEM_API VOID WINAPI XMemFreeDefault(
    PVOID Address,
    ULONGLONG Attributes
);

XMEM_API BOOL WINAPI XMemFreePhysicalPages(
    ULONG_PTR NumberOfPages,
    PULONG_PTR PageArray
);

XMEM_API BOOL WINAPI XMemGetWorkingSetStatistics(
    DWORD WorkingSetFlags,
    PXMEM_WORKING_SET_STATISTICS Statistics
);

XMEM_API BOOL WINAPI XMemIsToolsMemAvailable(
    VOID
);

XMEM_API PVOID WINAPI XMemMapPhysicalPages(
    PVOID VirtualAddress,
    ULONG_PTR NumberOfPages,
    PULONG_PTR PageArray
);

XMEM_API BOOL WINAPI XMemMapPhysicalPagesScatter(
    PVOID* VirtualAddressArray,
    ULONG_PTR NumberOfPages,
    PULONG_PTR PageArray
);

XMEM_API BOOL WINAPI XMemTransferMemory(
    UINT32 Direction,
    SIZE_T Size
);

XMEM_API PVOID WINAPI XMemVirtualAlloc(
    PVOID Address,
    SIZE_T Size,
    DWORD AllocationType,
    ULONG64 Attributes,
    DWORD Protect
);

#ifdef __cplusplus
}
#endif