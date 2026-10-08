cmake_minimum_required(VERSION 3.10)
project(mini-muduo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_executable(scratch
    scratch/main.cc
    muduo/base/Timestamp.cc
)

target_include_directories(scratch PRIVATE .)
target_compile_options(scratch PRIVATE -Wall -Wextra)
