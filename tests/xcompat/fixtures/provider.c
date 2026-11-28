#include <Windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void* reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}

int WINAPI NamedValue(void) { return 7; }
int WINAPI OrdinalValue(void) { return 11; }
