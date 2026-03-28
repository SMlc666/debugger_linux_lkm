cmake_minimum_required(VERSION 3.22)
project(lkmdbg_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(lkmdbg-agent
    src/main/cpp/main.cpp
)

target_include_directories(lkmdbg-agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
