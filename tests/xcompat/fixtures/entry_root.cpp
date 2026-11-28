#include <phnt_windows.h>
#include <phnt.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
    bool equal(const wchar_t* left, const wchar_t* right) {
        while (*left && *left == *right) { ++left; ++right; }
        return *left == *right;
    }

    bool contains(const UNICODE_STRING& value, const wchar_t* needle) {
        const auto length = value.Length / sizeof(wchar_t);
        unsigned needle_length = 0;
        while (needle[needle_length]) ++needle_length;
        if (needle_length > length) return false;
        for (unsigned i = 0; i + needle_length <= length; ++i) {
            unsigned j = 0;
            while (j < needle_length && value.Buffer[i + j] == needle[j]) ++j;
            if (j == needle_length) return true;
        }
        return false;
    }

    bool environment_has(const wchar_t* environment, const wchar_t* expected) {
        while (*environment) {
            if (equal(environment, expected)) return true;
            while (*environment) ++environment;
            ++environment;
        }
        return false;
    }
}

extern "C" DWORD WINAPI entry(PPEB peb) {
    if (!peb || peb->ImageBaseAddress != &__ImageBase) return 11;
    if (!peb->ProcessParameters) return 12;
    if (!contains(peb->ProcessParameters->CommandLine, L"arg with spaces")) return 13;
    if (!contains(peb->ProcessParameters->CommandLine, L"quote\\\"inside")) return 14;
    if (!contains(peb->ProcessParameters->CommandLine, L"trailing\\")) return 15;
    if (!peb->ProcessParameters->CurrentDirectory.DosPath.Length) return 16;
    if (!environment_has(static_cast<const wchar_t*>(peb->ProcessParameters->Environment),
                         L"XCOMPAT_GUEST_TEST=present")) return 17;
    if (contains(peb->ProcessParameters->CommandLine, L"after-empty") &&
        !contains(peb->ProcessParameters->CommandLine, L" \"\" after-empty")) return 18;
    return 0;
}
