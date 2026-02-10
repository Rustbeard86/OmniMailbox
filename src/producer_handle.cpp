#include "omni/producer_handle.hpp"
#include "omni/detail/spsc_queue.hpp"
#include <atomic>
#include <optional>
#include <limits>
#include <chrono>

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

#ifndef NDEBUG
// Test-only factory method (debug builds only)
ProducerHandle ProducerHandle::CreateForTesting_(std::shared_ptr<detail::SPSCQueue> queue) {
    return ProducerHandle(std::move(queue));
}

// Test helper: Get internal queue (for testing only)
std::shared_ptr<detail::SPSCQueue> ProducerHandle::GetQueueForTesting_() const noexcept {
    return pimpl_ ? pimpl_->queue_ : nullptr;
}
#endif

// ReserveResult destructor (RAII cleanup)
ProducerHandle::ReserveResult::~ReserveResult() {
    // Auto-rollback is handled by producer's Rollback()
    // This destructor exists to satisfy the declaration
    // The actual rollback logic is in ProducerHandle::Rollback()
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

bool ProducerHandle::Commit(size_t actual_bytes) noexcept {
    // 1. Validate preconditions
    if (actual_bytes == 0) {
        return false;  // actual_bytes must be > 0
    }
    
    if (actual_bytes > pimpl_->queue_->max_message_size) {
        return false;  // actual_bytes exceeds max_message_size
    }
    
    if (!pimpl_->reserved_slot_.has_value()) {
        return false;  // No active reservation
    }
    
    // 2. Write size prefix to slot
    const size_t slot_index = pimpl_->reserved_slot_.value();
    uint8_t* slot = pimpl_->queue_->buffer.get() + (slot_index * pimpl_->queue_->slot_size);
    
    // Write 4-byte size prefix (little-endian)
    const uint32_t size_prefix = static_cast<uint32_t>(actual_bytes);
    std::memcpy(slot, &size_prefix, 4);
    
    // 3. Load write_index (relaxed - own index)
    const uint64_t write = pimpl_->queue_->write_index.load(std::memory_order_relaxed);
    
    // 4. Store write_index + 1 (release) - ensures size + payload writes visible
    // Release fence ensures size + payload writes visible
    pimpl_->queue_->write_index.store(write + 1, std::memory_order_release);
    
    // 5. Call notify_one() on write_index
    pimpl_->queue_->write_index.notify_one();
    
    // 6. Update statistics (relaxed)
    pimpl_->messages_sent_.fetch_add(1, std::memory_order_relaxed);
    pimpl_->bytes_sent_.fetch_add(actual_bytes, std::memory_order_relaxed);
    
    // 7. Clear reserved_slot
    pimpl_->reserved_slot_.reset();
    
    return true;
}

void ProducerHandle::Rollback() noexcept {
    // Clear reserved_slot without advancing write_index
    pimpl_->reserved_slot_.reset();
}

PushResult ProducerHandle::BlockingPush(
    std::span<const uint8_t> data,
    std::chrono::milliseconds timeout) noexcept 
{
    // 1. Validate preconditions
    if (data.empty()) {
        pimpl_->failed_pushes_.fetch_add(1, std::memory_order_relaxed);
        return PushResult::InvalidSize;
    }
    
    if (data.size() > pimpl_->queue_->max_message_size) {
        pimpl_->failed_pushes_.fetch_add(1, std::memory_order_relaxed);
        return PushResult::InvalidSize;
    }
    
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    
    while (true) {
        // 2. Check if consumer is alive
        if (!pimpl_->queue_->consumer_alive.load(std::memory_order_relaxed)) {
            pimpl_->failed_pushes_.fetch_add(1, std::memory_order_relaxed);
            return PushResult::ChannelClosed;
        }
        
        // 3. Try Reserve
        auto result = Reserve(data.size());
        if (result.has_value()) {
            // 4. Copy data into reserved space
            std::memcpy(result->data, data.data(), data.size());
            
            // 5. Commit the message
            bool committed = Commit(data.size());
            if (!committed) {
                // This should never happen if Reserve succeeded
                pimpl_->failed_pushes_.fetch_add(1, std::memory_order_relaxed);
                return PushResult::QueueFull;
            }
            
            return PushResult::Success;
        }
        
        // 6. Check timeout
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            pimpl_->failed_pushes_.fetch_add(1, std::memory_order_relaxed);
            return PushResult::Timeout;
        }
        
        // 7. Wait for notification (with spurious wakeup protection)
        const uint64_t current_read = pimpl_->queue_->read_index.load(std::memory_order_acquire);
        const uint64_t current_write = pimpl_->queue_->write_index.load(std::memory_order_relaxed);
        
        const uint64_t mask = pimpl_->queue_->capacity - 1;
        if (((current_write + 1) & mask) != (current_read & mask)) {
            continue;  // Space became available, retry
        }
        
        // Wait until read_index changes (consumer pops a message)
        pimpl_->queue_->read_index.wait(current_read, std::memory_order_acquire);
    }
}

PushResult ProducerHandle::TryPush(std::span<const uint8_t> data) noexcept {
    // 1. Validate preconditions
    if (data.empty()) {
        return PushResult::InvalidSize;
    }
    
    if (data.size() > pimpl_->queue_->max_message_size) {
        return PushResult::InvalidSize;
    }
    
    // 2. Check if consumer is alive
    if (!pimpl_->queue_->consumer_alive.load(std::memory_order_relaxed)) {
        pimpl_->failed_pushes_.fetch_add(1, std::memory_order_relaxed);
        return PushResult::ChannelClosed;
    }
    
    // 3. Reserve space
    auto result = Reserve(data.size());
    if (!result.has_value()) {
        pimpl_->failed_pushes_.fetch_add(1, std::memory_order_relaxed);
        return PushResult::QueueFull;
    }
    
    // 4. Copy data into reserved space
    std::memcpy(result->data, data.data(), data.size());
    
    // 5. Commit the message
    bool committed = Commit(data.size());
    if (!committed) {
        // This should never happen if Reserve succeeded
        pimpl_->failed_pushes_.fetch_add(1, std::memory_order_relaxed);
        return PushResult::QueueFull;
    }
    
    return PushResult::Success;
}

bool ProducerHandle::IsConnected() const noexcept {
    // Check consumer_alive flag (relaxed read)
    return pimpl_->queue_->consumer_alive.load(std::memory_order_relaxed);
}

size_t ProducerHandle::Capacity() const noexcept {
    return pimpl_->queue_->capacity;
}

size_t ProducerHandle::MaxMessageSize() const noexcept {
    return pimpl_->queue_->max_message_size;
}

size_t ProducerHandle::AvailableSlots() const noexcept {
    // Approximate calculation using indices (relaxed reads)
    const uint64_t write = pimpl_->queue_->write_index.load(std::memory_order_relaxed);
    const uint64_t read = pimpl_->queue_->read_index.load(std::memory_order_relaxed);
    const uint64_t mask = pimpl_->queue_->capacity - 1;
    
    // Calculate used slots
    const uint64_t used = (write - read) & mask;
    
    // Available = capacity - used - 1 (leave 1 slot empty to distinguish full/empty)
    return pimpl_->queue_->capacity - used - 1;
}

ChannelConfig ProducerHandle::GetConfig() const noexcept {
    return ChannelConfig{
        .capacity = pimpl_->queue_->capacity,
        .max_message_size = pimpl_->queue_->max_message_size
    };
}

ProducerHandle::Stats ProducerHandle::GetStats() const noexcept {
    return Stats{
        .messages_sent = pimpl_->messages_sent_.load(std::memory_order_relaxed),
        .bytes_sent = pimpl_->bytes_sent_.load(std::memory_order_relaxed),
        .failed_pushes = pimpl_->failed_pushes_.load(std::memory_order_relaxed)
    };
}

ProducerHandle::~ProducerHandle() noexcept {
    if (pimpl_ && pimpl_->queue_) {
        // CRITICAL: Destruction barrier (seq_cst fence before signaling death)
        // Ensures all previous writes are visible before setting producer_alive to false
        std::atomic_thread_fence(std::memory_order_seq_cst);
        
        // Signal producer is dead (release semantics)
        pimpl_->queue_->producer_alive.store(false, std::memory_order_release);
        
        // Wake blocked consumer
        pimpl_->queue_->write_index.notify_one();
    }
}

ProducerHandle::ProducerHandle(ProducerHandle&&) noexcept = default;
ProducerHandle& ProducerHandle::operator=(ProducerHandle&&) noexcept = default;

} // namespace omni
