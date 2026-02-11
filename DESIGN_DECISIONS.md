# OmniMailbox: Design Decisions

This document records the key design choices made during the development of OmniMailbox. Each decision includes the problem statement, alternatives considered, the final decision, rationale, and trade-offs.

**Last Updated:** 2026-02-10  
**Version:** 1.0

---

## Table of Contents

1. [Power-of-2 Capacity Enforcement](#1-power-of-2-capacity-enforcement)
2. [Reservation Timeout Decision](#2-reservation-timeout-decision)
3. [Message Alignment](#3-message-alignment)
4. [Handle Lifetime Tracking](#4-handle-lifetime-tracking)
5. [Statistics Granularity](#5-statistics-granularity)
6. [FlatBuffers Dependency Management](#6-flatbuffers-dependency-management)
7. [Singleton Lifetime](#7-singleton-lifetime)
8. [Overflow Protection](#8-overflow-protection)
9. [Destruction Barrier](#9-destruction-barrier)

---

## 1. Power-of-2 Capacity Enforcement

### Problem Statement

Ring buffer index wrapping can be implemented using either modulo arithmetic (`index % capacity`) or bitwise masking (`index & (capacity - 1)`). Bitwise masking requires capacity to be a power of 2.

### Alternatives Considered

1. **Allow arbitrary capacity, use modulo operator**
   - Pros: User gets exact capacity requested
   - Cons: Modulo operation significantly slower in hot path (division instruction vs. bitwise AND)

2. **Reject non-power-of-2 capacities, return error**
   - Pros: Explicit contract, no surprises
   - Cons: Poor user experience, requires manual calculation

3. **Auto-round up to next power of 2**
   - Pros: Transparent optimization, minimal capacity increase
   - Cons: User gets slightly more capacity than requested

### Decision Made

**Auto-round up to next power of 2.**

### Rationale

Bitwise masking (`index & (capacity - 1)`) is significantly faster than modulo (`%`) in the hot path. Performance benefits outweigh minor capacity increase (e.g., 1000 → 1024). Users won't notice the extra slots but will notice modulo overhead.

### Implementation

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

### Trade-offs

- **Pros:** 
  - Consistent high performance in hot path
  - Transparent to user (happens during normalization)
- **Cons:** 
  - Slight memory overhead (worst case: user requests 513, gets 1024 - 2x overhead)
  - Capacity in `GetStats()` may not match requested value

---

## 2. Reservation Timeout Decision

### Problem Statement

When `Reserve()` is called to begin building a FlatBuffers message, the ring buffer might be full. Should we block with a timeout or fail immediately?

### Alternatives Considered

1. **Block with configurable timeout**
   - Pros: Automatic retry logic, producer doesn't need to handle full buffer
   - Cons: Holds up producer thread, adds complexity to API and implementation

2. **Block indefinitely until space available**
   - Pros: Simplest for user (no error handling needed)
   - Cons: Risk of deadlock if consumer dies, no way to cancel

3. **Fail-fast (return `nullopt` immediately)**
   - Pros: Producer maintains control flow, can decide retry strategy
   - Cons: User must implement own retry logic if desired

### Decision Made

**Fail-fast (return `nullopt` immediately).**

### Rationale

`Reserve()` begins the message-building process. Blocking here holds up producer logic unnecessarily. If buffer is full, producer should decide whether to drop, retry, or switch strategy. Timeout adds complexity without clear benefit for SPSC use case.

### Implementation

```cpp
std::optional<ReserveResult> ProducerHandle::Reserve(size_t bytes) noexcept {
    if (is_full()) {
        return std::nullopt;  // Immediate return
    }
    // ... proceed with reservation ...
}
```

### Trade-offs

- **Pros:**
  - Predictable, fast failure path
  - Producer retains control over backpressure handling
  - Simpler implementation (no timeout tracking)
- **Cons:**
  - User must implement own retry logic for high-throughput scenarios
  - More boilerplate code at call sites

---

## 3. Message Alignment

### Problem Statement

Messages stored in the ring buffer must be aligned for safe access. What alignment should we enforce?

### Alternatives Considered

1. **No alignment (byte-aligned)**
   - Pros: Maximum capacity utilization
   - Cons: Undefined behavior on ARM for unaligned access, potential performance penalty

2. **4-byte alignment**
   - Pros: Sufficient for most primitive types (int32, float)
   - Cons: Insufficient for int64, double, pointers on 64-bit systems

3. **8-byte alignment (default)**
   - Pros: Covers FlatBuffers requirements, POD structs, all primitives on 64-bit
   - Cons: Minor capacity waste for small messages

4. **16/32-byte alignment for SIMD**
   - Pros: Optimal for SSE/AVX vector operations
   - Cons: Significant capacity waste for typical use cases

### Decision Made

**8-byte alignment default, with configurable option reserved for future.**

### Rationale

8-byte alignment covers most use cases (FlatBuffers, POD structs, all standard types). For v1.0, hardcode to 8 bytes. If users need 16/32-byte alignment for SIMD types, add `alignment` parameter to `ChannelConfig` in v1.1.

### Implementation

```cpp
// Current v1.0
constexpr size_t MESSAGE_ALIGNMENT = 8;

// Future v1.1 API
struct ChannelConfig {
    size_t capacity = 1024;
    size_t max_message_size = 4096;
    size_t alignment = 8;  // Allow 16, 32 for SIMD
};
```

### Trade-offs

- **Pros:**
  - Safe for all standard C++ types and FlatBuffers
  - Minimal capacity waste for typical message sizes
  - Simple implementation (single codepath)
- **Cons:**
  - Not optimal for SIMD workloads (requires future enhancement)
  - Small messages (1-7 bytes) waste 7 bytes on average due to padding

---

## 4. Handle Lifetime Tracking

### Problem Statement

The `MailboxBroker` needs to know when producer/consumer handles are destroyed to safely clean up channels. How should handle lifetime be tracked?

### Alternatives Considered

1. **Shared ownership (`shared_ptr` reference counting)**
   - Pros: Automatic cleanup when all handles destroyed
   - Cons: Refcount contention in hot path, harder to detect single peer disconnect

2. **Separate token mechanism (`weak_ptr` tokens)**
   - Pros: Decouples handle from queue lifetime
   - Cons: Additional complexity, extra indirection, still requires liveness flags for peer detection

3. **Atomic liveness flags in queue**
   - Pros: Dual purpose (peer disconnect + lifetime tracking), minimal overhead
   - Cons: Requires discipline (handles must set flag in destructor)

### Decision Made

**Use atomic liveness flags in `SPSCQueue` (`producer_alive`, `consumer_alive`).**

### Rationale

Queue already has `producer_alive` and `consumer_alive` atomic flags for peer disconnect detection. These serve dual purpose:
1. Signal peer during operations (detect disconnect)
2. Track handle lifetime for broker cleanup

No need for separate `weak_ptr` token mechanism - simpler design, less overhead, same functionality.

### Implementation

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

### Trade-offs

- **Pros:**
  - Minimal overhead (flags already needed for peer detection)
  - Simple implementation (no additional synchronization primitives)
  - Clear ownership semantics (handle sets flag in destructor)
- **Cons:**
  - Requires correct destructor implementation (flag must be set)
  - No automatic cleanup if handle leaks (but leak is still detectable)

---

## 5. Statistics Granularity

### Problem Statement

Performance statistics (messages sent, bytes transferred, etc.) are essential for debugging. Should stats be global, per-channel, or both?

### Alternatives Considered

1. **Global statistics only**
   - Pros: Minimal overhead, single aggregation point
   - Cons: Cannot identify bottlenecks in multi-channel systems

2. **Per-channel statistics only**
   - Pros: Detailed per-channel metrics
   - Cons: No global view without manual aggregation

3. **Per-channel with broker aggregation**
   - Pros: Best of both worlds (detailed + global view)
   - Cons: Slight duplication of aggregation logic

### Decision Made

**Per-channel stats with broker aggregation.**

### Rationale

Per-channel statistics essential for debugging multi-threaded systems (identify bottlenecks). `MailboxBroker::GetStats()` aggregates across all channels for global view. Best of both worlds.

### Implementation

```cpp
// Per-channel stats
auto stats = producer.GetStats();
std::cout << "Channel throughput: " << stats.messages_sent << "\n";

// Global aggregation
auto global = broker.GetStats();
std::cout << "Total system throughput: " << global.total_messages_sent << "\n";
```

### Trade-offs

- **Pros:**
  - Detailed diagnostics per channel
  - Global overview for system health monitoring
  - Flexible querying (per-channel or aggregate)
- **Cons:**
  - Atomic counter overhead per channel (negligible: relaxed loads in hot path)
  - Slight API duplication (`producer.GetStats()` vs. `broker.GetStats()`)

---

## 6. FlatBuffers Dependency Management

### Problem Statement

OmniMailbox integrates with FlatBuffers for zero-copy serialization. Should FlatBuffers be vendored (included in repo) or installed externally?

### Alternatives Considered

1. **Vendor FlatBuffers (Git submodule or copy)**
   - Pros: Guaranteed version compatibility, works out-of-box
   - Cons: Repo bloat (+10MB), maintenance burden (security updates), license implications

2. **Header-only wrapper (minimal vendoring)**
   - Pros: Smaller footprint
   - Cons: Still requires vendoring core headers, version conflicts possible

3. **External installation (CMake `find_package`)**
   - Pros: Users control version, minimal repo size, leverages system package managers
   - Cons: Build complexity (users must install dependency first)

### Decision Made

**External installation (CMake `find_package`).**

### Rationale

FlatBuffers is mature, stable, widely packaged. Vendoring adds repo bloat and maintenance burden. Users installing OmniMailbox likely already have FlatBuffers or can install via package manager (apt, brew, vcpkg, Conan).

### Implementation

```cmake
# CMakeLists.txt
find_package(flatbuffers REQUIRED)
target_link_libraries(omni_mailbox PUBLIC flatbuffers::flatbuffers)
```

### Trade-offs

- **Pros:**
  - Minimal repository size (no vendor directory)
  - Users control FlatBuffers version (can upgrade independently)
  - Leverages existing package manager ecosystems
- **Cons:**
  - Build prerequisite (users must install FlatBuffers first)
  - Potential version compatibility issues (mitigated by `find_package` version checks)

---

## 7. Singleton Lifetime

### Problem Statement

The `MailboxBroker` is implemented as a singleton to provide centralized channel management. During static destruction at program exit, handles may outlive the broker singleton, leading to use-after-free.

### Alternatives Considered

1. **Normal static singleton (automatic destruction)**
   - Pros: Clean shutdown, destructor runs
   - Cons: Static destruction order fiasco - handles may try to access destroyed broker

2. **Reference counting to delay destruction**
   - Pros: Broker destroyed only after all handles
   - Cons: Complex implementation, refcount overhead in handle constructors/destructors

3. **Intentional memory leak (never destroy broker)**
   - Pros: Simple, guarantees handles can always access broker state
   - Cons: Memory leak at program exit (~16KB)

### Decision Made

**Intentional memory leak.**

### Rationale

Handles must be able to safely destroy themselves during static destruction. By never destroying the broker singleton, we guarantee the queue state remains accessible. The cost is minimal (~16KB leaked at program exit), and program exit cleanup is handled by the OS anyway.

### Implementation

```cpp
MailboxBroker& MailboxBroker::Instance() noexcept {
    // Intentional leak - broker destructor never runs
    // Ensures handles can always access queue state during shutdown
    // Cost: ~16KB leaked at program exit (acceptable)
    static MailboxBroker* instance = new MailboxBroker();
    return *instance;
}
```

### Trade-offs

- **Pros:**
  - Eliminates static destruction order fiasco
  - Simple implementation (no complex lifetime management)
  - Handles can safely destroy themselves at any time
- **Cons:**
  - Memory leak detected by leak sanitizers (requires suppression)
  - Broker destructor never runs (cleanup logic unreachable)
  - ~16KB memory not returned to OS (negligible at program exit)

---

## 8. Overflow Protection

### Problem Statement

When reserving space for a message, the total slot size includes the message size, a length prefix (4 bytes), and alignment padding. Arithmetic overflow could occur when calculating the total size.

### Alternatives Considered

1. **No overflow checking (assume valid input)**
   - Pros: Fastest path, minimal code
   - Cons: Undefined behavior on overflow, potential security vulnerability

2. **Checked arithmetic with exception on overflow**
   - Pros: Explicit error handling
   - Cons: Exceptions in hot path violate design constraints

3. **Pre-flight bounds checking**
   - Pros: Safe, no exceptions, minimal overhead
   - Cons: Requires careful constant calculation

### Decision Made

**Pre-flight bounds checking.**

### Rationale

Reserve() must prevent overflow in slot size calculation before doing arithmetic. By checking that the requested size is within safe limits (`SIZE_MAX - 12`), we guarantee that subsequent calculations cannot overflow.

### Implementation

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

### Trade-offs

- **Pros:**
  - Memory-safe (no overflow possible)
  - No exceptions (fail-fast with `nullopt`)
  - Single comparison (minimal overhead)
- **Cons:**
  - Maximum message size slightly reduced (by 12 bytes)
  - Requires documentation of actual maximum size

---

## 9. Destruction Barrier

### Problem Statement

When a handle is destroyed, its destructor sets the liveness flag to false and notifies the peer. However, operations may still be in flight when the destructor runs, leading to race conditions.

### Alternatives Considered

1. **No synchronization (immediate flag set)**
   - Pros: Fast destruction
   - Cons: Race condition if operation in flight on another core

2. **Mutex lock during destruction**
   - Pros: Guaranteed safety
   - Cons: Violates lock-free design, deadlock risk if operation holds "virtual lock"

3. **Memory fence before flag set**
   - Pros: Ensures all prior stores visible before signaling death
   - Cons: Requires `seq_cst` fence (expensive, but only on destruction)

### Decision Made

**Memory fence before flag set.**

### Rationale

Handle destructor uses `seq_cst` fence to ensure all prior operations complete and become visible before setting the liveness flag to false. This guarantees that the peer sees a consistent view of the queue state.

### Implementation

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

### Trade-offs

- **Pros:**
  - Guarantees memory consistency during destruction
  - No deadlock risk (fence is non-blocking)
  - Correct lock-free implementation
- **Cons:**
  - `seq_cst` fence is expensive (~100 cycles on x86)
  - Acceptable trade-off: destruction is cold path, happens once per handle lifetime

---

## Future Considerations

### Thundering Herd (MPSC/MPMC Expansion)

**Note:** Current SPSC design uses `atomic::wait()` efficiently (single consumer). If expanding to MPSC/MPMC in v2.0, beware of thundering herd where multiple threads wake simultaneously but only one acquires data. Current v1.0 implementation is optimal for SPSC.

### Lock-Free Registry

**Current:** Single `shared_mutex` protects all channels.  
**Trade-off:** Simplicity vs. scalability.

**Performance:**
- 10 channels: No contention
- 100 channels: Minor contention (acceptable)
- 1000+ channels: Significant contention (consider lock striping in v1.1)

**Decision:** Accept limitation for v1.0. If >100 channels needed, implement sharded registry in v1.1.

---

## Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-02-10 | Copilot | Initial documentation from specification sections 13-14 |

---

**END OF DOCUMENT**
