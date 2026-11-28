#include <phnt_windows.h>
#include <phnt.h>

DWORD WINAPI entry(void* peb) {
    MEMORY_BASIC_INFORMATION info;
    SIZE_T reserve;
    SIZE_T commit = 0;
    unsigned char* cursor;
    (void)peb;
    reserve = (SIZE_T)((unsigned char*)NtCurrentTeb()->NtTib.StackBase -
        (unsigned char*)NtCurrentTeb()->DeallocationStack);
    if (reserve < 2097152) return 71;
    cursor = (unsigned char*)NtCurrentTeb()->NtTib.StackLimit;
    while (cursor < (unsigned char*)NtCurrentTeb()->NtTib.StackBase) {
        if (VirtualQuery(cursor, &info, sizeof(info)) != sizeof(info)) return 72;
        if (info.State == MEM_COMMIT) commit += info.RegionSize;
        cursor = (unsigned char*)info.BaseAddress + info.RegionSize;
    }
    return commit >= 131072 ? 0 : 73;
}
