#include <Windows.h>
__declspec(dllimport) int WINAPI Cycle(void);
DWORD WINAPI entry(void* peb) { (void)peb; return Cycle(); }
