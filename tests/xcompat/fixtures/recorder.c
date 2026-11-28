#include <Windows.h>

static char order[3];
static LONG position;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void* reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}

void WINAPI Record(char value) {
    LONG index = InterlockedIncrement(&position) - 1;
    if (index < 2) order[index] = value;
}

const char* WINAPI RecordedOrder(void) { return order; }
