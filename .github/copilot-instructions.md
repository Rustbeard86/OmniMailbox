# OmniMailbox - Copilot Instructions

This is a C++23 lock-free SPSC messaging library. See `OmniMailbox_Design_Specification.md` for full details.

## Workflow Guidelines

**FOLLOW THESE RULES STRICTLY:**

1. **Do NOT Work Ahead**: Implement ONLY what is explicitly requested in the current prompt. Do not add "nice-to-have" features, optimizations, or improvements unless specifically asked.

2. **No Unicode Characters**: Use only ASCII characters in all code, comments, and documentation. Never use Unicode symbols (×, →, ✓, ❌, etc.) in source files.

3. **Always Refer to Spec**: When making ANY implementation decision, consult `OmniMailbox_Design_Specification.md` first. If the spec is unclear, ask before proceeding. Do not make assumptions.

4. **Minimal Changes**: Make the smallest possible change to satisfy the requirement. Avoid refactoring unrelated code.

## Critical Implementation Rules

### Memory Ordering (Non-Negotiable)
```cpp
// Producer pattern
const uint64_t write = write_index_.load(std::memory_order_relaxed);  // Own index
const uint64_t read = read_index_.load(std::memory_order_acquire);     // Remote index
// ... write data ...
write_index_.store(write + 1, std::memory_order_release);               // Publish
write_index_.notify_one();

// Consumer pattern  
const uint64_t read = read_index_.load(std::memory_order_relaxed);     // Own index
const uint64_t write = write_index_.load(std::memory_order_acquire);   // Remote index
// ... read data ...
read_index_.store(read + 1, std::memory_order_release);                 // Publish
```

**Rule:** `relaxed` for own atomics, `acquire` for remote reads, `release` for publishing. Never `seq_cst` in hot path.

**Exception:** Destruction barrier requires `seq_cst` fence (see below).

### Cache-Line Alignment (Non-Negotiable)
```cpp
#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 64;
#endif

struct SPSCQueue {
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> write_index{0};
    char padding1[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];
    
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> read_index{0};
    char padding2[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];
    
    // Liveness flags (separate cache line to avoid false sharing)
    alignas(CACHE_LINE_SIZE) std::atomic<bool> producer_alive{true};
    alignas(CACHE_LINE_SIZE) std::atomic<bool> consumer_alive{true};
    // ...
};
```

### Hot Path Constraints
**NEVER in Push/Pop/Reserve/Commit:**
- Dynamic allocation (new/delete/malloc)
- Exceptions
- Mutex locks
- Virtual calls

**Error handling:** Return `std::optional`, enums (`PushResult`, `PopResult`, `ChannelError`), or `std::pair<Result, Data>`.

## Naming Conventions

```cpp
namespace omni { }           // Public API
namespace omni::detail { }   // Internal

class ProducerHandle { };        // PascalCase classes
bool TryPush(...) noexcept;      // PascalCase public methods
void update_stats_() noexcept;   // snake_case private methods

std::atomic<uint64_t> write_index_;  // snake_case_ members
constexpr size_t CACHE_LINE_SIZE;    // UPPER_SNAKE constants
```

## Key Patterns

### RAII Handles (Move-Only)
```cpp
class ProducerHandle {
public:
    ProducerHandle(ProducerHandle&&) noexcept = default;
    ProducerHandle(const ProducerHandle&) = delete;  // Enforce SPSC
    
    ~ProducerHandle() noexcept {
        if (queue_) {
            // CRITICAL: Destruction barrier (seq_cst fence before signaling death)
            std::atomic_thread_fence(std::memory_order_seq_cst);
            queue_->producer_alive.store(false, std::memory_order_release);
            queue_->write_index.notify_one();  // Wake blocked consumer
        }
    }

private:
    friend class MailboxBroker;
    explicit ProducerHandle(std::shared_ptr<SPSCQueue> queue)
        : queue_(std::move(queue)) {
        queue_->producer_alive.store(true, std::memory_order_release);
    }
    
    std::shared_ptr<SPSCQueue> queue_;
};
```

**Lifetime Tracking:** Queue's `producer_alive`/`consumer_alive` flags serve dual purpose: (1) peer disconnect detection, (2) broker cleanup detection. No separate tokens needed.

**Move-After-Move:** Moved-from handles are valid but inactive (safe to destroy, all methods return errors).

### Ring Buffer Algorithm
```cpp
// Full check: Leave 1 slot empty to distinguish full/empty
const uint64_t mask = capacity_ - 1;  // capacity must be power-of-2
if (((write + 1) & mask) == (read & mask)) {
    return std::nullopt;  // Full
}

// Empty check
if ((read & mask) == (write & mask)) {
    return std::nullopt;  // Empty
}
```

### FlatBuffers Zero-Copy
```cpp
// Producer
auto res = producer.Reserve(256);
flatbuffers::FlatBufferBuilder fbb(res->capacity, res->data, false);
// ... build message ...
producer.Commit(fbb.GetSize());

// Consumer
auto [result, msg] = consumer.TryPop();
auto fb = msg->GetFlatBuffer<MyMessage>();  // Zero-copy access
```

### Error Reporting
```cpp
// Broker returns error code + optional result
auto [error, channel] = broker.RequestChannel("name", config);
switch (error) {
    case ChannelError::Success:
        // Use channel.value()
        break;
    case ChannelError::NameExists:
        // Handle duplicate name
        break;
    case ChannelError::InvalidConfig:
        // Config validation failed
        break;
    case ChannelError::AllocationFailed:
        // Out of memory
        break;
}

// Operations return result enums
auto result = producer.TryPush(data);
if (result == PushResult::Success) { /* ... */ }
```

### Batch Operations (High-Throughput)
```cpp
// BatchPush amortizes atomic overhead (10-100x improvement)
std::vector<std::span<const uint8_t>> messages;
// ... fill messages ...

size_t sent = producer.BatchPush(messages);
// Single notify for entire batch vs N notifies for N individual pushes
```

## Critical Safety Rules

### Singleton Lifetime
```cpp
// Intentional memory leak to avoid destruction order fiasco
MailboxBroker& MailboxBroker::Instance() noexcept {
    static MailboxBroker* instance = new MailboxBroker();
    return *instance;
}
```
**WARNING:** Never create global/static handles. They must be destroyed before program exit.

### Overflow Protection
```cpp
std::optional<ReserveResult> Reserve(size_t bytes) noexcept {
    constexpr size_t MAX_SAFE_SIZE = SIZE_MAX - 12;
    if (bytes > MAX_SAFE_SIZE || bytes > queue_->max_message_size) {
        return std::nullopt;  // Prevent overflow
    }
    // ...
}
```

### Normalize() Order
```cpp
ChannelConfig Normalize() const noexcept {
    // CRITICAL: Clamp BEFORE rounding to power-of-2
    normalized.capacity = std::clamp(capacity, size_t(8), size_t(524'288));
    
    // Then round up to power-of-2
    if ((normalized.capacity & (normalized.capacity - 1)) != 0) {
        normalized.capacity = RoundUpPowerOf2(normalized.capacity);
    }
    return normalized;
}
```

## Testing Requirements

- **Sanitizers:** All code must pass ASAN, TSAN, UBSAN, Valgrind
- **Coverage:** ≥90% line coverage
- **Performance:** ≥5M msg/sec (64-byte), p99 latency <500ns

## Code Style

- Use `[[nodiscard]]` for all query methods
- Use `noexcept` everywhere (except broker setup)
- Document memory ordering with inline comments
- Add `OMNI_ASSERT()` for preconditions in debug builds
- Prefer `constexpr` over `#define`

See full design spec for architecture, API contracts, and detailed algorithms.


## Testing Requirements

- **Sanitizers:** All code must pass ASAN, TSAN, UBSAN, Valgrind
- **Coverage:** ≥90% line coverage
- **Performance:** ≥5M msg/sec (64-byte), p99 latency <500ns

## Code Style

- Use `[[nodiscard]]` for all query methods
- Use `noexcept` everywhere (except broker setup)
- Document memory ordering with inline comments
- Add `NUSTREAM_ASSERT()` for preconditions in debug builds
- Prefer `constexpr` over `#define`

See full design spec for architecture, API contracts, and detailed algorithms.
