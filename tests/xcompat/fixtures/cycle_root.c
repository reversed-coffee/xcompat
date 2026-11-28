#include <Windows.h>
__declspec(dllimport) int WINAPI ATouch(void);
__declspec(dllimport) const char* WINAPI RecordedOrder(void);
DWORD WINAPI entry(void* peb) {
    const char* order;
    (void)peb;
    if (ATouch() != 2) return 101;
    order = RecordedOrder();
    return order[0] == 'A' && order[1] == 'B' ? 0 : 102;
}
