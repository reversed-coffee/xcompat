#include <Windows.h>
__declspec(dllimport) int WINAPI ConstructorReady(void);
__declspec(dllimport) int WINAPI ReservedReady(void);
DWORD WINAPI entry(void* peb) {
    (void)peb;
    if (!ConstructorReady()) return 31;
    if (!ReservedReady()) return 32;
    return 0;
}
