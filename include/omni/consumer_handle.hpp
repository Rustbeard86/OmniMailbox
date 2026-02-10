#ifndef OMNI_CONSUMER_HANDLE_HPP
#define OMNI_CONSUMER_HANDLE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <chrono>
#include <vector>
#include "omni/detail/config.hpp"

namespace omni {

// Forward declarations
class MailboxBroker;

namespace detail {
    struct SPSCQueue;
}

class ConsumerHandle {
public:
    // Zero-copy message view
    class Message {
    public:
        // Raw data access
        [[nodiscard]] std::span<const uint8_t> Data() const noexcept;
        
        // FlatBuffers convenience accessor
        // PRECONDITION: Message contains valid FlatBuffer of type T
        // RECOMMENDATION: Call Verify<T>() in debug builds
        template<typename T>
        [[nodiscard]] const T* GetFlatBuffer() const noexcept;
        
        // FlatBuffers integrity check
        // EXPENSIVE: Use in debug/testing only
        template<typename T>
        [[nodiscard]] bool Verify() const noexcept;
        
        // LIFETIME: Valid until next Pop() or ~ConsumerHandle()
        ~Message() = default;
        
    private:
        friend class ConsumerHandle;
        explicit Message(std::span<const uint8_t> data);
        std::span<const uint8_t> data_;
    };
    
    // Statistics (relaxed atomics)
    struct Stats {
        uint64_t messages_received;
        uint64_t bytes_received;
        uint64_t failed_pops;  // Timeouts + ChannelClosed
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
    
    // Get channel configuration
    // Returns the normalized configuration used to create the channel.
    // Identical to ProducerHandle::GetConfig() - both handles share same queue.
    [[nodiscard]] ChannelConfig GetConfig() const noexcept;
    
    // Get statistics (relaxed atomics)
    [[nodiscard]] Stats GetStats() const noexcept;
    
    // RAII: Destructor signals producer (sets consumer_alive = false)
    ~ConsumerHandle() noexcept;
    
    // Move-only
    ConsumerHandle(ConsumerHandle&&) noexcept;
    ConsumerHandle& operator=(ConsumerHandle&&) noexcept;
    
    // Non-copyable (enforces SPSC)
    ConsumerHandle(const ConsumerHandle&) = delete;
    ConsumerHandle& operator=(const ConsumerHandle&) = delete;

#ifndef NDEBUG
    // ===== TEST-ONLY METHODS (Debug builds only) =====
    // WARNING: These methods expose internal state for testing.
    // Do NOT use in production code.
    
    // Create handle for testing (bypasses broker)
    [[nodiscard]] static ConsumerHandle CreateForTesting_(std::shared_ptr<detail::SPSCQueue> queue);
    
    // Get internal queue (for unit testing only)
    [[nodiscard]] std::shared_ptr<detail::SPSCQueue> GetQueueForTesting_() const noexcept;
#endif

private:
    friend class MailboxBroker;
    explicit ConsumerHandle(std::shared_ptr<detail::SPSCQueue> queue);
    
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace omni

#endif // OMNI_CONSUMER_HANDLE_HPP
