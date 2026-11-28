#pragma once

#include <memory>

#ifndef XCOMPAT_KRN
#   define KRN_API __declspec(dllimport)
#else
#   define KRN_API __declspec(dllexport)
#endif

namespace xcompat::krn {
    KRN_API bool initialize();
}
