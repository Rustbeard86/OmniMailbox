#include "omni/mailbox_broker.hpp"
#include "omni/detail/spsc_queue.hpp"
#include <shared_mutex>
#include <unordered_map>
#include <chrono>
#include <atomic>

namespace omni {

struct MailboxBroker::Impl {
    struct ChannelState {
        std::shared_ptr<detail::SPSCQueue> queue;
        std::string name;
        std::chrono::steady_clock::time_point created_at;
    };

    mutable std::shared_mutex registry_mutex_;
    std::unordered_map<std::string, ChannelState> channels_;
    std::atomic<size_t> total_created_{0};
    std::atomic<size_t> total_destroyed_{0};
};

// Constructor - Initialize pimpl
MailboxBroker::MailboxBroker()
    : pimpl_(std::make_unique<Impl>())
{
}

// Destructor - Required for unique_ptr<Impl> with forward-declared Impl
MailboxBroker::~MailboxBroker() = default;

// Singleton instance implementation
MailboxBroker& MailboxBroker::Instance() noexcept {
    // INTENTIONAL MEMORY LEAK - Static Destruction Order Fiasco Prevention
    // 
    // The broker is never destroyed to avoid undefined behavior during program shutdown.
    // 
    // Problem:
    //   - Handles (ProducerHandle/ConsumerHandle) may outlive the broker singleton
    //     during static destruction if they're stored in global/static variables
    //   - C++ doesn't guarantee destruction order across translation units
    //   - Handle destructors access queue state (producer_alive/consumer_alive flags)
    //   - If broker destroyed first, handles access freed memory → crash
    // 
    // Solution:
    //   - Never call broker destructor by using 'new' without matching 'delete'
    //   - Ensures handles can always safely access queue state during shutdown
    //   - Cost: ~16KB leaked at program exit (acceptable for singleton pattern)
    // 
    // C++11 guarantees thread-safe initialization of local static variables,
    // so this pattern is both safe and standard.
    // 
    // Reference: Design Specification Section 14.1 "Singleton Lifetime"
    static MailboxBroker* instance = new MailboxBroker();
    return *instance;
}

std::pair<ChannelError, std::optional<ChannelPair>> MailboxBroker::RequestChannel(
    std::string_view name,
    const ChannelConfig& config) noexcept
{
    // 1. Validate config using IsValid() - check user-provided config first
    // Note: IsValid() requires power-of-2 capacity, so we normalize first if needed
    ChannelConfig normalized = config.Normalize();
    
    // After normalization, validate the normalized config
    if (!normalized.IsValid()) {
        return {ChannelError::InvalidConfig, std::nullopt};
    }
    
    // 3. Acquire write lock (unique_lock for exclusive access)
    std::unique_lock lock(pimpl_->registry_mutex_);
    
    // 4. Check if channel exists (return NameExists if found)
    if (pimpl_->channels_.contains(std::string(name))) {
        return {ChannelError::NameExists, std::nullopt};
    }
    
    // 5. Try to create queue (catch bad_alloc, return AllocationFailed)
    try {
        auto queue = std::make_shared<detail::SPSCQueue>(
            normalized.capacity,
            normalized.max_message_size
        );
        
        // 6. Store ChannelState in map
        Impl::ChannelState state{
            .queue = queue,
            .name = std::string(name),
            .created_at = std::chrono::steady_clock::now()
        };
        
        pimpl_->channels_.emplace(state.name, std::move(state));
        
        // 7. Increment total_created counter (relaxed ordering for stats)
        pimpl_->total_created_.fetch_add(1, std::memory_order_relaxed);
        
        // 8. Create ProducerHandle and ConsumerHandle
        // Both handles reference the same queue
        ProducerHandle producer(queue);
        ConsumerHandle consumer(queue);
        
        // 9. Return {Success, ChannelPair{producer, consumer}}
        return {ChannelError::Success, ChannelPair{std::move(producer), std::move(consumer)}};
        
    } catch (const std::bad_alloc&) {
        // Memory allocation failed
        return {ChannelError::AllocationFailed, std::nullopt};
    }
}

} // namespace omni
