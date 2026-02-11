# OmniMailbox API Reference

**Version 1.0**  
**C++23 Lock-Free Intra-Process Messaging Library**

---

## Table of Contents

1. [Overview](#1-overview)
2. [Core Types](#2-core-types)
3. [ChannelConfig API](#3-channelconfig-api)
4. [MailboxBroker API](#4-mailboxbroker-api)
5. [ProducerHandle API](#5-producerhandle-api)
6. [ConsumerHandle API](#6-consumerhandle-api)
7. [Error Handling Guide](#7-error-handling-guide)
8. [Thread Safety Guarantees](#8-thread-safety-guarantees)
9. [Performance Tips](#9-performance-tips)

---

## 1. Overview

OmniMailbox provides high-performance, lock-free message passing between threads using:

- **Lock-Free SPSC Queues**: Single Producer Single Consumer ring buffers
- **Zero-Copy Design**: Messages accessed via `std::span` views
- **FlatBuffers Integration**: Native support for structured messages
- **RAII Handles**: Automatic cleanup and lifecycle management
- **Singleton Broker**: Centralized channel management

### Quick Start

```cpp
#include <omni/mailbox.hpp>

// 1. Get broker singleton
auto& broker = omni::MailboxBroker::Instance();

// 2. Create channel
auto [error, channel] = broker.RequestChannel("my-channel", {
    .capacity = 2048,
    .max_message_size = 4096
});

if (error != omni::ChannelError::Success) {
    // Handle error
    return;
}

// 3. Use handles in separate threads
std::thread producer_thread([&]() {
    std::vector<uint8_t> msg = {1, 2, 3, 4};
    channel->producer.TryPush(msg);
});

std::thread consumer_thread([&]() {
    auto [result, msg] = channel->consumer.BlockingPop();
    if (result == omni::PopResult::Success) {
        // Process msg->Data()
    }
});

producer_thread.join();
consumer_thread.join();
```

---

## 2. Core Types

### 2.1 Enumerations

#### `PushResult`

Operation results for producer operations.

```cpp
enum class PushResult {
    Success,        // Message sent successfully
    Timeout,        // BlockingPush timed out
    ChannelClosed,  // Consumer destroyed
    InvalidSize,    // Message > max_message_size
    QueueFull       // TryPush only: no space available
};
```

#### `PopResult`

Operation results for consumer operations.

```cpp
enum class PopResult {
    Success,        // Message received successfully
    Timeout,        // BlockingPop timed out
    ChannelClosed,  // Producer destroyed
    Empty           // TryPop only: no messages available
};
```

#### `ChannelError`

Channel creation errors.

```cpp
enum class ChannelError {
    Success,            // Channel created successfully
    NameExists,         // Channel with this name already exists
    InvalidConfig,      // Config validation failed
    AllocationFailed    // Memory allocation failed
};
```

### 2.2 Structures

#### `ChannelPair`

Pair of producer and consumer handles for a channel.

```cpp
struct ChannelPair {
    ProducerHandle producer;
    ConsumerHandle consumer;
};
```

**Properties:**
- Both handles are **move-only** (non-copyable)
- Enforces SPSC semantics at compile-time
- Moved-from handles are valid but inactive (safe to destroy)

**Example:**

```cpp
auto [error, channel] = broker.RequestChannel("test");

// Move handles separately
auto producer = std::move(channel->producer);
auto consumer = std::move(channel->consumer);

// Moved-from handles are inactive but safe
auto result = channel->producer.TryPush(data);
assert(result == PushResult::ChannelClosed);  // Expected behavior
```

---

## 3. ChannelConfig API

### 3.1 Configuration Structure

```cpp
struct ChannelConfig {
    size_t capacity = 1024;          // Ring buffer capacity (slots)
    size_t max_message_size = 4096;  // Maximum message size (bytes)
};
```

### 3.2 Valid Ranges

| Parameter | Min | Max | Notes |
|-----------|-----|-----|-------|
| `capacity` | 8 | 524,288 | Rounded up to next power of 2 |
| `max_message_size` | 64 | 16,777,216 (16 MB) | Exact value used |

### 3.3 Methods

#### `Normalize()`

Validates and normalizes configuration to meet constraints.

```cpp
[[nodiscard]] ChannelConfig Normalize() const noexcept;
```

**Behavior:**
1. Clamps `capacity` to [8, 524288]
2. Clamps `max_message_size` to [64, 16777216]
3. Rounds `capacity` up to next power of 2
4. Returns normalized copy (original unchanged)

**Example:**

```cpp
ChannelConfig config{
    .capacity = 1000,  // Not power of 2
    .max_message_size = 4096
};

auto normalized = config.Normalize();
// normalized.capacity == 1024 (next power of 2)
// normalized.max_message_size == 4096 (unchanged)
```

#### `IsValid()`

Checks if configuration meets validity constraints.

```cpp
[[nodiscard]] bool IsValid() const noexcept;
```

**Returns:** `true` if all parameters within valid ranges (after normalization)

**Example:**

```cpp
ChannelConfig config{.capacity = 2000, .max_message_size = 4096};

if (!config.IsValid()) {
    config = config.Normalize();
}
```

### 3.4 Examples

#### Default Configuration

```cpp
auto [error, channel] = broker.RequestChannel("default");
// Uses: capacity = 1024, max_message_size = 4096
```

#### Custom Configuration

```cpp
auto [error, channel] = broker.RequestChannel("custom", {
    .capacity = 4096,       // Will use 4096 (already power of 2)
    .max_message_size = 8192
});
```

#### High-Throughput Configuration

```cpp
auto [error, channel] = broker.RequestChannel("high-throughput", {
    .capacity = 65536,      // Large buffer
    .max_message_size = 256 // Small messages
});
// Optimized for many small messages
```

#### Large Message Configuration

```cpp
auto [error, channel] = broker.RequestChannel("large-messages", {
    .capacity = 128,        // Fewer slots
    .max_message_size = 1048576  // 1 MB messages
});
// Optimized for few large messages
```

---

## 4. MailboxBroker API

Singleton dispatcher for managing named channels.

### 4.1 Instance Access

#### `Instance()`

Get the global broker singleton.

```cpp
[[nodiscard]] static MailboxBroker& Instance() noexcept;
```

**Thread Safety:** Thread-safe lazy initialization (C++11 guarantee)

**Lifetime:** Singleton persists until program termination (intentional leak)

**Example:**

```cpp
auto& broker = omni::MailboxBroker::Instance();
```

### 4.2 Channel Management

#### `RequestChannel()`

Create a new channel or return error if name exists.

```cpp
[[nodiscard]] std::pair<ChannelError, std::optional<ChannelPair>> 
RequestChannel(
    std::string_view name,
    const ChannelConfig& config = {}
) noexcept;
```

**Parameters:**
- `name`: Unique channel identifier (non-empty)
- `config`: Channel configuration (auto-normalized)

**Returns:** Pair of `(error_code, optional_channel)`

**Error Conditions:**
- `NameExists`: Channel with this name already exists
- `InvalidConfig`: Config invalid even after normalization
- `AllocationFailed`: Memory allocation failed

**Thread Safety:** Write lock (shared_mutex)

**Performance:** O(1) average, O(n) worst-case (hash collision)

**Example - Basic Usage:**

```cpp
auto [error, channel] = broker.RequestChannel("my-channel");

if (error == ChannelError::Success) {
    // Use channel.value()
    auto& producer = channel->producer;
    auto& consumer = channel->consumer;
} else {
    std::cerr << "Failed to create channel\n";
}
```

**Example - Error Handling:**

```cpp
auto [error, channel] = broker.RequestChannel("data-feed", {
    .capacity = 2048,
    .max_message_size = 4096
});

switch (error) {
    case ChannelError::Success:
        std::cout << "Channel created successfully\n";
        break;
    
    case ChannelError::NameExists:
        std::cerr << "Channel name already in use\n";
        // Option 1: Choose different name
        // Option 2: Use existing channel (if intentional)
        break;
    
    case ChannelError::InvalidConfig:
        std::cerr << "Invalid configuration\n";
        // Fix config and retry
        break;
    
    case ChannelError::AllocationFailed:
        std::cerr << "Out of memory\n";
        // Free resources and retry
        break;
}
```

**Example - Unique Names:**

```cpp
// Generate unique names for multiple channels
for (size_t i = 0; i < 10; ++i) {
    std::string name = "channel-" + std::to_string(i);
    auto [error, channel] = broker.RequestChannel(name);
    
    if (error == ChannelError::Success) {
        // Store handles for later use
        producers.push_back(std::move(channel->producer));
        consumers.push_back(std::move(channel->consumer));
    }
}
```

#### `HasChannel()`

Check if a channel exists.

```cpp
[[nodiscard]] bool HasChannel(std::string_view name) const noexcept;
```

**Parameters:**
- `name`: Channel identifier to check

**Returns:** `true` if channel exists, `false` otherwise

**Thread Safety:** Read lock (multiple threads can call simultaneously)

**Note:** Result may be stale if another thread removes the channel concurrently

**Example:**

```cpp
if (broker.HasChannel("my-channel")) {
    std::cout << "Channel exists\n";
} else {
    // Create channel
    auto [error, channel] = broker.RequestChannel("my-channel");
}
```

#### `RemoveChannel()`

Remove a channel if no active handles exist.

```cpp
bool RemoveChannel(std::string_view name) noexcept;
```

**Parameters:**
- `name`: Channel identifier to remove

**Returns:** 
- `true`: Channel removed successfully
- `false`: Handles still alive or channel doesn't exist

**Thread Safety:** Write lock

**Note:** Channel is automatically removed when all handles destroyed

**Example:**

```cpp
// Manual cleanup
{
    auto [error, channel] = broker.RequestChannel("temp");
    // Use channel...
}  // Handles destroyed here

// Channel auto-removed, but can force:
broker.RemoveChannel("temp");  // Returns false (already removed)
```

#### `GetStats()`

Get broker statistics.

```cpp
struct Stats {
    size_t active_channels;
    size_t total_channels_created;
    size_t total_messages_sent;
    size_t total_bytes_transferred;
};

[[nodiscard]] Stats GetStats() const noexcept;
```

**Thread Safety:** Relaxed atomics (approximate counts)

**Example:**

```cpp
auto stats = broker.GetStats();
std::cout << "Active channels: " << stats.active_channels << "\n"
          << "Total messages: " << stats.total_messages_sent << "\n"
          << "Total bytes: " << stats.total_bytes_transferred << "\n";
```

#### `Shutdown()`

Shutdown all channels and wait for handles to be released.

```cpp
void Shutdown() noexcept;
```

**Behavior:** Blocks until all handles destroyed

**Warning:** Deadlock risk if called while holding handles

**Example:**

```cpp
// Proper shutdown sequence
void shutdown_system() {
    // 1. Signal all threads to stop
    shutdown_flag.store(true);
    
    // 2. Wait for threads to finish
    for (auto& thread : worker_threads) {
        thread.join();
    }
    
    // 3. All handles destroyed by now, safe to shutdown
    broker.Shutdown();
}
```

### 4.3 Important Warnings

#### ⚠️ Global Handles Forbidden

```cpp
// ❌ INCORRECT: Global static handles
static ProducerHandle g_producer = broker.RequestChannel("bad")->producer;

// ✅ CORRECT: Explicit lifetime management
std::unique_ptr<ProducerHandle> g_producer;

void init() {
    auto [error, channel] = broker.RequestChannel("good");
    g_producer = std::make_unique<ProducerHandle>(
        std::move(channel->producer)
    );
}

void cleanup() {
    g_producer.reset();  // Explicit destruction before main() exits
}
```

#### ⚠️ Not Signal-Safe (POSIX)

```cpp
// ❌ INCORRECT: Calling from signal handler
void signal_handler(int) {
    MailboxBroker::Instance().Shutdown();  // Deadlock risk!
}

// ✅ CORRECT: Use atomic flags
std::atomic<bool> shutdown_requested{false};

void signal_handler(int) {
    shutdown_requested.store(true, std::memory_order_release);
}

int main() {
    signal(SIGTERM, signal_handler);
    
    while (!shutdown_requested.load(std::memory_order_acquire)) {
        // Process messages
    }
    
    broker.Shutdown();  // Safe in main thread
}
```

---

## 5. ProducerHandle API

Handle for sending messages to a channel (move-only, SPSC enforced).

### 5.1 Blocking Operations

#### `BlockingPush()`

Send message with timeout, blocking until space available.

```cpp
[[nodiscard]] PushResult BlockingPush(
    std::span<const uint8_t> data,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max()
) noexcept;
```

**Parameters:**
- `data`: Message data to send (copied into ring buffer)
- `timeout`: Maximum wait time (default: infinite)

**Returns:** `PushResult` status

**Preconditions:**
- `!data.empty()`
- `data.size() <= max_message_size`

**Blocks:** Until space available or timeout

**Wakes:** On consumer `Pop()` or timeout

**Example - Infinite Wait:**

```cpp
std::vector<uint8_t> message = {1, 2, 3, 4, 5};
auto result = producer.BlockingPush(message);

if (result == PushResult::Success) {
    std::cout << "Message sent\n";
}
```

**Example - With Timeout:**

```cpp
auto result = producer.BlockingPush(message, std::chrono::seconds(1));

switch (result) {
    case PushResult::Success:
        std::cout << "Message sent\n";
        break;
    
    case PushResult::Timeout:
        std::cout << "Queue full for 1 second, timeout\n";
        break;
    
    case PushResult::ChannelClosed:
        std::cout << "Consumer disconnected\n";
        break;
    
    case PushResult::InvalidSize:
        std::cout << "Message too large\n";
        break;
}
```

**Example - Convenience Overload (std::vector):**

```cpp
std::vector<uint8_t> msg = create_message();
auto result = producer.BlockingPush(msg);  // Implicit conversion to span
```

### 5.2 Non-Blocking Operations

#### `TryPush()`

Attempt to send message without blocking.

```cpp
[[nodiscard]] PushResult TryPush(std::span<const uint8_t> data) noexcept;
```

**Parameters:**
- `data`: Message data to send

**Returns:** 
- `Success`: Message sent
- `QueueFull`: No space available (immediate return)
- `ChannelClosed`: Consumer destroyed
- `InvalidSize`: Message too large

**Preconditions:** Same as `BlockingPush()`

**Does Not Block:** Returns immediately

**Example:**

```cpp
auto result = producer.TryPush(message);

if (result == PushResult::Success) {
    // Message sent
} else if (result == PushResult::QueueFull) {
    // Handle backpressure:
    // Option 1: Drop message
    // Option 2: Retry with timeout
    // Option 3: Switch to blocking
    auto retry = producer.BlockingPush(message, std::chrono::milliseconds(100));
}
```

**Example - Non-Blocking Loop:**

```cpp
while (has_more_messages()) {
    auto msg = get_next_message();
    
    if (producer.TryPush(msg) == PushResult::QueueFull) {
        // Queue saturated, rate limit
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}
```

#### `BatchPush()`

Send multiple messages efficiently (amortizes atomic overhead).

```cpp
[[nodiscard]] size_t BatchPush(
    std::span<const std::span<const uint8_t>> messages
) noexcept;
```

**Parameters:**
- `messages`: Span of message data spans

**Returns:** Number of messages successfully pushed [0, messages.size()]

**Behavior:** Pushes messages sequentially until queue full or consumer closes

**Performance:** 10-100x faster than individual `TryPush()` calls for high-throughput

**Example:**

```cpp
std::vector<std::vector<uint8_t>> messages;
for (int i = 0; i < 1000; ++i) {
    messages.push_back({1, 2, 3, 4});
}

// Convert to spans
std::vector<std::span<const uint8_t>> spans;
for (const auto& msg : messages) {
    spans.emplace_back(msg.data(), msg.size());
}

// Send batch (single atomic overhead)
size_t sent = producer.BatchPush(spans);
std::cout << "Sent " << sent << "/" << spans.size() << " messages\n";
```

**Example - High-Throughput Pipeline:**

```cpp
constexpr size_t BATCH_SIZE = 100;
std::vector<std::span<const uint8_t>> batch;
batch.reserve(BATCH_SIZE);

while (running) {
    // Accumulate messages
    while (batch.size() < BATCH_SIZE && has_data()) {
        batch.push_back(get_message());
    }
    
    // Flush batch
    if (!batch.empty()) {
        size_t sent = producer.BatchPush(batch);
        batch.erase(batch.begin(), batch.begin() + sent);
    }
}
```

### 5.3 Zero-Copy Operations (FlatBuffers)

#### `Reserve()`

Reserve space in ring buffer for zero-copy construction.

```cpp
struct ReserveResult {
    uint8_t* data;       // Pointer to write region
    size_t capacity;     // Available bytes (>= requested)
};

[[nodiscard]] std::optional<ReserveResult> Reserve(size_t bytes) noexcept;
```

**Parameters:**
- `bytes`: Number of bytes to reserve

**Returns:** Optional reserve result (nullopt on failure)

**Preconditions:**
- `bytes > 0`
- `bytes <= max_message_size`
- `IsConnected() == true`
- No previous reservation uncommitted

**Postcondition:** Must call `Commit(actual_bytes)` before next `Reserve()`

**Error Conditions:**
- Queue full
- Consumer disconnected
- Previous reservation not committed

**Example - FlatBuffers Integration:**

```cpp
// 1. Reserve space
auto reserve = producer.Reserve(256);
if (!reserve) {
    // Handle error
    return;
}

// 2. Build FlatBuffer directly into reserved space
flatbuffers::FlatBufferBuilder fbb(
    reserve->capacity,  // Use reserved capacity
    reserve->data,      // Use reserved pointer
    false               // Don't own memory
);

// 3. Build message
auto name = fbb.CreateString("Alice");
auto msg = CreateMessage(fbb, name, 42);
fbb.Finish(msg);

// 4. Commit actual size
producer.Commit(fbb.GetSize());
```

#### `Commit()`

Commit reserved space after writing.

```cpp
bool Commit(size_t actual_bytes) noexcept;
```

**Parameters:**
- `actual_bytes`: Number of bytes actually written

**Returns:** `true` on success, `false` on error

**Preconditions:**
- `Reserve()` previously succeeded
- `actual_bytes > 0`
- `actual_bytes <= ReserveResult::capacity`

**Postcondition:** Message visible to consumer

**Example:**

```cpp
auto reserve = producer.Reserve(100);
if (reserve) {
    // Write data
    std::memcpy(reserve->data, source, 50);
    
    // Commit actual size (may be less than reserved)
    producer.Commit(50);
}
```

#### `Rollback()`

Abort reservation without sending.

```cpp
void Rollback() noexcept;
```

**Behavior:** Cancels current reservation, no message sent

**Example:**

```cpp
auto reserve = producer.Reserve(256);
if (reserve) {
    if (!try_build_message(reserve->data)) {
        // Build failed, abort
        producer.Rollback();
    } else {
        producer.Commit(actual_size);
    }
}
```

### 5.4 Query Methods

#### `IsConnected()`

Check if consumer is alive.

```cpp
[[nodiscard]] bool IsConnected() const noexcept;
```

**Returns:** `true` if consumer handle exists, `false` otherwise

**Memory Order:** Relaxed (approximate)

**Example:**

```cpp
if (!producer.IsConnected()) {
    std::cout << "Consumer disconnected, stopping producer\n";
    return;
}
```

#### `Capacity()`

Get ring buffer capacity.

```cpp
[[nodiscard]] size_t Capacity() const noexcept;
```

**Returns:** Number of message slots

**Example:**

```cpp
std::cout << "Queue capacity: " << producer.Capacity() << " slots\n";
```

#### `MaxMessageSize()`

Get maximum message size.

```cpp
[[nodiscard]] size_t MaxMessageSize() const noexcept;
```

**Returns:** Maximum bytes per message

**Example:**

```cpp
if (message.size() > producer.MaxMessageSize()) {
    std::cerr << "Message too large\n";
}
```

#### `AvailableSlots()`

Get approximate free space.

```cpp
[[nodiscard]] size_t AvailableSlots() const noexcept;
```

**Returns:** Approximate number of free slots

**Memory Order:** Relaxed (may be stale)

**Example:**

```cpp
if (producer.AvailableSlots() < 10) {
    std::cout << "Queue nearly full, slowing down\n";
}
```

#### `GetConfig()`

Get normalized channel configuration.

```cpp
[[nodiscard]] ChannelConfig GetConfig() const noexcept;
```

**Returns:** Configuration used for this channel

**Example:**

```cpp
auto config = producer.GetConfig();
std::cout << "Capacity: " << config.capacity << "\n"
          << "Max message size: " << config.max_message_size << "\n";
```

#### `GetStats()`

Get producer statistics.

```cpp
struct Stats {
    uint64_t messages_sent;
    uint64_t bytes_sent;
    uint64_t failed_pushes;  // Timeouts + ChannelClosed
};

[[nodiscard]] Stats GetStats() const noexcept;
```

**Memory Order:** Relaxed atomics (approximate)

**Example:**

```cpp
auto stats = producer.GetStats();
std::cout << "Sent: " << stats.messages_sent << " messages, "
          << stats.bytes_sent << " bytes\n"
          << "Failed: " << stats.failed_pushes << "\n";
```

### 5.5 Lifecycle

#### Destructor

```cpp
~ProducerHandle() noexcept;
```

**Behavior:**
1. Sets `producer_alive` flag to `false` (release memory order)
2. Notifies consumer (wakes from blocking wait)
3. Consumer sees `ChannelClosed` on next `Pop()`

**Thread Safety:** Safe to destroy from any thread

**Example:**

```cpp
{
    auto [error, channel] = broker.RequestChannel("test");
    
    std::thread([&]() {
        // Producer scope
        auto& producer = channel->producer;
        producer.TryPush(msg);
    }).join();  // Producer destroyed here
    
    // Consumer sees ChannelClosed
    auto [result, msg] = channel->consumer.TryPop();
    assert(result == PopResult::ChannelClosed);
}
```

#### Move Semantics

```cpp
ProducerHandle(ProducerHandle&&) noexcept;
ProducerHandle& operator=(ProducerHandle&&) noexcept;
```

**Behavior:** Moved-from handle is valid but inactive

**Example:**

```cpp
ProducerHandle producer = std::move(channel->producer);

// Moved-from handle is inactive
auto result = channel->producer.TryPush(msg);
assert(result == PushResult::ChannelClosed);  // Safe, defined behavior

// Can restore
channel->producer = std::move(producer);
```

---

## 6. ConsumerHandle API

Handle for receiving messages from a channel (move-only, SPSC enforced).

### 6.1 Message Class

Zero-copy view of a message in the ring buffer.

```cpp
class Message {
public:
    // Raw data access
    [[nodiscard]] std::span<const uint8_t> Data() const noexcept;
    
    // FlatBuffers convenience accessor
    template<typename T>
    [[nodiscard]] const T* GetFlatBuffer() const noexcept;
    
    // FlatBuffers integrity check (expensive)
    template<typename T>
    [[nodiscard]] bool Verify() const noexcept;
};
```

**Lifetime:** Valid until next `Pop()` or `~ConsumerHandle()`

**Example - Raw Data:**

```cpp
auto [result, msg] = consumer.TryPop();
if (result == PopResult::Success) {
    auto data = msg->Data();
    std::cout << "Received " << data.size() << " bytes\n";
    
    // Process data
    process_bytes(data.data(), data.size());
}
```

**Example - FlatBuffers:**

```cpp
auto [result, msg] = consumer.BlockingPop();
if (result == PopResult::Success) {
    // Zero-copy access to FlatBuffer
    auto fb_msg = msg->GetFlatBuffer<MyMessage>();
    
    std::cout << "Name: " << fb_msg->name()->str() << "\n";
    std::cout << "Value: " << fb_msg->value() << "\n";
}
```

**Example - Verification (Debug):**

```cpp
#ifndef NDEBUG
    if (!msg->Verify<MyMessage>()) {
        std::cerr << "Corrupted FlatBuffer!\n";
        return;
    }
#endif
auto fb_msg = msg->GetFlatBuffer<MyMessage>();
```

### 6.2 Blocking Operations

#### `BlockingPop()`

Receive message with timeout, blocking until data available.

```cpp
[[nodiscard]] std::pair<PopResult, std::optional<Message>> BlockingPop(
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max()
) noexcept;
```

**Parameters:**
- `timeout`: Maximum wait time (default: infinite)

**Returns:** Pair of `(result, optional_message)`

**Blocks:** Until message available or timeout

**Wakes:** On producer `Push()` or timeout

**Example - Infinite Wait:**

```cpp
while (running) {
    auto [result, msg] = consumer.BlockingPop();
    
    if (result == PopResult::Success) {
        process_message(msg->Data());
    } else if (result == PopResult::ChannelClosed) {
        std::cout << "Producer disconnected\n";
        break;
    }
}
```

**Example - With Timeout:**

```cpp
auto [result, msg] = consumer.BlockingPop(std::chrono::seconds(5));

switch (result) {
    case PopResult::Success:
        process_message(msg->Data());
        break;
    
    case PopResult::Timeout:
        std::cout << "No message for 5 seconds\n";
        break;
    
    case PopResult::ChannelClosed:
        std::cout << "Producer disconnected\n";
        break;
}
```

**Example - Consumer Loop with Shutdown:**

```cpp
std::atomic<bool> shutdown{false};

while (!shutdown.load()) {
    // Short timeout allows checking shutdown flag
    auto [result, msg] = consumer.BlockingPop(std::chrono::milliseconds(100));
    
    if (result == PopResult::Success) {
        process_message(msg->Data());
    } else if (result == PopResult::ChannelClosed) {
        break;
    }
    // Timeout is normal, loop continues
}
```

### 6.3 Non-Blocking Operations

#### `TryPop()`

Attempt to receive message without blocking.

```cpp
[[nodiscard]] std::pair<PopResult, std::optional<Message>> TryPop() noexcept;
```

**Returns:**
- `(Success, message)`: Message received
- `(Empty, nullopt)`: No messages available (immediate return)
- `(ChannelClosed, nullopt)`: Producer destroyed

**Does Not Block:** Returns immediately

**Example:**

```cpp
auto [result, msg] = consumer.TryPop();

if (result == PopResult::Success) {
    process_message(msg->Data());
} else if (result == PopResult::Empty) {
    // Queue empty, do other work
    do_other_work();
}
```

**Example - Polling Loop:**

```cpp
while (running) {
    auto [result, msg] = consumer.TryPop();
    
    if (result == PopResult::Success) {
        process_message(msg->Data());
    } else if (result == PopResult::Empty) {
        // Avoid busy-waiting
        std::this_thread::yield();
    } else if (result == PopResult::ChannelClosed) {
        break;
    }
}
```

#### `BatchPop()`

Receive multiple messages efficiently.

```cpp
[[nodiscard]] std::pair<PopResult, std::vector<Message>> BatchPop(
    size_t max_count,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()
) noexcept;
```

**Parameters:**
- `max_count`: Maximum messages to receive
- `timeout`: Wait time if queue empty (default: no wait)

**Returns:** Pair of `(result, vector_of_messages)`

**Behavior:** Pops up to `max_count` messages (stops early if queue empty)

**Performance:** Amortizes overhead for high-throughput processing

**Example:**

```cpp
constexpr size_t BATCH_SIZE = 100;

while (running) {
    auto [result, messages] = consumer.BatchPop(BATCH_SIZE, std::chrono::milliseconds(10));
    
    if (!messages.empty()) {
        // Process batch
        for (const auto& msg : messages) {
            process_message(msg.Data());
        }
        std::cout << "Processed " << messages.size() << " messages\n";
    }
    
    if (result == PopResult::ChannelClosed) {
        break;
    }
}
```

**Example - Drain Queue:**

```cpp
// Drain all pending messages
auto [result, messages] = consumer.BatchPop(SIZE_MAX);

std::cout << "Drained " << messages.size() << " messages\n";
for (const auto& msg : messages) {
    process_message(msg.Data());
}
```

### 6.4 Query Methods

#### `IsConnected()`

Check if producer is alive.

```cpp
[[nodiscard]] bool IsConnected() const noexcept;
```

**Returns:** `true` if producer handle exists, `false` otherwise

**Memory Order:** Relaxed (approximate)

**Example:**

```cpp
if (!consumer.IsConnected()) {
    std::cout << "Producer disconnected, draining queue\n";
    
    // Drain remaining messages
    while (true) {
        auto [result, msg] = consumer.TryPop();
        if (result != PopResult::Success) break;
        process_message(msg->Data());
    }
}
```

#### `Capacity()`

Get ring buffer capacity.

```cpp
[[nodiscard]] size_t Capacity() const noexcept;
```

**Returns:** Number of message slots

#### `MaxMessageSize()`

Get maximum message size.

```cpp
[[nodiscard]] size_t MaxMessageSize() const noexcept;
```

**Returns:** Maximum bytes per message

#### `AvailableMessages()`

Get approximate number of pending messages.

```cpp
[[nodiscard]] size_t AvailableMessages() const noexcept;
```

**Returns:** Approximate pending message count

**Memory Order:** Relaxed (may be stale)

**Example:**

```cpp
if (consumer.AvailableMessages() > 1000) {
    std::cout << "Consumer falling behind, backlog: " 
              << consumer.AvailableMessages() << "\n";
}
```

#### `GetConfig()`

Get normalized channel configuration.

```cpp
[[nodiscard]] ChannelConfig GetConfig() const noexcept;
```

**Returns:** Configuration used for this channel

#### `GetStats()`

Get consumer statistics.

```cpp
struct Stats {
    uint64_t messages_received;
    uint64_t bytes_received;
    uint64_t failed_pops;  // Timeouts + ChannelClosed
};

[[nodiscard]] Stats GetStats() const noexcept;
```

**Memory Order:** Relaxed atomics (approximate)

**Example:**

```cpp
auto stats = consumer.GetStats();
double throughput = stats.messages_received / elapsed_seconds;
std::cout << "Throughput: " << throughput << " msg/sec\n";
```

### 6.5 Lifecycle

#### Destructor

```cpp
~ConsumerHandle() noexcept;
```

**Behavior:**
1. Sets `consumer_alive` flag to `false` (release memory order)
2. Notifies producer (wakes from blocking wait)
3. Producer sees `ChannelClosed` on next `Push()`

**Thread Safety:** Safe to destroy from any thread

#### Move Semantics

Same as `ProducerHandle` (moved-from handle is valid but inactive).

---

## 7. Error Handling Guide

### 7.1 Error Philosophy

1. **No Exceptions in Hot Path**: `Push`/`Pop` never throw
2. **Exceptions for Setup**: Broker construction, allocation failures
3. **Explicit Error Types**: Return codes over booleans

### 7.2 Common Error Patterns

#### Queue Full (Producer)

```cpp
// Option 1: Drop message
if (producer.TryPush(msg) == PushResult::QueueFull) {
    std::cerr << "Dropped message\n";
}

// Option 2: Retry with timeout
auto result = producer.TryPush(msg);
if (result == PushResult::QueueFull) {
    result = producer.BlockingPush(msg, std::chrono::milliseconds(100));
    if (result == PushResult::Timeout) {
        std::cerr << "Consumer too slow\n";
    }
}

// Option 3: Backpressure - slow down producer
while (producer.TryPush(msg) == PushResult::QueueFull) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}
```

#### Queue Empty (Consumer)

```cpp
// Option 1: Poll with yield
while (running) {
    auto [result, msg] = consumer.TryPop();
    if (result == PopResult::Empty) {
        std::this_thread::yield();
        continue;
    }
    process_message(msg->Data());
}

// Option 2: Block with timeout
auto [result, msg] = consumer.BlockingPop(std::chrono::milliseconds(100));
if (result == PopResult::Timeout) {
    // No messages for 100ms, normal
}

// Option 3: Infinite wait
while (running) {
    auto [result, msg] = consumer.BlockingPop();
    if (result == PopResult::Success) {
        process_message(msg->Data());
    } else {
        break;  // Channel closed
    }
}
```

#### Peer Disconnected

```cpp
// Producer checking consumer
if (!producer.IsConnected()) {
    std::cout << "Consumer died, stopping producer\n";
    return;
}

auto result = producer.TryPush(msg);
if (result == PushResult::ChannelClosed) {
    std::cout << "Consumer disconnected mid-send\n";
}

// Consumer checking producer
if (!consumer.IsConnected()) {
    // Drain remaining messages
    while (consumer.TryPop().first == PopResult::Success) {
        // Process messages
    }
    return;
}
```

#### Channel Creation Errors

```cpp
auto [error, channel] = broker.RequestChannel("my-channel", config);

if (error != ChannelError::Success) {
    switch (error) {
        case ChannelError::NameExists:
            // Recover: Use unique name or retrieve existing
            channel = broker.RequestChannel(
                "my-channel-" + std::to_string(instance_id)
            ).second;
            break;
        
        case ChannelError::InvalidConfig:
            // Fix config
            config = config.Normalize();
            channel = broker.RequestChannel("my-channel", config).second;
            break;
        
        case ChannelError::AllocationFailed:
            // Fatal error
            throw std::bad_alloc();
    }
}
```

### 7.3 Graceful Shutdown

```cpp
std::atomic<bool> shutdown_requested{false};

// Producer thread
void producer_thread() {
    while (!shutdown_requested.load()) {
        auto msg = generate_message();
        
        auto result = producer.TryPush(msg);
        if (result == PushResult::ChannelClosed) {
            break;  // Consumer died
        }
    }
    // Destructor signals consumer
}

// Consumer thread
void consumer_thread() {
    while (!shutdown_requested.load()) {
        auto [result, msg] = consumer.BlockingPop(std::chrono::milliseconds(100));
        
        if (result == PopResult::Success) {
            process_message(msg->Data());
        } else if (result == PopResult::ChannelClosed) {
            // Drain remaining messages
            while (consumer.TryPop().first == PopResult::Success) {}
            break;
        }
    }
}

// Main shutdown
void shutdown() {
    shutdown_requested.store(true);
    
    // Wait for threads
    producer_thread_handle.join();
    consumer_thread_handle.join();
    
    // Broker shutdown (optional, automatic)
    broker.Shutdown();
}
```

---

## 8. Thread Safety Guarantees

### 8.1 Concurrency Model

- **SPSC Only**: Each channel has exactly one producer and one consumer
- **Lock-Free**: No mutexes in `Push`/`Pop` operations
- **Wait-Free Reads**: Query methods (`IsConnected`, `Capacity`, etc.)

### 8.2 Memory Ordering

#### Producer Guarantees

```cpp
// Producer writes are visible to consumer after release-store
producer.TryPush(msg1);  // write_index.store(..., release)
producer.TryPush(msg2);

// Consumer sees messages in order
auto [r1, m1] = consumer.TryPop();  // read_index.load(..., acquire)
auto [r2, m2] = consumer.TryPop();
// Guaranteed: m1 before m2
```

#### Consumer Guarantees

```cpp
// Consumer reads signal producer about free space
consumer.TryPop();  // read_index.store(..., release)

// Producer sees updated space
producer.TryPush(msg);  // write_index.load(..., acquire)
```

#### Destruction Guarantees

```cpp
// ProducerHandle destructor uses seq_cst fence before death signal
producer.~ProducerHandle();
    // std::atomic_thread_fence(memory_order_seq_cst)
    // producer_alive.store(false, memory_order_release)
    // write_index.notify_one()

// Consumer sees all prior writes
auto [result, msg] = consumer.TryPop();
// If result == ChannelClosed, all prior Push() data is visible
```

### 8.3 Broker Thread Safety

| Method | Lock Type | Concurrent Access |
|--------|-----------|-------------------|
| `RequestChannel()` | Write lock | Exclusive |
| `RemoveChannel()` | Write lock | Exclusive |
| `HasChannel()` | Read lock | Multiple readers allowed |
| `GetStats()` | None | Relaxed atomics |

**Example - Concurrent Channel Creation:**

```cpp
// Safe: Multiple threads creating channels
std::vector<std::thread> threads;
for (int i = 0; i < 10; ++i) {
    threads.emplace_back([i]() {
        auto name = "channel-" + std::to_string(i);
        auto [error, channel] = broker.RequestChannel(name);
        // Use channel...
    });
}
```

### 8.4 What Is NOT Thread-Safe

#### ❌ Multiple Producers/Consumers

```cpp
// ❌ INCORRECT: Two threads sending to same handle
std::thread t1([&]() { producer.TryPush(msg1); });
std::thread t2([&]() { producer.TryPush(msg2); });  // DATA RACE!

// ✅ CORRECT: Create separate channels
auto [e1, ch1] = broker.RequestChannel("ch1");
auto [e2, ch2] = broker.RequestChannel("ch2");

std::thread t1([&]() { ch1->producer.TryPush(msg1); });
std::thread t2([&]() { ch2->producer.TryPush(msg2); });
```

#### ❌ Moving Handles Concurrently

```cpp
// ❌ INCORRECT: Moving while sending
ProducerHandle producer = ...;

std::thread t1([&]() { producer.TryPush(msg); });
auto p2 = std::move(producer);  // DATA RACE with TryPush!

// ✅ CORRECT: Synchronize moves
{
    std::lock_guard lock(mutex);
    auto p2 = std::move(producer);
}
```

---

## 9. Performance Tips

### 9.1 Batch Operations

**Use `BatchPush`/`BatchPop` for high throughput:**

```cpp
// ❌ SLOW: Individual pushes (65ns overhead × 1000 = 65µs)
for (auto& msg : messages) {
    producer.TryPush(msg);
}

// ✅ FAST: Batch push (65ns overhead × 1 = 65ns)
producer.BatchPush(message_spans);
// 10-100x improvement!
```

**When to use batching:**
- Throughput > 1000 msg/sec
- Message size < 1 KB
- Can tolerate slight latency increase (buffering)

### 9.2 Message Size Optimization

**Small messages (< 256 bytes):**
```cpp
// Optimize for throughput
auto [error, channel] = broker.RequestChannel("small-msgs", {
    .capacity = 32768,      // Many slots
    .max_message_size = 256 // Small messages
});
```

**Large messages (> 1 MB):**
```cpp
// Optimize for bandwidth
auto [error, channel] = broker.RequestChannel("large-msgs", {
    .capacity = 64,         // Fewer slots
    .max_message_size = 1048576  // 1 MB
});
```

### 9.3 Zero-Copy with FlatBuffers

```cpp
// ❌ SLOW: Double copy (serialize → vector → queue)
flatbuffers::FlatBufferBuilder fbb;
// ... build message ...
std::vector<uint8_t> buffer(fbb.GetBufferPointer(), 
                             fbb.GetBufferPointer() + fbb.GetSize());
producer.TryPush(buffer);  // Copy #2

// ✅ FAST: Single copy (serialize directly into queue)
auto reserve = producer.Reserve(256);
flatbuffers::FlatBufferBuilder fbb(reserve->capacity, reserve->data, false);
// ... build message ...
producer.Commit(fbb.GetSize());  // No extra copy!
```

### 9.4 Avoid Allocation in Hot Path

```cpp
// ❌ SLOW: Allocations in loop
while (running) {
    std::vector<uint8_t> msg = create_message();  // Allocate!
    producer.TryPush(msg);
}

// ✅ FAST: Reuse buffer
std::vector<uint8_t> msg;
msg.reserve(max_size);

while (running) {
    msg.clear();
    fill_message(msg);
    producer.TryPush(msg);  // No allocation!
}
```

### 9.5 Polling vs Blocking

**Use blocking for low-latency:**
```cpp
// Low CPU usage, <200ns wakeup latency
while (running) {
    auto [result, msg] = consumer.BlockingPop();
    process(msg);
}
```

**Use polling for ultra-low latency:**
```cpp
// High CPU usage, ~20ns response time
while (running) {
    auto [result, msg] = consumer.TryPop();
    if (result == PopResult::Success) {
        process(msg);
    }
    // No yield - busy wait
}
```

**Hybrid approach:**
```cpp
// Adaptive: Busy-poll during high activity, block during idle
constexpr auto IDLE_THRESHOLD = std::chrono::milliseconds(10);
auto last_msg_time = std::chrono::steady_clock::now();

while (running) {
    auto [result, msg] = consumer.TryPop();
    
    if (result == PopResult::Success) {
        process(msg);
        last_msg_time = std::chrono::steady_clock::now();
    } else {
        auto idle_time = std::chrono::steady_clock::now() - last_msg_time;
        if (idle_time > IDLE_THRESHOLD) {
            // Switch to blocking during idle
            auto [r, m] = consumer.BlockingPop(std::chrono::seconds(1));
            if (r == PopResult::Success) process(m);
        } else {
            std::this_thread::yield();
        }
    }
}
```

### 9.6 Capacity Planning

**Formula:**
```
Capacity = (peak_msg_rate × max_latency_seconds) × safety_factor
```

**Example:**
```cpp
// Peak rate: 1M msg/sec
// Max tolerable latency: 10ms
// Safety factor: 2x
size_t capacity = (1'000'000 * 0.010) * 2;  // 20,000 slots

auto [error, channel] = broker.RequestChannel("high-throughput", {
    .capacity = 32768,  // Round up to power of 2
    .max_message_size = 256
});
```

### 9.7 Cache-Line Awareness

**Already optimized internally:**
- `write_index` and `read_index` on separate cache lines
- Liveness flags on separate cache lines
- Ring buffer naturally aligned

**Your responsibility:**
```cpp
// ❌ BAD: False sharing between producer and consumer data
struct SharedData {
    std::atomic<bool> producer_flag;
    std::atomic<bool> consumer_flag;  // Same cache line!
};

// ✅ GOOD: Separate cache lines
struct alignas(64) ProducerData {
    std::atomic<bool> flag;
};

struct alignas(64) ConsumerData {
    std::atomic<bool> flag;
};
```

### 9.8 Profiling Checklist

**If performance issues:**

1. **Check alignment:**
   ```bash
   # Linux
   perf stat -e cache-misses,cache-references ./app
   
   # Expected: <5% cache miss rate
   ```

2. **Check atomic overhead:**
   ```bash
   perf record -g ./app
   perf report
   
   # Atomic operations should be <10% of total time
   ```

3. **Check message size:**
   ```cpp
   auto stats = producer.GetStats();
   double avg_size = stats.bytes_sent / stats.messages_sent;
   // Small messages (<256B) benefit most from batching
   ```

4. **Check queue utilization:**
   ```cpp
   double util = 1.0 - (producer.AvailableSlots() / producer.Capacity());
   // High utilization (>90%) → increase capacity
   // Low utilization (<10%) → consumer falling behind
   ```

---

## Appendix: Complete Example

### High-Throughput Telemetry System

```cpp
#include <omni/mailbox.hpp>
#include <flatbuffers/flatbuffers.h>
#include "telemetry_generated.h"  // FlatBuffers schema
#include <thread>
#include <atomic>
#include <chrono>

class TelemetrySystem {
public:
    TelemetrySystem() {
        auto& broker = omni::MailboxBroker::Instance();
        
        // High-throughput config
        auto [error, channel] = broker.RequestChannel("telemetry", {
            .capacity = 32768,
            .max_message_size = 512
        });
        
        if (error != omni::ChannelError::Success) {
            throw std::runtime_error("Failed to create channel");
        }
        
        producer_ = std::move(channel->producer);
        consumer_ = std::move(channel->consumer);
    }
    
    void Start() {
        running_ = true;
        
        producer_thread_ = std::thread([this]() { ProducerLoop(); });
        consumer_thread_ = std::thread([this]() { ConsumerLoop(); });
    }
    
    void Stop() {
        running_ = false;
        
        if (producer_thread_.joinable()) producer_thread_.join();
        if (consumer_thread_.joinable()) consumer_thread_.join();
        
        PrintStats();
    }

private:
    void ProducerLoop() {
        std::vector<std::span<const uint8_t>> batch;
        batch.reserve(100);
        
        while (running_) {
            // Generate telemetry data
            for (int i = 0; i < 100 && running_; ++i) {
                auto reserve = producer_.Reserve(512);
                if (!reserve) {
                    // Queue full, flush batch
                    break;
                }
                
                // Build FlatBuffer directly into queue
                flatbuffers::FlatBufferBuilder fbb(
                    reserve->capacity, reserve->data, false
                );
                
                auto msg = telemetry::CreateSample(
                    fbb,
                    std::chrono::system_clock::now().time_since_epoch().count(),
                    sensor_id_++,
                    generate_value()
                );
                fbb.Finish(msg);
                
                producer_.Commit(fbb.GetSize());
            }
            
            // Rate limiting
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }
    
    void ConsumerLoop() {
        constexpr size_t BATCH_SIZE = 100;
        
        while (running_) {
            // Batch pop for efficiency
            auto [result, messages] = consumer_.BatchPop(
                BATCH_SIZE,
                std::chrono::milliseconds(10)
            );
            
            if (!messages.empty()) {
                // Process batch
                for (const auto& msg : messages) {
                    auto sample = msg.GetFlatBuffer<telemetry::Sample>();
                    process_sample(sample);
                }
            }
            
            if (result == omni::PopResult::ChannelClosed) {
                // Drain remaining
                while (consumer_.TryPop().first == omni::PopResult::Success) {}
                break;
            }
        }
    }
    
    void PrintStats() {
        auto p_stats = producer_.GetStats();
        auto c_stats = consumer_.GetStats();
        
        std::cout << "Producer Stats:\n"
                  << "  Sent: " << p_stats.messages_sent << " messages\n"
                  << "  Bytes: " << p_stats.bytes_sent << " bytes\n"
                  << "  Failed: " << p_stats.failed_pushes << "\n\n";
        
        std::cout << "Consumer Stats:\n"
                  << "  Received: " << c_stats.messages_received << " messages\n"
                  << "  Bytes: " << c_stats.bytes_received << " bytes\n"
                  << "  Failed: " << c_stats.failed_pops << "\n";
    }
    
    double generate_value() { return 42.0; }
    void process_sample(const telemetry::Sample* s) { /* ... */ }
    
    omni::ProducerHandle producer_;
    omni::ConsumerHandle consumer_;
    std::thread producer_thread_;
    std::thread consumer_thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> sensor_id_{0};
};

int main() {
    TelemetrySystem system;
    system.Start();
    
    // Run for 10 seconds
    std::this_thread::sleep_for(std::chrono::seconds(10));
    
    system.Stop();
    return 0;
}
```

---

**End of API Reference**
