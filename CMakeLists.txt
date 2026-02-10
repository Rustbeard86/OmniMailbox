cmake_minimum_required(VERSION 3.20)
project(omni-mailbox VERSION 1.0.0 LANGUAGES CXX)

# C++23 with fallback to C++20
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)  # Allow fallback
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(OMNI_BUILD_TESTS "Build unit tests" ON)
option(OMNI_BUILD_EXAMPLES "Build examples" ON)
option(OMNI_BUILD_BENCHMARKS "Build benchmarks" ON)
option(OMNI_ENABLE_SANITIZERS "Enable ASAN/TSAN/UBSAN" OFF)
option(OMNI_HEADER_ONLY "Header-only mode" OFF)

# Platform detection
if(WIN32)
    add_compile_definitions(OMNI_PLATFORM_WINDOWS)
elseif(UNIX)
    add_compile_definitions(OMNI_PLATFORM_LINUX)
endif()

# Dependencies
find_package(flatbuffers REQUIRED)

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
    target_link_libraries(omni-mailbox PUBLIC flatbuffers::flatbuffers)
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
    find_package(GTest REQUIRED)
    
    add_executable(omni-tests
        tests/unit/test_spsc_queue.cpp
        tests/unit/test_broker.cpp
        tests/unit/test_handles.cpp
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
    
    add_executable(example-flatbuffers examples/flatbuffers_telemetry.cpp)
    target_link_libraries(example-flatbuffers PRIVATE omni-mailbox)
endif()

# Benchmarks
if(OMNI_BUILD_BENCHMARKS)
    find_package(benchmark REQUIRED)
    
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
