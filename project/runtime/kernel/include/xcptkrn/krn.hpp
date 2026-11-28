#pragma once

#ifndef XCOMPAT_KRN
#   define KRN_API __declspec(dllimport)
#else
#   define KRN_API __declspec(dllexport)
#endif
