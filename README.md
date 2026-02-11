# OmniMailbox

A high-performance C++23 lock-free SPSC (Single Producer Single Consumer) messaging library designed for ultra-low latency inter-thread communication.

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![CMake](https://img.shields.io/badge/CMake-3.20+-green.svg)](https://cmake.org/)

## Overview

OmniMailbox is a modern C++ messaging library that provides blazing-fast, lock-free communication channels between threads. Built on proven SPSC (Single Producer Single Consumer) queue algorithms, it delivers predictable performance for real-time applications, high-frequency trading systems, game engines, and any scenario where microsecond-level latency matters.

### Key Features

- **Lock-Free Design**: Zero mutex overhead using atomic operations with carefully tuned memory ordering
- **Exceptional Performance**: 
  - **24M+ messages/sec** throughput (64-byte messages)
  - **207ns p50** and **213ns p99** round-trip latency
  - **22 GiB/s** peak bandwidth (4KB messages)
- **Type-Safe**: Strongly-typed handles prevent SPSC violations at compile time
- **Zero-Copy**: Direct buffer access with FlatBuffers integration for serialization-free messaging
- **Flexible APIs**: Blocking, non-blocking, timeout-based, and batch operations
- **Robust Error Handling**: Comprehensive result codes for production reliability
- **Thread-Safe Lifecycle**: Automatic cleanup with RAII handles and safe destruction semantics
- **Cross-Platform**: Windows and Linux support with CMake build system
- **Modern C++23**: Leverages latest language features for safety and performance

### Use Cases

- Real-time audio/video processing pipelines
- High-frequency trading order routing
- Game engine actor systems
- IoT sensor data aggregation
- Logging and telemetry frameworks
- Producer-consumer task queues

## Performance Characteristics

Benchmarked on AMD Ryzen 9 7950X (see [BENCHMARKS.md](BENCHMARKS.md) for full details):

| Metric | Result |
|--------|--------|
| **Throughput (64B)** | 24.05M msg/sec (4.8x design target) |
| **Throughput (4KB)** | 5.86M msg/sec (22.34 GiB/s) |
| **Latency p50** | 207ns |
| **Latency p99** | 213ns (57% below 500ns target) |
| **Latency Variance** | 3.82ns stddev (1.84% CV) |

Performance remains excellent across all message sizes with minimal degradation.

## Quick Start

### Basic Example

```cpp
#include <omni/mailbox.hpp>
#include <thread>
#include <iostream>

int main() {
    auto& broker = omni::MailboxBroker::Instance();
    
    auto [error, channel] = broker.RequestChannel("my-channel", {
        .capacity = 1024,
        .max_message_size = 256
    });
    
    if (error != omni::ChannelError::Success) {
        std::cerr << "Failed to create channel\n";
        return 1;
    }
    
    std::thread consumer([&]() {
        auto& consumer = channel->consumer;
        
        while (true) {
            auto [result, msg] = consumer.BlockingPop();
            
            if (result == omni::PopResult::Success) {
                auto data = msg->Data();
                std::cout << "Received: " << data.size() << " bytes\n";
            } else if (result == omni::PopResult::ChannelClosed) {
                break;
            }
        }
    });
    
    std::thread producer([&]() {
        auto& producer = channel->producer;
        
        for (int i = 0; i < 100; ++i) {
            std::string message = "Hello #" + std::to_string(i);
            std::span<const uint8_t> data(
                reinterpret_cast<const uint8_t*>(message.data()),
                message.size()
            );
            
            producer.TryPush(data);
        }
    });
    
    producer.join();
    consumer.join();
    
    return 0;
}
```

### Zero-Copy with FlatBuffers

```cpp
#include <omni/mailbox.hpp>
#include <flatbuffers/flatbuffers.h>
#include "my_message_generated.h"  // Generated from .fbs schema

// Producer: Reserve buffer and build in-place
auto res = producer.Reserve(256);
if (res) {
    flatbuffers::FlatBufferBuilder fbb(res->capacity, res->data, false);
    auto msg = CreateMyMessage(fbb, 42, fbb.CreateString("data"));
    fbb.Finish(msg);
    producer.Commit(fbb.GetSize());
}

// Consumer: Zero-copy access to FlatBuffer
auto [result, msg] = consumer.TryPop();
if (result == omni::PopResult::Success) {
    auto fb = msg->GetFlatBuffer<MyMessage>();
    std::cout << "ID: " << fb->id() << ", Data: " << fb->data()->str() << "\n";
}
```

## Installation

### Requirements

- C++23 compiler (MSVC 19.35+, GCC 12+, Clang 16+)
- CMake 3.20 or higher
- FlatBuffers (automatically fetched by CMake)

### Using FetchContent (Recommended)

Add to your `CMakeLists.txt`:

```cmake
include(FetchContent)

FetchContent_Declare(
    omni-mailbox
    GIT_REPOSITORY https://github.com/Rustbeard86/OmniMailbox.git
    GIT_TAG main
)
FetchContent_MakeAvailable(omni-mailbox)

target_link_libraries(your_target PRIVATE omni::mailbox)
```

### Manual Installation

```bash
git clone https://github.com/Rustbeard86/OmniMailbox.git
cd OmniMailbox
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /your/install/path
```

Then in your project:

```cmake
find_package(omni-mailbox REQUIRED)
target_link_libraries(your_target PRIVATE omni::mailbox)
```

## Building from Source

### Basic Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Build Options

```bash
# Enable examples
cmake -B build -S . -DOMNI_BUILD_EXAMPLES=ON

# Enable tests (requires GTest)
cmake -B build -S . -DOMNI_BUILD_TESTS=ON

# Enable benchmarks (requires Google Benchmark)
cmake -B build -S . -DOMNI_BUILD_BENCHMARKS=ON

# Enable sanitizers (ASAN/TSAN/UBSAN)
cmake -B build -S . -DOMNI_ENABLE_SANITIZERS=ON
```

### Platform-Specific

**Windows (Visual Studio):**
```powershell
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**Linux (Ninja):**
```bash
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Running Tests

```bash
cmake -B build -S . -DOMNI_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Or run test executables directly:
```bash
./build/tests/omni-tests
```

## Usage Examples

See the [examples](examples/) directory for complete demonstrations:

- **[basic_usage.cpp](examples/basic_usage.cpp)**: Simple producer-consumer setup
- **[backpressure_demo.cpp](examples/backpressure_demo.cpp)**: Handling queue saturation

Run examples:
```bash
cmake -B build -S . -DOMNI_BUILD_EXAMPLES=ON
cmake --build build
./build/examples/basic_usage
./build/examples/backpressure_demo
```

## API Overview

### Broker

```cpp
auto& broker = omni::MailboxBroker::Instance();
auto [error, channel] = broker.RequestChannel("name", config);
```

### Producer Operations

```cpp
producer.TryPush(data);                    // Non-blocking
producer.BlockingPush(data, timeout);      // Blocking
producer.BatchPush(messages);              // Batch send
auto res = producer.Reserve(bytes);        // Zero-copy reserve
producer.Commit(bytes);                    // Commit reserved
producer.AvailableSlots();                 // Check capacity
```

### Consumer Operations

```cpp
consumer.TryPop();                         // Non-blocking
consumer.BlockingPop(timeout);             // Blocking
consumer.TryPopBatch(count);               // Batch receive
consumer.AvailableMessages();              // Check available
```

### Configuration

```cpp
omni::ChannelConfig config{
    .capacity = 1024,            // Power-of-2 ring buffer size
    .max_message_size = 4096     // Max bytes per message
};
```

## Documentation

- **[Design Specification](OmniMailbox_Design_Specification.md)**: Architecture, algorithms, memory ordering
- **[Benchmarks](BENCHMARKS.md)**: Performance analysis and validation
- **API Reference**: See header files in [include/omni](include/omni/)

## Contributing

Contributions are welcome! Please:

1. Follow the existing code style (see [.github/copilot-instructions.md](.github/copilot-instructions.md))
2. Add tests for new features
3. Run sanitizers (ASAN/TSAN/UBSAN) before submitting
4. Update documentation as needed

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Lock-free queue algorithm inspired by the Disruptor pattern
- FlatBuffers team for zero-copy serialization
- Google Benchmark and Google Test frameworks

## Contact

- **Author**: Rustbeard86
- **Repository**: https://github.com/Rustbeard86/OmniMailbox
- **Issues**: https://github.com/Rustbeard86/OmniMailbox/issues
