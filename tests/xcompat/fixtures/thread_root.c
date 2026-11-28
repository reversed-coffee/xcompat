#include <Windows.h>
__declspec(dllimport) int WINAPI TlsGet(void);
__declspec(dllimport) void WINAPI TlsSet(int value);
__declspec(dllimport) LONG WINAPI ThreadAttachCount(void);

static DWORD WINAPI worker(void* parameter) {
    DWORD* result = (DWORD*)parameter;
    if (TlsGet() != 41) return 61;
    TlsSet(99);
    *result = GetCurrentThreadId();
    return TlsGet() == 99 ? 0 : 62;
}

DWORD WINAPI entry(void* peb) {
    DWORD observed_id = 0;
    DWORD thread_id = 0;
    DWORD exit_code = 0;
    HANDLE thread;
    (void)peb;
    TlsSet(73);
    SetLastError(0x1234);
    if (CreateThread(0, 0, 0, 0, 0, 0) != 0 || GetLastError() != ERROR_INVALID_PARAMETER) return 63;
    thread = CreateThread(0, 131072, worker, &observed_id, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &thread_id);
    if (!thread || !thread_id || observed_id) return 64;
    if (ResumeThread(thread) == (DWORD)-1) return 65;
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) return 66;
    if (!GetExitCodeThread(thread, &exit_code) || exit_code || observed_id != thread_id) return 67;
    CloseHandle(thread);
    if (TlsGet() != 73 || ThreadAttachCount() < 1) return 68;
    return 0;
}
