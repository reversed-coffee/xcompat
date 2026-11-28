#include <Windows.h>
__declspec(dllimport) int WINAPI ExceptionReady(void);
__declspec(dllimport) int WINAPI SehReady(void);
__declspec(dllimport) int WINAPI UnwindReady(void);
DWORD WINAPI entry(void* peb) {
    (void)peb;
    const int unwind = UnwindReady();
    if (unwind != 1) return 50 + unwind;
    if (!SehReady()) return 49;
    return ExceptionReady() ? 0 : 51;
}
