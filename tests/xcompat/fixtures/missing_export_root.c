#include <Windows.h>
__declspec(dllimport) int WINAPI Missing(void);
DWORD WINAPI entry(void* peb) { (void)peb; return Missing(); }
