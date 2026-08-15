#pragma once

// Only one spelling is correct per compiler; listing all three meant every
// compiler warned about the two it does not understand.
#if defined(UVFS_BUILD_LIBRARY) && defined(UVFS_SHARED)
#if defined(_WIN32)
#define UVFS_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define UVFS_EXPORT __attribute__((visibility("default")))
#else
#define UVFS_EXPORT
#endif
#else
#define UVFS_EXPORT
#endif

#include <bit>
static_assert(
    std::endian::native == std::endian::little,
    "This UVFS implementation requires a little-endian CPU");
