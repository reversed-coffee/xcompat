#include <Windows.h>
__declspec(dllimport) void WINAPI Record(char value);
__declspec(dllimport) int WINAPI ATouch(void);
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void* reserved) {
    (void)instance; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) Record('B');
    return TRUE;
}
int WINAPI BTouch(void) { return 1; }
void* force_cycle = (void*)&ATouch;
