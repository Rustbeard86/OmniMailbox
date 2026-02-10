#include "omni/consumer_handle.hpp"
#include "omni/detail/spsc_queue.hpp"
#include "omni/detail/queue_helpers.hpp"
#include <atomic>
#include <optional>
#include <vector>

namespace omni {

// Internal implementation structure
struct ConsumerHandle::Impl {
    // Queue reference
    std::shared_ptr<detail::SPSCQueue> queue;
    
    // Statistics (relaxed atomics)
    Stats statistics;
    
    // Message buffer for zero-copy span lifetime
    std::vector<uint8_t> message_buffer;
    
    // Constructor: Initialize with queue and signal consumer alive
    explicit Impl(std::shared_ptr<detail::SPSCQueue> q)
        : queue(std::move(q))
        , statistics{0, 0, 0}
        , message_buffer()
    {
        // Signal consumer is alive (release semantics for visibility)
        queue->consumer_alive.store(true, std::memory_order_release);
    }
};

// Message implementation
ConsumerHandle::Message::Message(std::span<const uint8_t> data)
    : data_(data)
{
}

std::span<const uint8_t> ConsumerHandle::Message::Data() const noexcept {
    return data_;
}

// Constructor
ConsumerHandle::ConsumerHandle(std::shared_ptr<detail::SPSCQueue> queue)
    : pimpl_(std::make_unique<Impl>(std::move(queue)))
{
}

#ifndef NDEBUG
// Test-only factory method (debug builds only)
ConsumerHandle ConsumerHandle::CreateForTesting_(std::shared_ptr<detail::SPSCQueue> queue) {
    return ConsumerHandle(std::move(queue));
}

// Test helper: Get internal queue (for testing only)
std::shared_ptr<detail::SPSCQueue> ConsumerHandle::GetQueueForTesting_() const noexcept {
    return pimpl_ ? pimpl_->queue : nullptr;
}
#endif

std::pair<PopResult, std::optional<ConsumerHandle::Message>> ConsumerHandle::TryPop() noexcept {
    // 1. Check producer_alive flag (relaxed read)
    // If producer is dead, we still drain remaining messages
    const bool producer_alive = pimpl_->queue->producer_alive.load(std::memory_order_relaxed);
    
    // 2. Load read_index (relaxed - own index) and write_index (acquire - remote index)
    const uint64_t read = pimpl_->queue->read_index.load(std::memory_order_relaxed);
    const uint64_t write = pimpl_->queue->write_index.load(std::memory_order_acquire);  // Sync with producer
    
    // 3. Check if data available using utility function
    if (detail::IsQueueEmpty(read, write, pimpl_->queue->capacity)) {
        // If producer is dead and queue is empty, channel is closed
        if (!producer_alive) {
            pimpl_->statistics.failed_pops++;
            return {PopResult::ChannelClosed, std::nullopt};
        }
        // Otherwise, just empty
        return {PopResult::Empty, std::nullopt};
    }
    
    // 4. Calculate slot pointer using utility function
    uint8_t* slot = detail::GetSlotPointer(
        pimpl_->queue->buffer.get(),
        read,
        pimpl_->queue->capacity,
        pimpl_->queue->slot_size);
    
    // 5. Read size prefix using utility function
    const size_t message_size = detail::ReadSizePrefix(slot);
    
    // 6. Get payload pointer using utility function
    const uint8_t* payload = detail::GetPayloadPointer(slot);
    
    // 7. Create span to payload (zero-copy view into ring buffer)
    std::span<const uint8_t> message_span(payload, message_size);
    
    // 8. Store read_index + 1 (release) - ensures consumer has finished reading
    pimpl_->queue->read_index.store(read + 1, std::memory_order_release);
    
    // 9. Call notify_one() on read_index to wake blocked producer
    pimpl_->queue->read_index.notify_one();
    
    // 10. Update statistics (relaxed)
    pimpl_->statistics.messages_received++;
    pimpl_->statistics.bytes_received += message_size;
    
    // 11. Return success with message view
    return {PopResult::Success, Message{message_span}};
}

bool ConsumerHandle::IsConnected() const noexcept {
    return pimpl_->queue->producer_alive.load(std::memory_order_relaxed);
}

size_t ConsumerHandle::Capacity() const noexcept {
    return pimpl_->queue->capacity;
}

size_t ConsumerHandle::MaxMessageSize() const noexcept {
    return pimpl_->queue->max_message_size;
}

size_t ConsumerHandle::AvailableMessages() const noexcept {
    const uint64_t read = pimpl_->queue->read_index.load(std::memory_order_relaxed);
    const uint64_t write = pimpl_->queue->write_index.load(std::memory_order_relaxed);
    return detail::AvailableMessages(read, write, pimpl_->queue->capacity);
}

ChannelConfig ConsumerHandle::GetConfig() const noexcept {
    return ChannelConfig{
        .capacity = pimpl_->queue->capacity,
        .max_message_size = pimpl_->queue->max_message_size
    };
}

ConsumerHandle::Stats ConsumerHandle::GetStats() const noexcept {
    return pimpl_->statistics;
}

ConsumerHandle::~ConsumerHandle() noexcept {
    if (pimpl_ && pimpl_->queue) {
        // CRITICAL: Destruction barrier (seq_cst fence before signaling death)
        std::atomic_thread_fence(std::memory_order_seq_cst);
        pimpl_->queue->consumer_alive.store(false, std::memory_order_release);
        pimpl_->queue->read_index.notify_one();  // Wake blocked producer
    }
}

ConsumerHandle::ConsumerHandle(ConsumerHandle&&) noexcept = default;
ConsumerHandle& ConsumerHandle::operator=(ConsumerHandle&&) noexcept = default;

std::pair<PopResult, std::optional<ConsumerHandle::Message>> ConsumerHandle::BlockingPop(
    std::chrono::milliseconds timeout) noexcept {
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
            pimpl_->statistics.failed_pops++;
            return {PopResult::Timeout, std::nullopt};
        }
        
        // 3. Wait for notification (with spurious wakeup protection)
        const uint64_t current_write = pimpl_->queue->write_index.load(std::memory_order_acquire);
        const uint64_t current_read = pimpl_->queue->read_index.load(std::memory_order_relaxed);
        
        if (current_write != current_read) {
            continue;  // Data arrived, retry
        }
        
        // Wait until write_index changes OR timeout
        pimpl_->queue->write_index.wait(current_write, std::memory_order_acquire);
    }
}

std::pair<PopResult, std::vector<ConsumerHandle::Message>> ConsumerHandle::BatchPop(
    size_t /*max_count*/,
    std::chrono::milliseconds /*timeout*/) noexcept {
    // TODO: Implement BatchPop
    return {PopResult::Empty, std::vector<Message>{}};
}

} // namespace omni
