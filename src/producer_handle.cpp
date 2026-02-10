#include "omni/producer_handle.hpp"
#include "omni/detail/spsc_queue.hpp"
#include <atomic>
#include <optional>

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

} // namespace omni
