#include <Windows.h>

__declspec(dllimport) int WINAPI NamedValue(void);
__declspec(dllimport) int WINAPI ForwardNamed(void);
__declspec(dllimport) int WINAPI ForwardOrdinal(void);

__declspec(dllimport) int WINAPI DelayNamedValue(void);

const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
    sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64)
};
void* __guard_dispatch_icall_fptr = 0;

DWORD WINAPI entry(void* peb) {
    (void)peb;
    if (NamedValue() != 7) return 21;
    if (ForwardNamed() != 7) return 22;
    if (ForwardOrdinal() != 11) return 23;
    if (DelayNamedValue() != 7) return 24;
    return 0;
}
