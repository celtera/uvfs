cmake_minimum_required(VERSION 3.14)

project(uvfs LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-Wall -Wextra -flto")

# Libraries
add_library(uvfs_reader STATIC
  src/reader.cpp
  src/fd_handle.hpp
  src/format.hpp
)

target_compile_definitions(uvfs_reader PRIVATE $<BUILD_INTERFACE:UVFS_BUILD_LIBRARY=1>)
target_include_directories(uvfs_reader PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/unordered_dense/include>)
target_include_directories(uvfs_reader PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

add_library(uvfs_writer STATIC
  src/writer.cpp
  src/fd_handle.hpp
  src/format.hpp
)

target_compile_definitions(uvfs_writer PRIVATE $<BUILD_INTERFACE:UVFS_BUILD_LIBRARY=1>)
target_include_directories(uvfs_writer PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/readerwriterqueue>)
target_include_directories(uvfs_writer PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# Archiver app
add_executable(uvfs src/archiver.cpp)
target_link_libraries(uvfs PRIVATE uvfs_writer)

# Test app
add_executable(uvfs_test src/test.cpp)
target_link_libraries(uvfs_test PRIVATE uvfs_reader uvfs_writer)
