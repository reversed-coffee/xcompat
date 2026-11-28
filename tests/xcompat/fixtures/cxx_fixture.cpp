#include <Windows.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
    int constructed;
    int process_reserved;
    volatile LONG thread_attaches;
    thread_local int tls_value = 41;

    struct constructor_probe {
        constructor_probe() { constructed = 0x5143; }
    } probe;
}

BOOL WINAPI DllMain(HINSTANCE, const DWORD reason, void* reserved) {
    if (reason == DLL_PROCESS_ATTACH) process_reserved = reserved != nullptr;
    if (reason == DLL_THREAD_ATTACH) InterlockedIncrement(&thread_attaches);
    return TRUE;
}

extern "C" __declspec(dllexport) int WINAPI ConstructorReady() { return constructed == 0x5143; }
extern "C" __declspec(dllexport) int WINAPI TlsGet() { return tls_value; }
extern "C" __declspec(dllexport) void WINAPI TlsSet(const int value) { tls_value = value; }
extern "C" __declspec(dllexport) int WINAPI ReservedReady() { return process_reserved; }
extern "C" __declspec(dllexport) LONG WINAPI ThreadAttachCount() { return thread_attaches; }
extern "C" __declspec(dllexport) int WINAPI ExceptionReady();
extern "C" __declspec(dllexport) int WINAPI UnwindReady() {
    DWORD64 image_base{};
    const auto function = RtlLookupFunctionEntry(
        reinterpret_cast<DWORD64>(&ExceptionReady), &image_base, nullptr);
    if (!function) return 2;
    if (image_base != reinterpret_cast<DWORD64>(&__ImageBase)) return 3;
    return 1;
}
extern "C" __declspec(dllexport) int WINAPI SehReady() {
    __try {
        RaiseException(0xE0424242, 0, 0, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}
extern "C" __declspec(dllexport) int WINAPI ExceptionReady() {
    try {
        throw 0x7351;
    } catch (const int value) {
        return value == 0x7351;
    }
}
