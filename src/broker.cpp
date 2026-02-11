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

} // namespace omni
