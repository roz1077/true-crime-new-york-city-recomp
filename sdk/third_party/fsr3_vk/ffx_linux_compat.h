// Force-included when building AMD's FidelityFX SDK 1.1.4 FSR 3.1 Vulkan sources on Linux (not part of the SDK):
// the standard headers and MSVC conveniences the sources take for granted on Windows.
#pragma once
#include <cstring>
#include <cwchar>
#include <codecvt>
#include <locale>
#include <string>
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif
#include <cmath>
#include <cstdio>
#include <cstdarg>
#if !defined(_WIN32)
template <size_t N> inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) {
    std::wcsncpy(dst, src, N - 1);
    dst[N - 1] = 0;
    return 0;
}
inline int wcscpy_s(wchar_t* dst, size_t n, const wchar_t* src) {
    if (!n) return 0;
    std::wcsncpy(dst, src, n - 1);
    dst[n - 1] = 0;
    return 0;
}
template <size_t N> inline int strcpy_s(char (&dst)[N], const char* src) {
    std::snprintf(dst, N, "%s", src);
    return 0;
}
inline int strcpy_s(char* dst, size_t n, const char* src) {
    if (n) std::snprintf(dst, n, "%s", src);
    return 0;
}
template <size_t N> inline int sprintf_s(char (&dst)[N], const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int r = std::vsnprintf(dst, N, fmt, args);
    va_end(args);
    return r;
}
inline int sprintf_s(char* dst, size_t n, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int r = std::vsnprintf(dst, n, fmt, args);
    va_end(args);
    return r;
}
using std::floor;
using std::log2;
#endif
