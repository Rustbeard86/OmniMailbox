#include "omni/producer_handle.hpp"
#include "omni/detail/spsc_queue.hpp"
#include <atomic>
#include <optional>
#include <limits>

namespace omni {

// Internal implementation structure
struct ProducerHandle::Impl {
    // Queue reference
    std::shared_ptr<detail::SPSCQueue> queue_;
    
    // Statistics (atomic for thread-safe relaxed reads)
    std::atomic<uint64_t> messages_sent_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> failed_pushes_{0};
    
    // Reservation tracking (nullopt = no active reservation)
    std::optional<size_t> reserved_slot_;
    
    // Constructor: Initialize with queue and signal producer alive
    explicit Impl(std::shared_ptr<detail::SPSCQueue> queue)
        : queue_(std::move(queue))
        , messages_sent_(0)
        , bytes_sent_(0)
        , failed_pushes_(0)
        , reserved_slot_(std::nullopt)
    {
        // Signal producer is alive (release semantics for visibility)
        queue_->producer_alive.store(true, std::memory_order_release);
    }
};

// Overflow protection constant
constexpr size_t MAX_SAFE_SIZE = std::numeric_limits<size_t>::max() - 12;

// Constructor
ProducerHandle::ProducerHandle(std::shared_ptr<detail::SPSCQueue> queue)
    : pimpl_(std::make_unique<Impl>(std::move(queue)))
{
}

std::optional<ProducerHandle::ReserveResult> ProducerHandle::Reserve(size_t bytes) noexcept {
    // 1. Validate preconditions
    if (bytes == 0) {
        return std::nullopt;
    }
    
    if (bytes > MAX_SAFE_SIZE) {
        return std::nullopt;  // Overflow protection
    }
    
    if (bytes > pimpl_->queue_->max_message_size) {
        return std::nullopt;
    }
    
    // Check for previous reservation not committed
    if (pimpl_->reserved_slot_.has_value()) {
        return std::nullopt;
    }
    
    // 2. Check consumer_alive flag (relaxed read)
    if (!pimpl_->queue_->consumer_alive.load(std::memory_order_relaxed)) {
        return std::nullopt;  // Consumer died
    }
    
    // 3. Load write_index (relaxed - own index) and read_index (acquire - remote index)
    const uint64_t write = pimpl_->queue_->write_index.load(std::memory_order_relaxed);
    const uint64_t read = pimpl_->queue_->read_index.load(std::memory_order_acquire);  // Sync with consumer
    
    // 4. Check for queue full using mask
    const uint64_t mask = pimpl_->queue_->capacity - 1;
    if (((write + 1) & mask) == (read & mask)) {
        return std::nullopt;  // Queue full (leave 1 slot empty to distinguish full/empty)
    }
    
    // 5. Calculate slot pointer
    const size_t slot_index = write & mask;
    uint8_t* slot = pimpl_->queue_->buffer.get() + (slot_index * pimpl_->queue_->slot_size);
    
    // 6. Store reserved_slot in Impl
    pimpl_->reserved_slot_ = slot_index;
    
    // 7. Return ReserveResult with pointer to payload (skip 4-byte size prefix)
    return ReserveResult{
        .data = slot + 4,
        .capacity = pimpl_->queue_->max_message_size
    };
}

} // namespace omni
