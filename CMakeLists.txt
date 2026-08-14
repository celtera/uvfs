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

# ------------------------------------------------------------------ 3rdparty
set(UVFS_UNORDERED_DENSE_INC
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/unordered_dense/include)
set(UVFS_RWQUEUE_INC ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/readerwriterqueue)

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
target_include_directories(uvfs_reader SYSTEM PRIVATE ${UVFS_UNORDERED_DENSE_INC})
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
target_include_directories(uvfs_writer SYSTEM PRIVATE ${UVFS_RWQUEUE_INC})
target_include_directories(uvfs_writer PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

find_package(Threads REQUIRED)
target_link_libraries(uvfs_writer PUBLIC Threads::Threads)

# --------------------------------------------------------------------- tools
if(UVFS_BUILD_TOOLS)
  add_executable(uvfs src/archiver.cpp)
  target_link_libraries(uvfs PRIVATE uvfs_writer $<BUILD_INTERFACE:uvfs_build_settings>)
endif()

# --------------------------------------------------------------------- tests
if(UVFS_BUILD_TESTS)
  enable_testing()
  add_executable(uvfs_tests
    tests/main.cpp
    tests/test_roundtrip.cpp
  )
  target_link_libraries(uvfs_tests
    PRIVATE uvfs_reader uvfs_writer $<BUILD_INTERFACE:uvfs_build_settings>)
  target_include_directories(uvfs_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME uvfs_tests COMMAND uvfs_tests)
endif()

# ---------------------------------------------------------------- benchmarks
if(UVFS_BUILD_BENCHMARKS)
  add_executable(uvfs_bench src/bench.cpp)
  target_link_libraries(uvfs_bench
    PRIVATE uvfs_reader uvfs_writer $<BUILD_INTERFACE:uvfs_build_settings>)
  target_include_directories(uvfs_bench SYSTEM PRIVATE ${UVFS_UNORDERED_DENSE_INC})
endif()

# ------------------------------------------------------------------- install
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
