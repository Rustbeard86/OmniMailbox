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

} // namespace omni
