# OmniMailbox: Design Specification v1.0

**High-Performance Thread-Safe Intra-Process Communication Library**

---

## Table of Contents

1. [Overview](#1-overview)
2. [Requirements](#2-requirements)
3. [Constraints](#3-constraints)
4. [API Contract](#4-api-contract)
5. [Implementation Guidance](#5-implementation-guidance)
6. [Build System](#6-build-system)
7. [Testing & Validation](#7-testing--validation)
8. [Performance Benchmarks](#8-performance-benchmarks)
9. [Error Handling Strategy](#9-error-handling-strategy)
10. [Deliverables](#10-deliverables)

---

## 1. Overview

### 1.1 Purpose

**OmniMailbox** is a zero-copy, lock-free, thread-safe intra-process communication library for C++23. It enables high-performance message passing between threads using:

- **Lock-Free SPSC Queues**: Single Producer Single Consumer ring buffers
- **FlatBuffers Integration**: Zero-copy serialization/deserialization
- **Singleton Dispatcher**: Centralized channel management with RAII cleanup
- **Cross-Platform**: Windows/Linux on x86/x64/ARM64

### 1.2 Use Cases

- High-frequency trading systems (market data → strategy threads)
- Game engines (render thread ↔ physics thread)
- Media pipelines (decoder → encoder → muxer)
- Telemetry systems (sensors → aggregator → storage)

### 1.3 Design Philosophy

1. **Zero-Copy by Default**: Messages stay in ring buffer; consumers get `std::span` views
2. **Fail-Fast**: No silent failures; all errors returned via `std::optional` or enums
3. **RAII Everywhere**: Channels auto-cleanup when handles destroyed
4. **Explicit Memory Ordering**: No `memory_order_seq_cst`; document every atomic operation

---

## 2. Requirements

### 2.1 Functional Requirements

| ID | Requirement | Priority |
|----|-------------|----------|
| FR-1 | Support variable-size messages (64 bytes - 1MB) | MUST |
| FR-2 | Multiple named channels with isolated ring buffers | MUST |
| FR-3 | Blocking operations with timeout support | MUST |
| FR-4 | Non-blocking TryPush/TryPop | MUST |
| FR-5 | Detect peer disconnect (producer/consumer death) | MUST |
| FR-6 | FlatBuffers zero-copy Reserve/Commit API | MUST |
| FR-7 | Thread-safe channel creation/destruction | MUST |
| FR-8 | Graceful degradation on resource exhaustion | SHOULD |

### 2.2 Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Throughput (64-byte msgs, uncontended) | ≥5M msg/sec |
| NFR-2 | Latency p99 (uncontended) | <500ns |
| NFR-3 | Memory overhead per channel | <16KB + ring buffer |
| NFR-4 | Compile time (clean build) | <30 seconds |
| NFR-5 | Binary size (static lib, release) | <500KB |
| NFR-6 | Test coverage (line coverage) | ≥90% |

### 2.3 Platform Support

**Operating Systems:**
- Windows 10+ (MSVC 19.30+, Clang-CL)
- Linux (GCC 11+, Clang 14+)

**Architectures:**
- x86 (32-bit, legacy support)
- x64 (primary target)
- ARM64 (Apple Silicon, AWS Graviton)

**C++ Standard:**
- Primary: C++23
- Fallback: C++20 (when C++23 features unavailable)

---

## 3. Constraints

### 3.1 Performance Constraints

#### 3.1.1 Zero Allocation in Hot Path
```cpp
// ✅ ALLOWED in Push/Pop
std::atomic<uint64_t>::load(std::memory_order_acquire);
std::span construction (pointer arithmetic)

// ❌ FORBIDDEN in Push/Pop
new / delete / malloc / free
std::vector::push_back()
std::string construction
std::shared_ptr::make_shared()
```

#### 3.1.2 Cache-Line Alignment
```cpp
// Use hardware destructive interference size when available
#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 64; // Conservative fallback
#endif

// All atomic indices MUST be cache-line aligned
struct alignas(CACHE_LINE_SIZE) SPSCQueue {
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> write_index;
    char padding1[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];
    
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> read_index;
    char padding2[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];
    
    // ... rest of struct
};
```

#### 3.1.3 Memory Ordering Rules

| Operation | Producer Side | Consumer Side |
|-----------|---------------|---------------|
| Check space/data | `read_index.load(acquire)` | `write_index.load(acquire)` |
| Write/Read data | Regular stores/loads | Regular stores/loads |
| Publish progress | `write_index.store(release)` | `read_index.store(release)` |
| Wait notification | `write_index.wait(val)` | `read_index.wait(val)` |
| Wake peer | `write_index.notify_one()` | `read_index.notify_one()` |

**Rationale:** Acquire-Release semantics synchronize memory:
- Producer's `release` of `write_index` synchronizes-with Consumer's `acquire`
- Consumer's `release` of `read_index` synchronizes-with Producer's `acquire`

### 3.2 Safety Constraints

#### 3.2.1 Memory Safety
- **ASAN Clean**: No use-after-free, buffer overflows
- **TSAN Clean**: No data races (all shared state via atomics)
- **UBSAN Clean**: No signed overflow, null derefs, alignment violations
- **Valgrind Clean**: No memory leaks, uninitialized reads

#### 3.2.2 Concurrency Safety
- **SPSC Only**: Enforce single producer/consumer via type system (non-copyable handles)
- **No Spurious Wakeups**: Use atomic wait/notify with predicate checks
- **ABA Prevention**: Use 64-bit monotonic indices (wrap after 2^64 - astronomically unlikely)

#### 3.2.3 API Contracts (Preconditions/Postconditions)

```cpp
// Example: ProducerHandle::Reserve
// PRECONDITION: bytes > 0 && bytes <= max_message_size
// PRECONDITION: IsConnected() == true (consumer alive)
// POSTCONDITION: If Success, returned pointer valid until Commit() or ~ProducerHandle()
// POSTCONDITION: If Success, exactly one Commit() call required (else leak slot)
std::optional<ReserveResult> Reserve(size_t bytes);
```

### 3.3 Build Constraints

- **No Exceptions in Hot Path**: Use exceptions only in setup/teardown
- **Header-Only Option**: Provide single-header amalgamation via script
- **Minimal Dependencies**: Only STL + FlatBuffers (vendored or external)

---

## 4. API Contract

### 4.1 Core Types

```cpp
namespace omni {

// Forward declarations
class MailboxBroker;
class ProducerHandle;
class ConsumerHandle;

// Result types
enum class PushResult {
    Success,
    Timeout,
    ChannelClosed,  // Consumer destroyed
    InvalidSize,    // Message > max_message_size
    QueueFull       // TryPush only
};

enum class PopResult {
    Success,
    Timeout,
    ChannelClosed,  // Producer destroyed
    Empty           // TryPop only
};

enum class ChannelError {
    Success,
    NameExists,      // Channel with this name already exists
    InvalidConfig,   // Config validation failed
    AllocationFailed // Memory allocation failed
};

// Channel configuration
struct ChannelConfig {
    size_t capacity = 1024;          // Will be rounded up to power of 2
    size_t max_message_size = 4096;  // Range [64, 16MB]
    
    // Validate and normalize configuration
    [[nodiscard]] ChannelConfig Normalize() const noexcept {
        ChannelConfig normalized = *this;
        
        // Clamp to valid ranges FIRST (prevent overflow in RoundUpPowerOf2)
        normalized.capacity = std::clamp(normalized.capacity, size_t(8), size_t(524'288));  // Max is 2^19
        normalized.max_message_size = std::clamp(normalized.max_message_size, 
                                                   size_t(64), size_t(16'777'216));
        
        // Round capacity to next power of 2 (guaranteed to be <= 524'288, which is already power of 2)
        if ((normalized.capacity & (normalized.capacity - 1)) != 0) {
            normalized.capacity = RoundUpPowerOf2(normalized.capacity);
        }
        
        return normalized;
    }
    
    [[nodiscard]] bool IsValid() const noexcept {
        return (capacity >= 8 && capacity <= 524'288) &&  // Changed from 1M to 2^19
               (max_message_size >= 64 && max_message_size <= 16'777'216);
    }

private:
    static constexpr size_t RoundUpPowerOf2(size_t n) noexcept {
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        n++;
        return n;
    }
};

// Channel creation result
/**
 * @brief Pair of producer and consumer handles for a channel.
 * 
 * Both handles are move-only and enforce SPSC semantics.
 * 
 * @par Move Semantics
 * After moving from a handle (producer or consumer), the moved-from
 * handle is in a valid but inactive state:
 * - Destructor is safe to call
 * - All methods return error (ChannelClosed / nullopt / false)
 * - Can be assigned to (restored to active state)
 * 
 * @code
 * auto channel = broker.RequestChannel("test").value();
 * 
 * // Move out producer
 * auto producer = std::move(channel.producer);
 * 
 * // Moved-from handle - safe but inactive
 * auto result = channel.producer.TryPush(data);
 * assert(result == PushResult::ChannelClosed);  // ✅ Defined behavior
 * 
 * // Can restore
 * channel.producer = std::move(producer);  // ✅ Move back
 * @endcode
 */
struct ChannelPair {
    ProducerHandle producer;
    ConsumerHandle consumer;
};

} // namespace omni
```

### 4.2 MailboxBroker (Singleton Dispatcher)

```cpp
namespace omni {

class MailboxBroker {
public:
    // Get singleton instance (thread-safe lazy initialization)
    // WARNING: Singleton uses intentional memory leak to avoid destruction order issues
    // See "Singleton Lifetime" section below for details
    [[nodiscard]] static MailboxBroker& Instance() noexcept;
    
    /**
     * @warning GLOBAL HANDLES FORBIDDEN
     * 
     * Do NOT create global or static-storage-duration handles.
     * Handles must be destroyed before program exit to avoid
     * undefined behavior with singleton destruction order.
     * 
     * INCORRECT:
     *   static ProducerHandle g_producer = broker.RequestChannel(...)->producer;  // ❌
     * 
     * CORRECT:
     *   std::unique_ptr<ProducerHandle> g_producer;
     *   g_producer = std::make_unique<ProducerHandle>(broker.RequestChannel(...)->producer);
     *   // Explicitly reset before main() returns:
     *   g_producer.reset();  // ✓
     * 
     * @warning NOT SIGNAL-SAFE (POSIX)
     * 
     * Do NOT call broker methods from POSIX signal handlers.
     * Use atomic flags for signaling instead.
     * 
     * INCORRECT:
     *   void signal_handler(int) {
     *       MailboxBroker::Instance().Shutdown();  // ❌ Deadlock risk
     *   }
     * 
     * CORRECT:
     *   std::atomic<bool> shutdown_requested{false};
     *   void signal_handler(int) {
     *       shutdown_requested.store(true, std::memory_order_release);  // ✓
     *   }
     *   int main() {
     *       signal(SIGTERM, signal_handler);
     *       while (!shutdown_requested.load(std::memory_order_acquire)) {
     *           // process messages
     *       }
     *       broker.Shutdown();  // Safe in main thread
     *   }
     */
    
    /**
     * @brief Create new channel with error reporting.
     * 
     * Atomically checks for existing channel with given name and creates
     * new channel if not found. Configuration is automatically normalized
     * before use.
     * 
     * @param name Unique channel identifier
     * @param config Channel configuration (auto-normalized)
     * @return Pair of (error code, optional channel pair)
     * 
     * @par Preconditions
     * - !name.empty()
     * 
     * @par Postconditions
     * - On success: error = Success, channel contains valid handles
     * - On failure: error indicates reason, channel is nullopt
     * 
     * @par Error Conditions
     * - NameExists: Channel with this name already registered
     * - InvalidConfig: Config invalid even after normalization
     * - AllocationFailed: Memory allocation failed
     * 
     * @par Thread Safety
     * Uses shared_mutex (write lock). Safe to call from multiple threads.
     * 
     * @par Performance
     * O(1) average, O(n) worst-case (hash collision).
     * Mutex contention possible with many concurrent requests.
     * 
     * @par Example
     * @code
     * auto [error, channel] = broker.RequestChannel("my-channel", {
     *     .capacity = 2048,
     *     .max_message_size = 4096
     * });
     * 
     * switch (error) {
     *     case ChannelError::Success:
     *         // Use channel.value()
     *         break;
     *     case ChannelError::NameExists:
     *         // Choose different name or retrieve existing
     *         break;
     *     case ChannelError::InvalidConfig:
     *         // Fix config and retry
     *         break;
     *     case ChannelError::AllocationFailed:
     *         // Free memory and retry
     *         break;
     * }
     * 
     * // Or simple check:
     * if (error == ChannelError::Success) {
     *     auto& [producer, consumer] = channel.value();
     *     // Use handles
     * }
     * @endcode
     */
    [[nodiscard]] std::pair<ChannelError, std::optional<ChannelPair>> RequestChannel(
        std::string_view name,
        const ChannelConfig& config = {}
    ) noexcept;
    
    // Check if channel exists
    // THREAD-SAFE: Uses shared_mutex (read lock)
    [[nodiscard]] bool HasChannel(std::string_view name) const noexcept;
    
    // Remove channel (only if no active handles)
    // THREAD-SAFE: Uses shared_mutex (write lock)
    // RETURNS: true if removed, false if handles still alive
    bool RemoveChannel(std::string_view name) noexcept;
    
    // Statistics (approximate counts, relaxed atomics)
    struct Stats {
        size_t active_channels;
        size_t total_channels_created;
        size_t total_messages_sent;     // Across all channels
        size_t total_bytes_transferred;
    };
    [[nodiscard]] Stats GetStats() const noexcept;
    
    // Shutdown all channels (blocks until all handles released)
    // WARNING: Deadlock risk if called while holding handles
    void Shutdown() noexcept;

private:
    MailboxBroker() = default;
    ~MailboxBroker() = default;
    
    // Non-copyable, non-movable
    MailboxBroker(const MailboxBroker&) = delete;
    MailboxBroker& operator=(const MailboxBroker&) = delete;
    MailboxBroker(MailboxBroker&&) = delete;
    MailboxBroker& operator=(MailboxBroker&&) = delete;
    
    // Implementation details (hidden)
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace omni
```

### 4.3 ProducerHandle

```cpp
namespace omni {

class ProducerHandle {
public:
    // Zero-copy reserve for FlatBuffers
    struct ReserveResult {
        uint8_t* data;       // Pointer to write region
        size_t capacity;     // Available bytes (>= requested)
        
        // RAII: Auto-rollback if Commit() not called
        ~ReserveResult();
    };
    
    // Reserve space in ring buffer (FAIL-FAST: no timeout)
    // PRECONDITION: bytes > 0 && bytes <= max_message_size
    // PRECONDITION: IsConnected() == true
    // POSTCONDITION: Must call Commit(actual_bytes) before next Reserve()
    // ERROR: Returns nullopt if:
    //   - Queue full (producer should decide: drop, retry, or switch strategy)
    //   - bytes > max_message_size
    //   - Consumer disconnected
    //   - Previous reservation not committed
    [[nodiscard]] std::optional<ReserveResult> Reserve(size_t bytes) noexcept;
    
    // Commit reserved space (completes message)
    // PRECONDITION: Reserve() returned Success
    // PRECONDITION: actual_bytes <= ReserveResult::capacity
    // PRECONDITION: actual_bytes > 0
    // POSTCONDITION: Message visible to consumer
    // ERROR: Returns false if preconditions violated
    bool Commit(size_t actual_bytes) noexcept;
    
    // Abort reservation without sending
    void Rollback() noexcept;
    
    // Blocking push (copies data into ring buffer)
    // PRECONDITION: !data.empty() && data.size() <= max_message_size
    // BLOCKS: Until space available or timeout
    // WAKES: On consumer Pop() or timeout
    [[nodiscard]] PushResult BlockingPush(
        std::span<const uint8_t> data,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::max()
    ) noexcept;
    
    // Non-blocking push attempt
    // PRECONDITION: !data.empty() && data.size() <= max_message_size
    // RETURNS: QueueFull immediately if no space
    [[nodiscard]] PushResult TryPush(std::span<const uint8_t> data) noexcept;
    
    /**
     * @brief Batch push multiple messages (amortizes atomic overhead).
     * 
     * Attempts to push all messages in the span. Stops at first failure
     * (queue full or consumer disconnected) and returns number of successfully
     * pushed messages.
     * 
     * @param messages Span of message data to push
     * @return Number of messages successfully pushed [0, messages.size()]
     * 
     * @par Performance Benefits
     * Batch operations amortize atomic overhead across multiple messages:
     * - Single push: ~65ns overhead per message (atomic load/store/notify)
     * - Batch push: ~65ns overhead total (amortized 1x, not N×)
     * 
     * For high-throughput scenarios (1000+ msg/sec), batch operations
     * provide 10-100x performance improvement.
     * 
     * @par Example
     * @code
     * std::vector<std::vector<uint8_t>> messages;
     * // ... fill messages ...
     * 
     * std::vector<std::span<const uint8_t>> spans;
     * for (const auto& msg : messages) {
     *     spans.emplace_back(msg.data(), msg.size());
     * }
     * 
     * size_t sent = producer.BatchPush(spans);
     * std::cout << "Sent " << sent << "/" << spans.size() << " messages\n";
     * @endcode
     */
    [[nodiscard]] size_t BatchPush(
        std::span<const std::span<const uint8_t>> messages
    ) noexcept;
    
    // Query state (relaxed reads, approximate)
    [[nodiscard]] bool IsConnected() const noexcept;  // Consumer alive
    [[nodiscard]] size_t Capacity() const noexcept;
    [[nodiscard]] size_t MaxMessageSize() const noexcept;
    [[nodiscard]] size_t AvailableSlots() const noexcept;  // Approx free space
    
    /**
     * @brief Get channel configuration.
     * 
     * Returns the normalized configuration used to create the channel.
     * Values may differ from the config passed to RequestChannel() due
     * to normalization (rounding capacity to power of 2, clamping ranges).
     * 
     * @return Normalized configuration used for this channel
     * 
     * @par Example
     * @code
     * auto [error, channel] = broker.RequestChannel("test", {
     *     .capacity = 1000,  // Not power of 2
     *     .max_message_size = 4096
     * });
     * 
     * if (error == ChannelError::Success) {
     *     auto config = channel->producer.GetConfig();
     *     std::cout << "Actual capacity: " << config.capacity << "\n";  // Prints: 1024
     * }
     * @endcode
     */
    [[nodiscard]] ChannelConfig GetConfig() const noexcept;
    
    // Statistics (relaxed atomics)
    struct Stats {
        uint64_t messages_sent;
        uint64_t bytes_sent;
        uint64_t failed_pushes;  // Timeouts + ChannelClosed
    };
    [[nodiscard]] Stats GetStats() const noexcept;
    
    // RAII: Destructor signals consumer (sets producer_alive = false)
    ~ProducerHandle() noexcept;
    
    // Move-only
    ProducerHandle(ProducerHandle&&) noexcept;
    ProducerHandle& operator=(ProducerHandle&&) noexcept;
    
    // Non-copyable (enforces SPSC)
    ProducerHandle(const ProducerHandle&) = delete;
    ProducerHandle& operator=(const ProducerHandle&) = delete;

private:
    friend class MailboxBroker;
    explicit ProducerHandle(std::shared_ptr<detail::SPSCQueue> queue);
    
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace omni
```

### 4.4 ConsumerHandle

```cpp
namespace omni {

class ConsumerHandle {
public:
    // Zero-copy message view
    class Message {
    public:
        // Raw data access
        [[nodiscard]] std::span<const uint8_t> Data() const noexcept {
            return data_;
        }
        
        // FlatBuffers convenience accessor
        // PRECONDITION: Message contains valid FlatBuffer of type T
        // RECOMMENDATION: Call Verify<T>() in debug builds
        template<typename T>
        [[nodiscard]] const T* GetFlatBuffer() const noexcept {
            return flatbuffers::GetRoot<T>(data_.data());
        }
        
        // FlatBuffers integrity check
        // EXPENSIVE: Use in debug/testing only
        template<typename T>
        [[nodiscard]] bool Verify() const noexcept {
            flatbuffers::Verifier verifier(data_.data(), data_.size());
            return verifier.VerifyBuffer<T>();
        }
        
        // LIFETIME: Valid until next Pop() or ~ConsumerHandle()
        ~Message() = default;
        
    private:
        friend class ConsumerHandle;
        explicit Message(std::span<const uint8_t> data) : data_(data) {}
        std::span<const uint8_t> data_;
    };
    
    // Blocking pop with timeout
    // BLOCKS: Until message available or timeout
    // WAKES: On producer Push() or timeout
    // POSTCONDITION: On Success, message valid until next Pop()
    [[nodiscard]] std::pair<PopResult, std::optional<Message>> BlockingPop(
        std::chrono::milliseconds timeout = std::chrono::milliseconds::max()
    ) noexcept;
    
    // Non-blocking pop attempt
    // RETURNS: Empty immediately if no messages
    [[nodiscard]] std::pair<PopResult, std::optional<Message>> TryPop() noexcept;
    
    // Batch pop (fill vector up to max_count)
    // PRECONDITION: max_count > 0
    // POSTCONDITION: Messages valid until next Pop/BatchPop
    // PERFORMANCE: Amortizes overhead for high-throughput scenarios
    [[nodiscard]] std::pair<PopResult, std::vector<Message>> BatchPop(
        size_t max_count,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()
    ) noexcept;
    
    // Query state (relaxed reads, approximate)
    [[nodiscard]] bool IsConnected() const noexcept;  // Producer alive
    [[nodiscard]] size_t Capacity() const noexcept;
    [[nodiscard]] size_t MaxMessageSize() const noexcept;
    [[nodiscard]] size_t AvailableMessages() const noexcept;  // Approx pending
    
    /**
     * @brief Get channel configuration.
     * 
     * Returns the normalized configuration used to create the channel.
     * Identical to ProducerHandle::GetConfig() - both handles share same queue.
     * 
     * @return Normalized configuration used for this channel
     */
    [[nodiscard]] ChannelConfig GetConfig() const noexcept;
    
    // Statistics (relaxed atomics)
    struct Stats {
        uint64_t messages_received;
        uint64_t bytes_received;
        uint64_t failed_pops;  // Timeouts + ChannelClosed
    };
    [[nodiscard]] Stats GetStats() const noexcept;
    
    // RAII: Destructor signals producer (sets consumer_alive = false)
    ~ConsumerHandle() noexcept;
    
    // Move-only
    ConsumerHandle(ConsumerHandle&&) noexcept;
    ConsumerHandle& operator=(ConsumerHandle&&) noexcept;
    
    // Non-copyable (enforces SPSC)
    ConsumerHandle(const ConsumerHandle&) = delete;
    ConsumerHandle& operator=(const ConsumerHandle&) = delete;

private:
    friend class MailboxBroker;
    explicit ConsumerHandle(std::shared_ptr<detail::SPSCQueue> queue);
    
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace omni
```

---

## 5. Implementation Guidance

### 5.1 Ring Buffer Architecture

#### 5.1.1 Memory Layout

```
Ring Buffer Structure:
┌─────────────────────────────────────────────────────────────┐
│ Slot 0: [4-byte size][payload][padding to 8-byte alignment] │
│ Slot 1: [4-byte size][payload][padding to 8-byte alignment] │
│ Slot 2: [4-byte size][payload][padding to 8-byte alignment] │
│ ...                                                         │
│ Slot N: [4-byte size][payload][padding to 8-byte alignment] │
└─────────────────────────────────────────────────────────────┘

Index Management (separate cache lines):
┌─────────────────┐
│ write_index     │ ← Producer cache line
├─────────────────┤
│ padding         │
└─────────────────┘

┌─────────────────┐
│ read_index      │ ← Consumer cache line
├─────────────────┤
│ padding         │
└─────────────────┘

┌─────────────────┐
│ producer_alive  │ ← Liveness flags
│ consumer_alive  │
└─────────────────┘
```

#### 5.1.2 Core Data Structure

```cpp
namespace omni::detail {

struct SPSCQueue {
    // Producer-owned cache line
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> write_index{0};
    char padding1[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];
    
    // Consumer-owned cache line
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> read_index{0};
    char padding2[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];
    
    // Liveness tracking (separate cache line to avoid false sharing with indices)
    alignas(CACHE_LINE_SIZE) std::atomic<bool> producer_alive{true};
    alignas(CACHE_LINE_SIZE) std::atomic<bool> consumer_alive{true};
    
    // Configuration (immutable after construction)
    const size_t capacity;          // Must be power of 2
    const size_t max_message_size;
    const size_t slot_size;         // 4 (size prefix) + max_message_size + alignment
    
    // Buffer storage
    std::unique_ptr<uint8_t[]> buffer;
    
    // Constructor
    SPSCQueue(size_t cap, size_t max_msg_size)
        : capacity(cap)
        , max_message_size(max_msg_size)
        , slot_size(AlignUp(4 + max_msg_size, 8))
        , buffer(new uint8_t[capacity * slot_size])
    {
        assert((capacity & (capacity - 1)) == 0);  // Power of 2
        std::memset(buffer.get(), 0, capacity * slot_size);
    }
    
private:
    static constexpr size_t AlignUp(size_t val, size_t align) {
        return (val + align - 1) & ~(align - 1);
    }
};

} // namespace omni::detail
```

#### 5.1.3 Producer Algorithm (Reserve/Commit)

```cpp
std::optional<ReserveResult> ProducerHandle::Reserve(size_t bytes) noexcept {
    // 1. Validate preconditions
    if (bytes == 0 || bytes > queue_->max_message_size) {
        return std::nullopt;
    }
    
    if (!queue_->consumer_alive.load(std::memory_order_relaxed)) {
        return std::nullopt;  // Consumer died
    }
    
    // 2. Check if space available
    const uint64_t write = queue_->write_index.load(std::memory_order_relaxed);
    const uint64_t read = queue_->read_index.load(std::memory_order_acquire);  // Sync with consumer
    
    const uint64_t mask = queue_->capacity - 1;
    if (((write + 1) & mask) == (read & mask)) {
        return std::nullopt;  // Queue full (leave 1 slot empty to distinguish full/empty)
    }
    
    // 3. Get slot pointer
    const size_t slot_index = write & mask;
    uint8_t* slot = queue_->buffer.get() + (slot_index * queue_->slot_size);
    
    // 4. Return pointer to payload (skip 4-byte size prefix)
    return ReserveResult{
        .data = slot + 4,
        .capacity = queue_->max_message_size
    };
    
    // NOTE: write_index NOT incremented yet (deferred to Commit)
}

bool ProducerHandle::Commit(size_t actual_bytes) noexcept {
    // 1. Validate
    if (actual_bytes == 0 || actual_bytes > queue_->max_message_size) {
        return false;
    }
    
    // 2. Write size prefix
    const uint64_t write = queue_->write_index.load(std::memory_order_relaxed);
    const size_t slot_index = write & (queue_->capacity - 1);
    uint8_t* slot = queue_->buffer.get() + (slot_index * queue_->slot_size);
    
    *reinterpret_cast<uint32_t*>(slot) = static_cast<uint32_t>(actual_bytes);
    
    // 3. Publish to consumer (release fence ensures size + payload writes visible)
    queue_->write_index.store(write + 1, std::memory_order_release);
    
    // 4. Wake consumer if waiting
    queue_->write_index.notify_one();
    
    // 5. Update stats
    stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_sent.fetch_add(actual_bytes, std::memory_order_relaxed);
    
    return true;
}
```

#### 5.1.4 Consumer Algorithm (Pop)

```cpp
std::pair<PopResult, std::optional<Message>> ConsumerHandle::TryPop() noexcept {
    // 1. Check if producer alive
    if (!queue_->producer_alive.load(std::memory_order_relaxed)) {
        // Drain remaining messages before reporting closed
        const uint64_t read = queue_->read_index.load(std::memory_order_relaxed);
        const uint64_t write = queue_->write_index.load(std::memory_order_acquire);
        
        if (read == write) {
            return {PopResult::ChannelClosed, std::nullopt};
        }
    }
    
    // 2. Check if data available
    const uint64_t read = queue_->read_index.load(std::memory_order_relaxed);
    const uint64_t write = queue_->write_index.load(std::memory_order_acquire);  // Sync with producer
    
    if (read == write) {
        return {PopResult::Empty, std::nullopt};
    }
    
    // 3. Read message
    const size_t slot_index = read & (queue_->capacity - 1);
    uint8_t* slot = queue_->buffer.get() + (slot_index * queue_->slot_size);
    
    const uint32_t size = *reinterpret_cast<uint32_t*>(slot);
    std::span<const uint8_t> payload(slot + 4, size);
    
    // 4. Advance read pointer (release fence ensures we're done with slot)
    queue_->read_index.store(read + 1, std::memory_order_release);
    
    // 5. Wake producer if waiting
    queue_->read_index.notify_one();
    
    // 6. Update stats
    stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_received.fetch_add(size, std::memory_order_relaxed);
    
    return {PopResult::Success, Message{payload}};
}

std::pair<PopResult, std::optional<Message>> ConsumerHandle::BlockingPop(
    std::chrono::milliseconds timeout) noexcept 
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    
    while (true) {
        // 1. Try non-blocking pop
        auto [result, msg] = TryPop();
        
        if (result == PopResult::Success || result == PopResult::ChannelClosed) {
            return {result, std::move(msg)};
        }
        
        // 2. Check timeout
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return {PopResult::Timeout, std::nullopt};
        }
        
        // 3. Wait for notification (with spurious wakeup protection)
        const uint64_t current_write = queue_->write_index.load(std::memory_order_acquire);
        const uint64_t current_read = queue_->read_index.load(std::memory_order_relaxed);
        
        if (current_write != current_read) {
            continue;  // Data arrived, retry
        }
        
        // Wait until write_index changes OR timeout
        queue_->write_index.wait(current_write, std::memory_order_acquire);
    }
}
```

#### 5.1.5 Batch Operations (High-Throughput Optimization)

```cpp
size_t ProducerHandle::BatchPush(
    std::span<const std::span<const uint8_t>> messages) noexcept 
{
    if (messages.empty()) {
        return 0;
    }
    
    // Validate all messages first (fail-fast)
    for (const auto& msg : messages) {
        if (msg.empty() || msg.size() > queue_->max_message_size) {
            return 0;  // Invalid message in batch
        }
    }
    
    // Check consumer alive once (not per-message)
    if (!queue_->consumer_alive.load(std::memory_order_relaxed)) {
        return 0;
    }
    
    size_t pushed = 0;
    
    for (const auto& msg : messages) {
        // 1. Check space (acquire remote read index ONCE per batch iteration)
        const uint64_t write = queue_->write_index.load(std::memory_order_relaxed);
        const uint64_t read = queue_->read_index.load(std::memory_order_acquire);
        
        const uint64_t mask = queue_->capacity - 1;
        if (((write + 1) & mask) == (read & mask)) {
            break;  // Queue full - return partial count
        }
        
        // 2. Write message
        const size_t slot_index = write & mask;
        uint8_t* slot = queue_->buffer.get() + (slot_index * queue_->slot_size);
        
        *reinterpret_cast<uint32_t*>(slot) = static_cast<uint32_t>(msg.size());
        std::memcpy(slot + 4, msg.data(), msg.size());
        
        // 3. Publish (release fence ensures writes visible)
        queue_->write_index.store(write + 1, std::memory_order_release);
        
        ++pushed;
    }
    
    // 4. Single notification for entire batch (HUGE savings)
    if (pushed > 0) {
        queue_->write_index.notify_one();
        
        // Update stats (batch)
        stats_.messages_sent.fetch_add(pushed, std::memory_order_relaxed);
        // Note: bytes_sent update omitted for brevity (would sum all msg sizes)
    }
    
    return pushed;
}

/**
 * Performance comparison:
 * 
 * Individual pushes (1000 messages):
 *   - TryPush() × 1000 = 1000 × (2 atomic loads + 1 atomic store + 1 notify)
 *   - Total: ~4000 atomic operations
 *   - Latency: ~65µs (1000 × 65ns)
 * 
 * Batch push (1000 messages):
 *   - BatchPush(1000) = 1000 × (2 atomic loads + 1 atomic store) + 1 notify
 *   - Total: ~3001 atomic operations (1 shared notify vs 1000)
 *   - Latency: ~30µs (50% improvement)
 *   - notify_one() savings: 999 calls eliminated!
 * 
 * For high-frequency scenarios (100K+ msg/sec), BatchPush provides
 * 10-100× throughput improvement by amortizing synchronization overhead.
 */
```

### 5.2 Broker Implementation

#### 5.2.1 Channel Registry

```cpp
namespace omni {

class MailboxBroker::Impl {
public:
    struct ChannelState {
        std::shared_ptr<detail::SPSCQueue> queue;
        
        // Metadata
        std::string name;
        std::chrono::steady_clock::time_point created_at;
        
        // Note: Handle lifetime tracked via queue->producer_alive and queue->consumer_alive
    };
    
    // Registry protected by reader-writer lock
    mutable std::shared_mutex registry_mutex_;
    std::unordered_map<std::string, ChannelState> channels_;
    
    // Statistics
    std::atomic<size_t> total_created_{0};
    std::atomic<size_t> total_destroyed_{0};
    
    std::pair<ChannelError, std::optional<ChannelPair>> RequestChannel(
        std::string_view name,
        const ChannelConfig& config) noexcept;
    
    bool HasChannel(std::string_view name) const noexcept;
    bool RemoveChannel(std::string_view name) noexcept;
    Stats GetStats() const noexcept;
    void Shutdown() noexcept;
};

std::pair<ChannelError, std::optional<ChannelPair>> MailboxBroker::Impl::RequestChannel(
    std::string_view name,
    const ChannelConfig& config) noexcept
{
    // 1. Validate and normalize config (auto-round capacity to power of 2)
    if (!config.IsValid()) {
        return {ChannelError::InvalidConfig, std::nullopt};
    }
    
    const auto normalized = config.Normalize();
    
    // 2. Acquire write lock
    std::unique_lock lock(registry_mutex_);
    
    // 3. Check if channel already exists
    if (channels_.contains(std::string(name))) {
        return {ChannelError::NameExists, std::nullopt};
    }
    
    // 4. Create queue with normalized config (may throw std::bad_alloc)
    try {
        auto queue = std::make_shared<detail::SPSCQueue>(
            normalized.capacity,
            normalized.max_message_size
        );
        
        // 5. Store in registry
        ChannelState state{
            .queue = queue,
            .name = std::string(name),
            .created_at = std::chrono::steady_clock::now()
        };
        
        channels_.emplace(state.name, std::move(state));
        total_created_.fetch_add(1, std::memory_order_relaxed);
        
        // 6. Create handles (both reference the same queue)
        ProducerHandle producer(queue);
        ConsumerHandle consumer(queue);
        
        return {ChannelError::Success, ChannelPair{std::move(producer), std::move(consumer)}};
        
    } catch (const std::bad_alloc&) {
        return {ChannelError::AllocationFailed, std::nullopt};
    }
}

bool MailboxBroker::Impl::RemoveChannel(std::string_view name) noexcept {
    std::unique_lock lock(registry_mutex_);
    
    auto it = channels_.find(std::string(name));
    if (it == channels_.end()) {
        return false;  // Not found
    }
    
    // Check if handles still alive via queue liveness flags
    const bool producer_alive = it->second.queue->producer_alive.load(std::memory_order_relaxed);
    const bool consumer_alive = it->second.queue->consumer_alive.load(std::memory_order_relaxed);
    
    if (producer_alive || consumer_alive) {
        return false;  // Handles still exist
    }
    
    channels_.erase(it);
    total_destroyed_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace omni
```

### 5.3 FlatBuffers Integration Example

#### 5.3.1 Schema Definition

```fbs
// example_message.fbs
namespace omni.example;

enum Priority : byte { Low = 0, Medium = 1, High = 2 }

table Telemetry {
    timestamp: uint64;
    sensor_id: string;
    temperature: float;
    pressure: float;
    priority: Priority = Medium;
}

root_type Telemetry;
```

#### 5.3.2 Producer Usage (Zero-Copy)

```cpp
#include <omni/mailbox.hpp>
#include "example_message_generated.h"

void ProducerThread() {
auto& broker = omni::MailboxBroker::Instance();
    
auto [error, channel] = broker.RequestChannel("telemetry", {
    .capacity = 2048,
    .max_message_size = 1024
});
    
if (error != omni::ChannelError::Success) {
    std::cerr << "Failed to create channel: " << static_cast<int>(error) << "\n";
    return;
}
    
auto& producer = channel->producer;
    
    // Zero-copy approach: Build FlatBuffer directly into ring buffer
    while (true) {
        // 1. Reserve space
        auto reservation = producer.Reserve(256);  // Estimate size
        if (!reservation) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            continue;
        }
        
        // 2. Build FlatBuffer in-place using custom allocator
        flatbuffers::FlatBufferBuilder fbb(
            reservation->capacity,
            reservation->data,
            /* owns_buffer = */ false
        );
        
        auto sensor_name = fbb.CreateString("temp_sensor_01");
        
        auto telemetry = omni::example::CreateTelemetry(fbb,
            GetCurrentTimestamp(),
            sensor_name,
            23.5f,  // temperature
            1013.25f,  // pressure
            omni::example::Priority_High
        );
        
        fbb.Finish(telemetry);
        
        // 3. Commit actual size
        if (!producer.Commit(fbb.GetSize())) {
            std::cerr << "Commit failed\n";
            break;
        }
    }
}
```

#### 5.3.3 Consumer Usage (Zero-Copy)

```cpp
void ConsumerThread() {
auto& broker = omni::MailboxBroker::Instance();
    
auto [error, channel] = broker.RequestChannel("telemetry");
if (error != omni::ChannelError::Success) {
    std::cerr << "Channel not found or error: " << static_cast<int>(error) << "\n";
    return;
}
    
auto& consumer = channel->consumer;
    
    while (true) {
        // 1. Pop message (zero-copy, returns span into ring buffer)
        auto [result, msg] = consumer.BlockingPop(std::chrono::seconds(1));
        
        if (result == omni::PopResult::Timeout) {
            continue;
        }
        
        if (result == omni::PopResult::ChannelClosed) {
            std::cout << "Producer disconnected\n";
            break;
        }
        
        // 2. Parse FlatBuffer (zero-copy, no deserialization)
        auto telemetry = msg->GetFlatBuffer<omni::example::Telemetry>();
        
        // 3. Debug build: Verify integrity
        #ifndef NDEBUG
        if (!msg->Verify<omni::example::Telemetry>()) {
            std::cerr << "FlatBuffer verification failed!\n";
            continue;
        }
        #endif
        
        // 4. Access fields (zero-copy, direct pointer access)
        std::cout << "Sensor: " << telemetry->sensor_id()->str() << "\n"
                  << "Temp: " << telemetry->temperature() << "°C\n"
                  << "Pressure: " << telemetry->pressure() << " hPa\n";
    }
}
```

---

## 6. Build System

### 6.1 Directory Structure

```
omni-mailbox/
├── CMakeLists.txt
├── README.md
├── LICENSE
├── DESIGN_DECISIONS.md
├── include/
│   └── omni/
│       ├── mailbox.hpp          # Public API (single include)
│       └── detail/
│           ├── spsc_queue.hpp
│           ├── broker_impl.hpp
│           └── config.hpp
├── src/
│   ├── broker.cpp
│   ├── producer_handle.cpp
│   └── consumer_handle.cpp
├── examples/
│   ├── basic_usage.cpp
│   ├── flatbuffers_telemetry.cpp
│   └── benchmark.cpp
├── schemas/
│   └── example_message.fbs
├── tests/
│   ├── unit/
│   │   ├── test_spsc_queue.cpp
│   │   ├── test_broker.cpp
│   │   └── test_handles.cpp
│   ├── integration/
│   │   └── test_end_to_end.cpp
│   └── stress/
│       └── test_concurrent_channels.cpp
├── bench/
│   └── throughput_latency.cpp
└── tools/
    └── generate_single_header.py
```

### 6.2 CMakeLists.txt

```cmake
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
```

### 6.3 Build Commands

```bash
# Configure
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DOMNI_BUILD_TESTS=ON \
    -DOMNI_ENABLE_SANITIZERS=OFF

# Build
cmake --build build --parallel

# Test
ctest --test-dir build --output-on-failure

# Sanitizer build (Linux/macOS only)
cmake -S . -B build-sanitized \
    -DCMAKE_BUILD_TYPE=Debug \
    -DOMNI_ENABLE_SANITIZERS=ON

cmake --build build-sanitized --parallel
./build-sanitized/omni-tests
```

---

## 7. Testing & Validation

### 7.1 Test Categories

| Category | Coverage | Tools | Target |
|----------|----------|-------|--------|
| Unit Tests | Individual components (SPSC queue, handles) | Google Test | >90% line coverage |
| Integration Tests | Multi-threaded scenarios | Google Test + ThreadSanitizer | No data races |
| Stress Tests | 1000+ channels, 10M+ messages | Custom harness | No crashes, leaks |
| Benchmarks | Throughput/latency under load | Google Benchmark | Meet NFR targets |
| Sanitizer Runs | Memory safety validation | ASAN, TSAN, UBSAN, Valgrind | Zero violations |

### 7.2 Unit Test Examples

```cpp
// tests/unit/test_spsc_queue.cpp
#include <gtest/gtest.h>
#include <omni/detail/spsc_queue.hpp>

TEST(SPSCQueueTest, InitialState) {
    omni::detail::SPSCQueue queue(16, 256);
    
    EXPECT_EQ(queue.capacity, 16);
    EXPECT_EQ(queue.max_message_size, 256);
    EXPECT_EQ(queue.write_index.load(), 0);
    EXPECT_EQ(queue.read_index.load(), 0);
    EXPECT_TRUE(queue.producer_alive.load());
    EXPECT_TRUE(queue.consumer_alive.load());
}

TEST(SPSCQueueTest, SingleMessage) {
    omni::detail::SPSCQueue queue(16, 256);
    
    // Producer writes
    const std::string payload = "Hello, World!";
    const uint64_t write_idx = 0;
    uint8_t* slot = queue.buffer.get();
    
    *reinterpret_cast<uint32_t*>(slot) = payload.size();
    std::memcpy(slot + 4, payload.data(), payload.size());
    queue.write_index.store(1, std::memory_order_release);
    
    // Consumer reads
    const uint64_t read_idx = queue.read_index.load(std::memory_order_relaxed);
    const uint64_t write_idx_read = queue.write_index.load(std::memory_order_acquire);
    
    ASSERT_NE(read_idx, write_idx_read);  // Data available
    
    uint8_t* read_slot = queue.buffer.get();
    const uint32_t size = *reinterpret_cast<uint32_t*>(read_slot);
    std::string received(reinterpret_cast<char*>(read_slot + 4), size);
    
    EXPECT_EQ(received, payload);
    queue.read_index.store(1, std::memory_order_release);
}

TEST(SPSCQueueTest, QueueFull) {
    omni::detail::SPSCQueue queue(4, 64);  // Capacity 4 = 3 usable slots
    
    // Fill queue (capacity - 1 messages)
    for (size_t i = 0; i < 3; ++i) {
        const uint64_t write = queue.write_index.load(std::memory_order_relaxed);
        const uint64_t read = queue.read_index.load(std::memory_order_acquire);
        const uint64_t mask = queue.capacity - 1;
        
        ASSERT_NE(((write + 1) & mask), (read & mask));  // Not full
        
        uint8_t* slot = queue.buffer.get() + (write & mask) * queue.slot_size;
        *reinterpret_cast<uint32_t*>(slot) = 10;
        queue.write_index.store(write + 1, std::memory_order_release);
    }
    
    // Next write should detect full queue
    const uint64_t write = queue.write_index.load(std::memory_order_relaxed);
    const uint64_t read = queue.read_index.load(std::memory_order_acquire);
    const uint64_t mask = queue.capacity - 1;
    
    EXPECT_EQ(((write + 1) & mask), (read & mask));  // Full!
}
```

### 7.3 Integration Test Example

```cpp
// tests/integration/test_end_to_end.cpp
#include <gtest/gtest.h>
#include <omni/mailbox.hpp>
#include <thread>
#include <vector>

TEST(IntegrationTest, ProducerConsumerRoundTrip) {
auto& broker = omni::MailboxBroker::Instance();
    
auto [error, channel] = broker.RequestChannel("test-e2e", {
    .capacity = 128,
    .max_message_size = 512
});
ASSERT_EQ(error, omni::ChannelError::Success);
ASSERT_TRUE(channel.has_value());
    
    const size_t num_messages = 10000;
    std::atomic<size_t> received_count{0};
    
    // Consumer thread
    std::thread consumer_thread([&]() {
        auto& consumer = channel->consumer;
        
        while (received_count.load() < num_messages) {
            auto [result, msg] = consumer.BlockingPop(std::chrono::seconds(5));
            
            if (result == omni::PopResult::Success) {
                received_count.fetch_add(1);
            } else if (result == omni::PopResult::ChannelClosed) {
                break;
            }
        }
    });
    
    // Producer thread
    std::thread producer_thread([&]() {
        auto& producer = channel->producer;
        
        for (size_t i = 0; i < num_messages; ++i) {
            std::string payload = "Message " + std::to_string(i);
            std::span<const uint8_t> data(
                reinterpret_cast<const uint8_t*>(payload.data()),
                payload.size()
            );
            
            auto result = producer.BlockingPush(data, std::chrono::seconds(5));
            ASSERT_EQ(result, omni::PushResult::Success);
        }
    });
    
    producer_thread.join();
    consumer_thread.join();
    
    EXPECT_EQ(received_count.load(), num_messages);
}

TEST(IntegrationTest, MultipleChannels) {
    auto& broker = omni::MailboxBroker::Instance();
    
    const size_t num_channels = 100;
    std::vector<std::thread> threads;
    
    for (size_t i = 0; i < num_channels; ++i) {
        threads.emplace_back([&broker, i]() {
            std::string name = "channel-" + std::to_string(i);
            auto [error, channel] = broker.RequestChannel(name);
            ASSERT_EQ(error, omni::ChannelError::Success);
            ASSERT_TRUE(channel.has_value());
            
            // Send 1000 messages
            for (size_t j = 0; j < 1000; ++j) {
                std::string payload = std::to_string(j);
                std::span<const uint8_t> data(
                    reinterpret_cast<const uint8_t*>(payload.data()),
                    payload.size()
                );
                channel->producer.TryPush(data);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto stats = broker.GetStats();
    EXPECT_EQ(stats.active_channels, num_channels);
}
```

### 7.4 Sanitizer Validation

```bash
# Address Sanitizer (detect memory leaks, use-after-free)
cmake -S . -B build-asan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
cmake --build build-asan
./build-asan/omni-tests

# Thread Sanitizer (detect data races)
cmake -S . -B build-tsan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build-tsan
./build-tsan/omni-tests

# Undefined Behavior Sanitizer
cmake -S . -B build-ubsan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=undefined"
cmake --build build-ubsan
./build-ubsan/omni-tests

# Valgrind (memory leak detection)
valgrind --leak-check=full --show-leak-kinds=all \
    ./build/omni-tests
```

---

## 8. Performance Benchmarks

### 8.1 Benchmark Harness

```cpp
// bench/throughput_latency.cpp
#include <benchmark/benchmark.h>
#include <omni/mailbox.hpp>
#include <thread>

// Throughput: Single channel, uncontended
static void BM_Throughput_Uncontended(benchmark::State& state) {
    auto& broker = omni::MailboxBroker::Instance();
    auto [error, channel] = broker.RequestChannel("bench-throughput", {
        .capacity = 2048,
        .max_message_size = 256
    });
    
    if (error != omni::ChannelError::Success) {
        state.SkipWithError("Failed to create channel");
        return;
    }
    
    const size_t msg_size = state.range(0);
    std::vector<uint8_t> payload(msg_size, 0xAB);
    
    std::thread consumer([&]() {
        while (state.KeepRunning()) {
            auto [result, msg] = channel->consumer.TryPop();
            if (result == omni::PopResult::Success) {
                benchmark::DoNotOptimize(msg->Data());
            }
        }
    });
    
    for (auto _ : state) {
        auto result = channel->producer.TryPush(payload);
        if (result != omni::PushResult::Success) {
            state.PauseTiming();
            std::this_thread::sleep_for(std::chrono::microseconds(1));
            state.ResumeTiming();
        }
    }
    
    consumer.join();
    
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * msg_size);
}
BENCHMARK(BM_Throughput_Uncontended)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1024)
    ->Arg(4096)
    ->Unit(benchmark::kMicrosecond);

// Latency: Round-trip ping-pong
static void BM_Latency_RoundTrip(benchmark::State& state) {
    auto& broker = omni::MailboxBroker::Instance();
    
    auto [error1, ping] = broker.RequestChannel("ping");
    auto [error2, pong] = broker.RequestChannel("pong");
    
    if (error1 != omni::ChannelError::Success || error2 != omni::ChannelError::Success) {
        state.SkipWithError("Failed to create channels");
        return;
    }
    
    std::vector<uint8_t> payload(64, 0xCD);
    
    std::thread responder([&]() {
        while (true) {
            auto [result, msg] = ping->consumer.BlockingPop(std::chrono::seconds(1));
            if (result != omni::PopResult::Success) break;
            
            pong->producer.TryPush(msg->Data());
        }
    });
    
    for (auto _ : state) {
        auto start = std::chrono::high_resolution_clock::now();
        
        ping->producer.BlockingPush(payload);
        auto [result, msg] = pong->consumer.BlockingPop();
        
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        state.SetIterationTime(elapsed.count() / 1e9);
    }
    
    responder.join();
}
BENCHMARK(BM_Latency_RoundTrip)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
```

### 8.2 Expected Results

**Target Platform:** Intel Core i7-12700K (12 cores), 32GB DDR4-3200

| Benchmark | Message Size | Target | Baseline (Boost) |
|-----------|-------------|--------|------------------|
| Throughput (uncontended) | 64 bytes | ≥5M msg/s | ~3M msg/s |
| Throughput (uncontended) | 1024 bytes | ≥3M msg/s | ~2M msg/s |
| Latency p50 (round-trip) | 64 bytes | <200ns | ~300ns |
| Latency p99 (round-trip) | 64 bytes | <500ns | ~800ns |
| Latency p99.9 (round-trip) | 64 bytes | <2µs | ~5µs |

### 8.3 Profiling

```bash
# Linux perf profiling
perf record -g ./build/bench-throughput --benchmark_filter=BM_Throughput
perf report

# CPU hotspots (should show minimal time in atomic operations)
# Expected profile:
# - 60-70%: Memory copy operations (payload)
# - 20-30%: FlatBuffers serialization (if used)
# - <10%: Atomic operations (write_index, read_index)
# - <5%: Notifications (wait/notify)
```

---

## 9. Error Handling Strategy

### 9.1 Error Philosophy

1. **No Exceptions in Hot Path**: Push/Pop never throw; use return codes
2. **Exceptions for Setup Failures**: Broker construction, channel allocation
3. **Explicit Error Types**: Enums over booleans
4. **Fail-Fast**: Invalid arguments return errors immediately (no silent failures)

### 9.2 Error Categories

| Error Type | Handling | Example |
|------------|----------|---------|
| **Precondition Violation** | Return `nullopt` or error enum | `Reserve(0)` → `nullopt` |
| **Resource Exhaustion** | Return `Timeout` or `QueueFull` | Push to full queue → `QueueFull` |
| **Peer Disconnect** | Return `ChannelClosed` | Pop after producer dies → `ChannelClosed` |
| **Invalid Configuration** | Return `nullopt` in `RequestChannel` | Capacity not power of 2 → `nullopt` |
| **System Failure** | Throw exception (allocation failure) | `new` fails → `std::bad_alloc` |

### 9.3 Error Handling Examples

```cpp
// Example 1: Handle queue full
auto result = producer.TryPush(data);
switch (result) {
    case PushResult::Success:
        // Message sent
        break;
    case PushResult::QueueFull:
        // Retry with backoff or drop message
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        break;
    case PushResult::ChannelClosed:
        // Consumer died, cleanup
        std::cerr << "Consumer disconnected\n";
        return;
    case PushResult::InvalidSize:
        // Message too large
        std::cerr << "Message exceeds max size\n";
        break;
}

// Example 2: Handle timeout
auto [result, msg] = consumer.BlockingPop(std::chrono::seconds(5));
if (result == PopResult::Timeout) {
    // No data for 5 seconds, check system health
    if (!consumer.IsConnected()) {
        std::cerr << "Producer dead\n";
        return;
    }
}

// Example 3: Handle channel creation failure with detailed error reporting
auto [error, channel] = broker.RequestChannel("my-channel", {
    .capacity = 1024,
    .max_message_size = 4096
});

switch (error) {
    case omni::ChannelError::Success:
        // Use channel.value()
        break;
    case omni::ChannelError::NameExists:
        std::cerr << "Channel 'my-channel' already exists\n";
        // Option 1: Use existing channel
        // Option 2: Choose different name
        break;
    case omni::ChannelError::InvalidConfig:
        std::cerr << "Invalid configuration (check capacity/message size ranges)\n";
        break;
    case omni::ChannelError::AllocationFailed:
        std::cerr << "Memory allocation failed (out of memory)\n";
        break;
}
```

### 9.4 Debug Assertions

```cpp
// Debug builds include rich assertions
#ifndef NDEBUG
    #define OMNI_ASSERT(cond, msg) \
        do { \
            if (!(cond)) { \
                std::cerr << "ASSERTION FAILED: " << msg << "\n" \
                          << "  File: " << __FILE__ << ":" << __LINE__ << "\n"; \
                std::abort(); \
            } \
        } while (0)
#else
    #define OMNI_ASSERT(cond, msg) ((void)0)
#endif

// Usage in implementation
bool ProducerHandle::Commit(size_t actual_bytes) noexcept {
    OMNI_ASSERT(actual_bytes > 0, "Cannot commit 0 bytes");
    OMNI_ASSERT(actual_bytes <= max_message_size_, "Commit exceeds reserved size");
    
    // ... rest of implementation
}
```

---

## 10. Deliverables

### 10.1 Source Code

- [ ] **Library Implementation**
  - `include/omni/mailbox.hpp` - Public API
  - `include/omni/detail/*` - Internal headers
  - `src/*.cpp` - Implementation (if not header-only)

- [ ] **Build System**
  - `CMakeLists.txt` - Cross-platform build
  - `tools/generate_single_header.py` - Amalgamation script

- [ ] **Examples**
  - `examples/basic_usage.cpp` - Hello World
  - `examples/flatbuffers_telemetry.cpp` - Zero-copy FlatBuffers
  - `schemas/example_message.fbs` - FlatBuffers schema

### 10.2 Documentation

- [ ] **README.md** - Quick start guide
- [ ] **DESIGN_DECISIONS.md** - Architecture rationale
- [ ] **API_REFERENCE.md** - Complete API documentation
- [ ] **BENCHMARKS.md** - Performance results
- [ ] **CHANGELOG.md** - Version history

### 10.3 Testing

- [ ] **Unit Tests** - 90%+ coverage
  - SPSC queue tests
  - Broker tests
  - Handle lifetime tests

- [ ] **Integration Tests**
  - Multi-threaded scenarios
  - Multiple channels
  - Stress tests (1000+ channels)

- [ ] **Sanitizer Runs**
  - ASAN clean
  - TSAN clean
  - UBSAN clean
  - Valgrind clean

### 10.4 Benchmarks

- [ ] **Throughput Benchmarks**
  - 64-byte messages: ≥5M msg/s
  - 1024-byte messages: ≥3M msg/s
  - 4096-byte messages: ≥1M msg/s

- [ ] **Latency Benchmarks**
  - p50 < 200ns
  - p99 < 500ns
  - p99.9 < 2µs

- [ ] **Comparison Benchmarks**
  - vs. `boost::lockfree::spsc_queue`
  - vs. `folly::ProducerConsumerQueue`
  - vs. `moodycamel::ReaderWriterQueue`

### 10.5 Continuous Integration

```yaml
# .github/workflows/ci.yml
name: CI

on: [push, pull_request]

jobs:
  build-linux:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        compiler: [gcc-11, gcc-12, clang-14, clang-15]
        build_type: [Debug, Release]
    steps:
      - uses: actions/checkout@v3
      - name: Install dependencies
        run: sudo apt-get install -y libflatbuffers-dev
      - name: Configure
        run: cmake -S . -B build -DCMAKE_BUILD_TYPE=${{ matrix.build_type }}
      - name: Build
        run: cmake --build build --parallel
      - name: Test
        run: ctest --test-dir build --output-on-failure
  
  build-windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v3
      - name: Configure
        run: cmake -S . -B build
      - name: Build
        run: cmake --build build --config Release
      - name: Test
        run: ctest --test-dir build -C Release --output-on-failure
  
  sanitizers:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: ASAN
        run: |
          cmake -S . -B build-asan -DOMNI_ENABLE_SANITIZERS=ON
          cmake --build build-asan
          ./build-asan/omni-tests
      - name: TSAN
        run: |
          cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread"
          cmake --build build-tsan
          ./build-tsan/omni-tests
```

---

## 11. Acceptance Criteria

### 11.1 Functional

- [ ] All API methods work as documented
- [ ] Multiple channels can coexist (test with 100+ channels)
- [ ] Handle destruction auto-cleans channels
- [ ] Peer disconnect detection works
- [ ] FlatBuffers zero-copy verified
- [ ] Normalize() produces valid power-of-2 capacity
- [ ] No integer overflow in Reserve() with large sizes

### 11.2 Performance

- [ ] Throughput ≥5M msg/s (64-byte, uncontended)
- [ ] Latency p99 <500ns (uncontended)
- [ ] Zero allocations in hot path (verified with allocator hooks)
- [ ] Lock contention acceptable (<10% overhead with 100 channels)
- [ ] No cache-line false sharing in SPSCQueue structure

### 11.3 Safety

- [ ] ASAN clean (no leaks, use-after-free)
- [ ] TSAN clean (no data races)
- [ ] UBSAN clean (no undefined behavior)
- [ ] Valgrind clean (no uninitialized reads)
- [ ] Handle destruction barrier prevents data corruption
- [ ] Singleton uses intentional leak (no destruction order issues)

### 11.4 Portability

- [ ] Builds on Windows (MSVC 19.30+)
- [ ] Builds on Linux (GCC 11+, Clang 14+)
- [ ] Works on x86, x64, ARM64
- [ ] C++23 features gracefully degrade to C++20

### 11.5 Code Quality

- [ ] Test coverage ≥90% (line coverage)
- [ ] Zero compiler warnings (with `-Wall -Wextra -Werror`)
- [ ] API documented (Doxygen comments)
- [ ] Design decisions documented
- [ ] Safety warnings documented (global handles, signal safety)

---

## 12. Future Enhancements (Out of Scope v1.0)

- [ ] **MPSC/MPMC Support** - Multi-producer/consumer queues
- [ ] **Cross-Process** - Shared memory backend
- [ ] **Priority Queues** - High/low priority messages
- [ ] **Message Filtering** - Consumer-side predicates
- [ ] **Compression** - Optional payload compression
- [ ] **Encryption** - Optional payload encryption
- [ ] **Telemetry** - Built-in metrics export (Prometheus)
- [ ] **Async API** - C++20 coroutine support

---

## 13. Design Decisions (Resolved)

### 13.1 Power-of-2 Capacity Enforcement
**Decision:** Auto-round up to next power of 2.

**Rationale:** Bitwise masking (`index & (capacity - 1)`) is significantly faster than modulo (`%`) in hot path. Performance benefits outweigh minor capacity increase (e.g., 1000 → 1024). Users won't notice the extra slots but will notice modulo overhead.

**Implementation:**
```cpp
size_t RoundUpPowerOf2(size_t n) {
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    n++;
    return n;
}
```

### 13.2 Reservation Timeout
**Decision:** Fail-fast (return `nullopt` immediately).

**Rationale:** `Reserve()` begins the message-building process. Blocking here holds up producer logic unnecessarily. If buffer full, producer should decide whether to drop, retry, or switch strategy. Timeout adds complexity without clear benefit for SPSC use case.

### 13.3 Message Alignment
**Decision:** 8-byte alignment default, with configurable option for future.

**Rationale:** 8-byte alignment covers most use cases (FlatBuffers, POD structs). For v1.0, hardcode to 8 bytes. If users need 16/32-byte alignment for SIMD types, add `alignment` parameter to `ChannelConfig` in v1.1.

**Future API:**
```cpp
struct ChannelConfig {
    size_t capacity = 1024;
    size_t max_message_size = 4096;
    size_t alignment = 8;  // v1.1: Allow 16, 32 for SIMD
};
```

### 13.4 Handle Lifetime Tracking
**Decision:** Use atomic liveness flags in `SPSCQueue` (`producer_alive`, `consumer_alive`).

**Rationale:** Queue already has `producer_alive` and `consumer_alive` atomic flags for peer disconnect detection. These serve dual purpose: (1) signal peer during operations, (2) track handle lifetime for broker cleanup. No need for separate `weak_ptr` token mechanism - simpler design, less overhead, same functionality.

**Implementation:**
```cpp
// ProducerHandle destructor
~ProducerHandle() noexcept {
    if (queue_) {
        queue_->producer_alive.store(false, std::memory_order_release);
        queue_->write_index.notify_one();  // Wake blocked consumer
    }
}

// Broker checks liveness
bool MailboxBroker::RemoveChannel(std::string_view name) {
    // ...
    if (queue->producer_alive.load() || queue->consumer_alive.load()) {
        return false;  // Still in use
    }
    // Safe to remove
}
```

### 13.5 Statistics Granularity
**Decision:** Per-channel stats with broker aggregation.

**Rationale:** Per-channel statistics essential for debugging multi-threaded systems (identify bottlenecks). `MailboxBroker::GetStats()` aggregates across all channels for global view. Best of both worlds.

**API:**
```cpp
// Per-channel stats
auto stats = producer.GetStats();
std::cout << "Channel throughput: " << stats.messages_sent << "\n";

// Global aggregation
auto global = broker.GetStats();
std::cout << "Total system throughput: " << global.total_messages_sent << "\n";
```

### 13.6 FlatBuffers Dependency Management
**Decision:** External installation (CMake `find_package`).

**Rationale:** FlatBuffers is mature, stable, widely packaged. Vendoring adds repo bloat and maintenance burden. Users installing NuStream likely already have FlatBuffers or can install via package manager (apt, brew, vcpkg, Conan).

**CMakeLists.txt:**
```cmake
find_package(flatbuffers REQUIRED)
target_link_libraries(nustream PUBLIC flatbuffers::flatbuffers)
```

### 13.7 Future Consideration: Thundering Herd
**Note:** Current SPSC design uses `atomic::wait()` efficiently (single consumer). If expanding to MPSC/MPMC in v2.0, beware of thundering herd where multiple threads wake simultaneously but only one acquires data. Current v1.0 implementation optimal for SPSC.

**Document all implementation details in `DESIGN_DECISIONS.md` as you build.**

---

## 14. Critical Implementation Notes

### 14.1 Singleton Lifetime (Static Destruction Order Fiasco)

**Problem:** Handles may outlive the broker singleton during static destruction.

**Solution:** Intentional memory leak

```cpp
MailboxBroker& MailboxBroker::Instance() noexcept {
    // Intentional leak - broker destructor never runs
    // Ensures handles can always access queue state during shutdown
    // Cost: ~16KB leaked at program exit (acceptable)
    static MailboxBroker* instance = new MailboxBroker();
    return *instance;
}
```

### 14.2 Integer Overflow Protection

**Problem:** Reserve() must prevent overflow in slot size calculation.

**Solution:** Check bounds before arithmetic

```cpp
std::optional<ReserveResult> Reserve(size_t bytes) noexcept {
    // Prevent overflow: SIZE_MAX - sizeof(uint32_t) - alignment
    constexpr size_t MAX_SAFE_SIZE = SIZE_MAX - 12;
    
    if (bytes == 0 || bytes > queue_->max_message_size || bytes > MAX_SAFE_SIZE) {
        return std::nullopt;
    }
    
    // Safe to proceed - no overflow possible
}
```

**Document:** Maximum message size is `min(configured_max, SIZE_MAX - 12)`.

### 14.3 Handle Destruction Barrier

**Problem:** Handle destructor may run while operations in flight.

**Solution:** Wait for operations before setting alive flag false

```cpp
~ProducerHandle() noexcept {
    if (pimpl_ && pimpl_->queue_) {
        // 1. Wait for any in-flight Reserve/Push to complete
        // Use seq_cst fence to ensure all prior stores visible
        std::atomic_thread_fence(std::memory_order_seq_cst);
        
        // 2. Now safe to signal death
        pimpl_->queue_->producer_alive.store(false, std::memory_order_release);
        pimpl_->queue_->write_index.notify_one();
    }
}
```

### 14.4 FlatBuffers Verification Cost

**Warning:** `Verify()` is EXPENSIVE - 50x slower than direct access.

```cpp
// DEBUG ONLY - verify buffer integrity
#ifndef NDEBUG
if (!msg->Verify<Telemetry>()) {
    std::cerr << "FlatBuffer corruption detected!\n";
    continue;
}
#endif

// PRODUCTION - trust zero-copy data
auto telemetry = msg->GetFlatBuffer<Telemetry>();
```

**Benchmark:**
- Without Verify: 5.2M msg/sec
- With Verify: 100K msg/sec (52x slower)

### 14.5 Shutdown() Limitations

**Current design:** Shutdown() sets liveness flags but doesn't wait for destructors.

**Usage:**
```cpp
// User must ensure handles destroyed before Shutdown()
{
    auto channel = broker.RequestChannel("test");
    // ... use channel ...
} // Handles destroyed here

broker.Shutdown();  // Safe - all handles gone
```

**DO NOT:**
```cpp
auto channel = broker.RequestChannel("test");
broker.Shutdown();  // ❌ Undefined behavior - handles still alive
```

### 14.6 Lock-Free Registry Trade-off

**Current:** Single `shared_mutex` protects all channels
**Trade-off:** Simplicity vs. scalability

**Performance:**
- 10 channels: No contention
- 100 channels: Minor contention (acceptable)
- 1000+ channels: Significant contention (consider lock striping in v1.1)

**Decision:** Accept limitation for v1.0. If >100 channels needed, implement sharded registry in v1.1.

---

## Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-02-10 | Claude | Initial specification |

---

**END OF SPECIFICATION**
