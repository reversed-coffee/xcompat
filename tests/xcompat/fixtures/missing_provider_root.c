#include <Windows.h>
__declspec(dllimport) int WINAPI AbsentValue(void);
DWORD WINAPI entry(void* peb) { (void)peb; return AbsentValue(); }
