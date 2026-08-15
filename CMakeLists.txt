cmake_minimum_required(VERSION 3.16)

project(uvfs VERSION 0.2.0 LANGUAGES CXX)

include(CMakeDependentOption)
include(GNUInstallDirs)

set(UVFS_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(UVFS_IS_TOP_LEVEL ON)
endif()

option(UVFS_BUILD_TESTS "Build the uvfs test suite" ${UVFS_IS_TOP_LEVEL})
option(UVFS_BUILD_TOOLS "Build the uvfs command line tools" ${UVFS_IS_TOP_LEVEL})
option(UVFS_BUILD_BENCHMARKS "Build the uvfs benchmarks" OFF)
option(UVFS_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)
option(UVFS_LTO "Enable link-time optimization" OFF)
set(UVFS_SANITIZE "" CACHE STRING
    "Sanitizers to enable, e.g. 'address,undefined' (GCC/Clang only)")

# ------------------------------------------------------------------ warnings
# An interface target so flags apply to our targets only, and never leak into
# consumers or clobber whatever the user passed on the command line.
add_library(uvfs_build_settings INTERFACE)

if(MSVC)
  target_compile_options(uvfs_build_settings INTERFACE /W4 /permissive-)
else()
  target_compile_options(uvfs_build_settings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wcast-qual -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
  )
endif()

if(UVFS_SANITIZE AND NOT MSVC)
  target_compile_options(uvfs_build_settings INTERFACE
    -fsanitize=${UVFS_SANITIZE} -fno-omit-frame-pointer)
  target_link_options(uvfs_build_settings INTERFACE -fsanitize=${UVFS_SANITIZE})
endif()

if(UVFS_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# ---------------------------------------------------------------------- zstd
# Compression is optional at build time: the format encodes a codec per entry,
# so a build without zstd still reads and writes archives, and simply refuses
# entries it cannot decompress.
option(UVFS_WITH_ZSTD "Enable zstd compression support" ON)
set(UVFS_ZSTD_FOUND OFF)
if(UVFS_WITH_ZSTD)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(ZSTD QUIET IMPORTED_TARGET libzstd)
  endif()
  if(TARGET PkgConfig::ZSTD)
    set(UVFS_ZSTD_FOUND ON)
    add_library(uvfs_zstd INTERFACE)
    target_link_libraries(uvfs_zstd INTERFACE PkgConfig::ZSTD)
  else()
    find_path(ZSTD_INCLUDE_DIR zstd.h)
    find_library(ZSTD_LIBRARY NAMES zstd libzstd)
    if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
      set(UVFS_ZSTD_FOUND ON)
      add_library(uvfs_zstd INTERFACE)
      target_include_directories(uvfs_zstd SYSTEM INTERFACE ${ZSTD_INCLUDE_DIR})
      target_link_libraries(uvfs_zstd INTERFACE ${ZSTD_LIBRARY})
    endif()
  endif()
  if(NOT UVFS_ZSTD_FOUND)
    message(WARNING
      "uvfs: zstd not found; building without compression support. "
      "Archives using it will not be readable by this build.")
  endif()
endif()

# ------------------------------------------------------------------ 3rdparty
set(UVFS_UNORDERED_DENSE_INC
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/unordered_dense/include)
set(UVFS_XXHASH_INC ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/xxHash)

# ------------------------------------------------------------------- library
add_library(uvfs_reader STATIC
  src/reader.cpp
  src/fd_handle.hpp
  src/format.hpp
)
add_library(uvfs::reader ALIAS uvfs_reader)

target_compile_features(uvfs_reader PUBLIC cxx_std_20)
target_compile_definitions(uvfs_reader PRIVATE UVFS_BUILD_LIBRARY=1)
target_link_libraries(uvfs_reader PRIVATE $<BUILD_INTERFACE:uvfs_build_settings>)
target_include_directories(uvfs_reader SYSTEM PRIVATE ${UVFS_XXHASH_INC})
if(UVFS_ZSTD_FOUND)
  target_link_libraries(uvfs_reader PRIVATE $<BUILD_INTERFACE:uvfs_zstd>)
  target_compile_definitions(uvfs_reader PRIVATE UVFS_HAS_ZSTD=1)
endif()
target_include_directories(uvfs_reader PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_library(uvfs_writer STATIC
  src/writer.cpp
  src/fd_handle.hpp
  src/format.hpp
)
add_library(uvfs::writer ALIAS uvfs_writer)

target_compile_features(uvfs_writer PUBLIC cxx_std_20)
target_compile_definitions(uvfs_writer PRIVATE UVFS_BUILD_LIBRARY=1)
target_link_libraries(uvfs_writer PRIVATE $<BUILD_INTERFACE:uvfs_build_settings>)
target_include_directories(uvfs_writer SYSTEM PRIVATE ${UVFS_XXHASH_INC})
if(UVFS_ZSTD_FOUND)
  target_link_libraries(uvfs_writer PRIVATE $<BUILD_INTERFACE:uvfs_zstd>)
  target_compile_definitions(uvfs_writer PRIVATE UVFS_HAS_ZSTD=1)
endif()
target_include_directories(uvfs_writer PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

find_package(Threads REQUIRED)
target_link_libraries(uvfs_writer PUBLIC Threads::Threads)

# --------------------------------------------------------------------- tools
if(UVFS_BUILD_TOOLS)
  add_executable(uvfs src/archiver.cpp)
  target_link_libraries(uvfs PRIVATE uvfs_reader uvfs_writer $<BUILD_INTERFACE:uvfs_build_settings>)
endif()

# --------------------------------------------------------------------- tests
if(UVFS_BUILD_TESTS)
  enable_testing()
  add_executable(uvfs_tests
    tests/main.cpp
    tests/test_roundtrip.cpp
    tests/test_format.cpp
    tests/test_writer.cpp
    tests/test_paths.cpp
    tests/test_index.cpp
    tests/test_integrity.cpp
    tests/test_compression.cpp
    tests/test_dedup.cpp
    tests/test_oom.cpp
    tests/test_cleanup.cpp
    tests/test_enospc.cpp
  )
  target_link_libraries(uvfs_tests
    PRIVATE uvfs_reader uvfs_writer $<BUILD_INTERFACE:uvfs_build_settings>)
  if(UVFS_ZSTD_FOUND)
    target_compile_definitions(uvfs_tests PRIVATE UVFS_HAS_ZSTD=1)
  endif()
  # Tests reach into the private format header so that assertions about the
  # on-disk layout are written in terms of the real constants rather than
  # magic numbers that silently rot when the layout changes.
  target_include_directories(uvfs_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_include_directories(uvfs_tests SYSTEM PRIVATE ${UVFS_XXHASH_INC})
  # Runs one commit under an RLIMIT_AS cap in its own process, so that a
  # std::terminate inside a worker is observable instead of fatal to the suite.
  add_executable(uvfs_enospc_commit tests/enospc_commit.cpp)
  target_link_libraries(uvfs_enospc_commit
    PRIVATE uvfs_writer $<BUILD_INTERFACE:uvfs_build_settings>)
  add_dependencies(uvfs_tests uvfs_enospc_commit)
  target_compile_definitions(uvfs_tests PRIVATE
    UVFS_ENOSPC_HELPER="$<TARGET_FILE:uvfs_enospc_commit>")

  add_executable(uvfs_oom_commit tests/oom_commit.cpp)
  target_link_libraries(uvfs_oom_commit
    PRIVATE uvfs_writer $<BUILD_INTERFACE:uvfs_build_settings>)
  add_dependencies(uvfs_tests uvfs_oom_commit)
  target_compile_definitions(uvfs_tests PRIVATE
    UVFS_OOM_HELPER="$<TARGET_FILE:uvfs_oom_commit>")

  add_test(NAME uvfs_tests COMMAND uvfs_tests)
endif()

# ------------------------------------------------------------------- fuzzers
if(UVFS_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "uvfs: UVFS_BUILD_FUZZERS requires clang (libFuzzer)")
  endif()
  add_executable(uvfs_fuzz_reader tests/fuzz_reader.cpp)
  target_link_libraries(uvfs_fuzz_reader
    PRIVATE uvfs_reader $<BUILD_INTERFACE:uvfs_build_settings>)
  target_compile_options(uvfs_fuzz_reader PRIVATE -fsanitize=fuzzer)
  target_link_options(uvfs_fuzz_reader PRIVATE -fsanitize=fuzzer)
endif()

# ---------------------------------------------------------------- benchmarks
if(UVFS_BUILD_BENCHMARKS)
  add_executable(uvfs_bench src/bench.cpp)
  target_link_libraries(uvfs_bench
    PRIVATE uvfs_reader uvfs_writer $<BUILD_INTERFACE:uvfs_build_settings>)
  target_include_directories(uvfs_bench SYSTEM PRIVATE ${UVFS_UNORDERED_DENSE_INC})
endif()

# ------------------------------------------------------------------- install
if(UVFS_ZSTD_FOUND)
  install(TARGETS uvfs_zstd EXPORT uvfsTargets)
endif()

install(TARGETS uvfs_reader uvfs_writer uvfs_build_settings
  EXPORT uvfsTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/uvfs DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT uvfsTargets
  FILE uvfsTargets.cmake
  NAMESPACE uvfs::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uvfs)
