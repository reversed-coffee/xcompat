#include <Windows.h>
__declspec(dllimport) int WINAPI TlsGet(void);
__declspec(dllimport) void WINAPI TlsSet(int value);
DWORD WINAPI entry(void* peb) {
    (void)peb;
    if (TlsGet() != 41) return 41;
    TlsSet(73);
    return TlsGet() == 73 ? 0 : 42;
}
