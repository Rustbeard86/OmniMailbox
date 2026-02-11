cmake_minimum_required(VERSION 3.20)
project(omni-mailbox VERSION 1.0.0 LANGUAGES CXX)

# C++23 with fallback to C++20
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)  # Allow fallback
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(OMNI_BUILD_TESTS "Build unit tests" ON)
option(OMNI_BUILD_EXAMPLES "Build examples" OFF)
option(OMNI_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(OMNI_ENABLE_SANITIZERS "Enable ASAN/TSAN/UBSAN" OFF)
option(OMNI_HEADER_ONLY "Header-only mode" OFF)

# Platform detection
if(WIN32)
    add_compile_definitions(OMNI_PLATFORM_WINDOWS)
elseif(UNIX)
    add_compile_definitions(OMNI_PLATFORM_LINUX)
endif()

# Dependencies - Use FetchContent to automatically download
include(FetchContent)

# Flatbuffers
FetchContent_Declare(
    flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG v24.3.25
    GIT_SHALLOW TRUE
)
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(flatbuffers)

# Library target
if(OMNI_HEADER_ONLY)
    add_library(omni-mailbox INTERFACE)
    target_include_directories(omni-mailbox INTERFACE include/)
else()
    add_library(omni-mailbox STATIC
        src/broker.cpp
        src/producer_handle.cpp
        src/consumer_handle.cpp
    )
    target_include_directories(omni-mailbox PUBLIC include/)
    target_link_libraries(omni-mailbox PUBLIC flatbuffers)
endif()

# Compiler warnings (strict)
if(MSVC)
    target_compile_options(omni-mailbox PRIVATE /W4 /WX)
else()
    target_compile_options(omni-mailbox PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wno-unused-parameter  # Common in interface methods
    )
endif()

# Sanitizers
if(OMNI_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(omni-mailbox PRIVATE
        -fsanitize=address,thread,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(omni-mailbox PRIVATE
        -fsanitize=address,thread,undefined
    )
endif()

# Tests
if(OMNI_BUILD_TESTS)
    enable_testing()
    
    # Fetch GoogleTest
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    
    add_executable(omni-tests
        tests/unit/test_config.cpp
        tests/unit/test_spsc_queue.cpp
        tests/unit/test_broker.cpp
        tests/unit/test_handles.cpp
        tests/unit/test_consumer_handle.cpp
        tests/integration/test_end_to_end.cpp
    )
    target_link_libraries(omni-tests PRIVATE
        omni-mailbox
        GTest::gtest_main
    )
    
    add_test(NAME omni-unit-tests COMMAND omni-tests)
endif()

# Examples
if(OMNI_BUILD_EXAMPLES)
    add_executable(example-basic examples/basic_usage.cpp)
    target_link_libraries(example-basic PRIVATE omni-mailbox)
    
    add_executable(example-backpressure examples/backpressure_demo.cpp)
    target_link_libraries(example-backpressure PRIVATE omni-mailbox)
    
    # TODO: Create flatbuffers example
    # add_executable(example-flatbuffers examples/flatbuffers_telemetry.cpp)
    # target_link_libraries(example-flatbuffers PRIVATE omni-mailbox)
endif()

# Benchmarks
if(OMNI_BUILD_BENCHMARKS)
    # Fetch Google Benchmark
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.1
        GIT_SHALLOW TRUE
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
    
    add_executable(bench-throughput bench/throughput_latency.cpp)
    target_link_libraries(bench-throughput PRIVATE
        omni-mailbox
        benchmark::benchmark
    )
endif()

# Install
install(TARGETS omni-mailbox
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/omni DESTINATION include)
