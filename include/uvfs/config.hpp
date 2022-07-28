#pragma once

#if defined(UVFS_BUILD_LIBRARY)
#if defined(_WIN32)
#define UVFS_EXPORT [[gnu::dllexport]] [[clang::dllexport]] __declspec(dllexport)
#else
#define UVFS_EXPORT [[gnu::visibility("default")]]
#endif
#else
#define UVFS_EXPORT
#endif

#include <bit>
static_assert(
    std::endian::native == std::endian::little,
    "This UVFS implementation requires a little-endian CPU");
