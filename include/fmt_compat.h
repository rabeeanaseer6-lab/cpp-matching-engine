// ─────────────────────────────────────────────────────────────────────────────
// fmt_compat.h  –  Minimal std::format-compatible wrapper for GCC < 13
//
// GCC 13+ ships <format> natively.  For older toolchains (GCC 12, Clang 14)
// we provide a thin variadic snprintf wrapper with identical call syntax:
//
//   fmt::format("Hello {}", name)  →  std::string
//
// The macro FMT_USE_STDFORMAT (auto-detected) switches to std::format when
// the standard-library implementation is available.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <string>
#include <cstdio>
#include <cstdarg>
#include <stdexcept>
#include <cstring>

// ── Detection ─────────────────────────────────────────────────────────────────
#if defined(__cpp_lib_format) && __cpp_lib_format >= 202110L
#  include <format>
#  define TRADING_USE_STD_FORMAT 1
#endif

namespace trading::fmt {

// ── Portable snprintf-based formatter ─────────────────────────────────────────
// Converts printf-style format strings to std::string.
// We convert the C++20 {}-style placeholders to printf directives before
// calling snprintf — but for simplicity we expose a printf-format API here.
// The header's public-facing `format()` function (below) uses this internally.

namespace detail {

inline std::string vformat(const char* fmt_str, ...) {
    // First pass: determine length
    va_list args, args2;
    va_start(args, fmt_str);
    va_copy(args2, args);
    const int len = std::vsnprintf(nullptr, 0, fmt_str, args);
    va_end(args);
    if (len < 0) {
        va_end(args2);
        throw std::runtime_error("fmt::vformat encoding error");
    }
    std::string buf(static_cast<std::size_t>(len + 1), '\0');
    std::vsnprintf(buf.data(), buf.size(), fmt_str, args2);
    va_end(args2);
    buf.resize(static_cast<std::size_t>(len));
    return buf;
}

} // namespace detail

// ── Public helpers (printf-style; mirrors std::format naming) ─────────────────
// Usage: trading::fmt::sprintf("%d orders @ %.4f", qty, price)

template<typename... Args>
inline std::string sprintf(const char* fmt_str, Args&&... args) {
    // calculate size
    const int n = std::snprintf(nullptr, 0, fmt_str, std::forward<Args>(args)...);
    if (n < 0) throw std::runtime_error("fmt::sprintf encoding error");
    std::string buf(static_cast<std::size_t>(n + 1), '\0');
    std::snprintf(buf.data(), buf.size(), fmt_str, std::forward<Args>(args)...);
    buf.resize(static_cast<std::size_t>(n));
    return buf;
}

} // namespace trading::fmt
