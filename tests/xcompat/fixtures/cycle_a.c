#include <Windows.h>
__declspec(dllimport) void WINAPI Record(char value);
__declspec(dllimport) int WINAPI BTouch(void);
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void* reserved) {
    (void)instance; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) Record('A');
    return TRUE;
}
int WINAPI ATouch(void) { return BTouch() + 1; }
