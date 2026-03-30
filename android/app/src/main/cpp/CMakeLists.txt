cmake_minimum_required(VERSION 3.22.1)

project(lkmdbg_native LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CAPSTONE_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTEST OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_LEGACY_TESTS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_INSTALL OFF CACHE BOOL "" FORCE)
set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
set(CAPSTONE_AARCH64_SUPPORT ON CACHE BOOL "" FORCE)

set(CAPSTONE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party/capstone)
add_subdirectory(${CAPSTONE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/capstone)

set(BUILD_LIBS_ONLY ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(KEYSTONE_BUILD_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
set(LLVM_TARGETS_TO_BUILD "AArch64" CACHE STRING "" FORCE)

set(KEYSTONE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party/keystone)
add_subdirectory(${KEYSTONE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/keystone)

add_library(lkmdbg_native SHARED
	native_disassembler.cpp
	native_workspace_renderer.cpp)
target_compile_definitions(lkmdbg_native PRIVATE CAPSTONE_STATIC)
target_include_directories(lkmdbg_native PRIVATE ${KEYSTONE_DIR}/include)
target_link_libraries(lkmdbg_native PRIVATE capstone_static keystone android jnigraphics log)
