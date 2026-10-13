cmake_minimum_required(VERSION 3.10)
project(M1 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # 关掉 GNU 扩展，保证可移植
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # 给 clangd / IDE 用

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()


# Sanitizer 开关（默认全关）
#
#   cmake -S . -B build-asan -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
#   cmake -S . -B build-tsan -DENABLE_TSAN=ON
#
# ⚠️ ASan 和 TSan 不能同时开 —— 两者都要接管内存分配。

option(ENABLE_ASAN  "AddressSanitizer（越界/use-after-free/泄漏）" OFF)
option(ENABLE_TSAN  "ThreadSanitizer（数据竞争）" OFF)
option(ENABLE_UBSAN "UndefinedBehaviorSanitizer（未定义行为）" OFF)

function(apply_sanitizers tgt)
    if(ENABLE_ASAN AND ENABLE_TSAN)
        message(FATAL_ERROR
            "ASan 和 TSan 不能同时启用，请分成两个构建目录：\n"
            "  cmake -S . -B build-asan -DENABLE_ASAN=ON\n"
            "  cmake -S . -B build-tsan -DENABLE_TSAN=ON")
    endif()

    if(ENABLE_ASAN)
        target_compile_options(${tgt} PRIVATE -fsanitize=address -g)
        target_link_options(${tgt} PRIVATE -fsanitize=address)
    endif()
    if(ENABLE_TSAN)
        target_compile_options(${tgt} PRIVATE -fsanitize=thread -g)
        target_link_options(${tgt} PRIVATE -fsanitize=thread)
    endif()
    if(ENABLE_UBSAN)
        target_compile_options(${tgt} PRIVATE
            -fsanitize=undefined -fno-sanitize-recover=undefined)
        target_link_options(${tgt} PRIVATE -fsanitize=undefined)
    endif()
endfunction()


# ★ 写过的源文件，每加一个 .cc 就在这里加一行
set(MUDUO_SRCS
    muduo/base/Timestamp.cc
    muduo/base/CurrentThread.cc
    muduo/base/Thread.cc
    muduo/base/ThreadPool.cc
    muduo/base/Logging.cc
)


# 想临时手动验证某个模块时，在这里临时加一个 add_executable，

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)      # std::thread 需要（测试里开线程）
enable_testing()

add_executable(unit_tests
    tests/test_timestamp.cc
    tests/test_atomic.cc
    tests/test_currentthread.cc
    tests/test_mutex.cc
    tests/test_condition.cc
    tests/test_countdownlatch.cc
    tests/test_producer_consumer.cc
    tests/test_thread.cc
    tests/test_blockingqueue.cc
    tests/test_threadpool.cc
    tests/test_logging.cc
    # 每写一个测试文件就在这里加一行：
    ${MUDUO_SRCS}
)
target_include_directories(unit_tests PRIVATE .)
target_compile_options(unit_tests PRIVATE -Wall -Wextra)
target_link_libraries(unit_tests PRIVATE GTest::gtest GTest::gtest_main Threads::Threads)
apply_sanitizers(unit_tests)

include(GoogleTest)
gtest_discover_tests(unit_tests PROPERTIES TIMEOUT 60)
