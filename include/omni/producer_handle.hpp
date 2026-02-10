#ifndef OMNI_PRODUCER_HANDLE_HPP
#define OMNI_PRODUCER_HANDLE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <chrono>
#include "omni/detail/config.hpp"

namespace omni {

// Forward declarations
class MailboxBroker;

namespace detail {
    struct SPSCQueue;
}

class ProducerHandle {
public:
    // Zero-copy reserve for FlatBuffers
    struct ReserveResult {
        uint8_t* data;       // Pointer to write region
        size_t capacity;     // Available bytes (>= requested)
        
        // RAII: Auto-rollback if Commit() not called
        ~ReserveResult();
    };
    
    // Statistics (relaxed atomics)
    struct Stats {
        uint64_t messages_sent;
        uint64_t bytes_sent;
        uint64_t failed_pushes;  // Timeouts + ChannelClosed
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
    
    // Batch push multiple messages (amortizes atomic overhead)
    // Attempts to push all messages in the span. Stops at first failure
    // (queue full or consumer disconnected) and returns number of successfully
    // pushed messages.
    // RETURNS: Number of messages successfully pushed [0, messages.size()]
    [[nodiscard]] size_t BatchPush(
        std::span<const std::span<const uint8_t>> messages
    ) noexcept;
    
    // Query state (relaxed reads, approximate)
    [[nodiscard]] bool IsConnected() const noexcept;  // Consumer alive
    [[nodiscard]] size_t Capacity() const noexcept;
    [[nodiscard]] size_t MaxMessageSize() const noexcept;
    [[nodiscard]] size_t AvailableSlots() const noexcept;  // Approx free space
    
    // Get channel configuration
    // Returns the normalized configuration used to create the channel.
    // Values may differ from the config passed to RequestChannel() due
    // to normalization (rounding capacity to power of 2, clamping ranges).
    [[nodiscard]] ChannelConfig GetConfig() const noexcept;
    
    // Get statistics (relaxed atomics)
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
friend class ProducerHandleTestFixture;  // For testing
explicit ProducerHandle(std::shared_ptr<detail::SPSCQueue> queue);
    
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace omni

#endif // OMNI_PRODUCER_HANDLE_HPP
