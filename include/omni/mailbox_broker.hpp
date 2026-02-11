#ifndef OMNI_MAILBOX_BROKER_HPP
#define OMNI_MAILBOX_BROKER_HPP

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include "omni/detail/config.hpp"
#include "omni/producer_handle.hpp"
#include "omni/consumer_handle.hpp"

namespace omni {

/**
 * @brief Pair of producer and consumer handles for a channel.
 * 
 * Both handles are move-only and enforce SPSC semantics.
 * 
 * @par Move Semantics
 * After moving from a handle (producer or consumer), the moved-from
 * handle is in a valid but inactive state:
 * - Destructor is safe to call
 * - All methods return error (ChannelClosed / nullopt / false)
 * - Can be assigned to (restored to active state)
 * 
 * @code
 * auto channel = broker.RequestChannel("test").value();
 * 
 * // Move out producer
 * auto producer = std::move(channel.producer);
 * 
 * // Moved-from handle - safe but inactive
 * auto result = channel.producer.TryPush(data);
 * assert(result == PushResult::ChannelClosed);  // Valid behavior
 * 
 * // Can restore
 * channel.producer = std::move(producer);  // Move back
 * @endcode
 */
struct ChannelPair {
    ProducerHandle producer;
    ConsumerHandle consumer;
};

/**
 * @brief Singleton dispatcher for managing named channels.
 * 
 * Thread-safe broker that creates and manages SPSC channels identified
 * by unique string names. Provides centralized channel lifecycle management
 * with automatic cleanup via RAII handles.
 * 
 * @par Thread Safety
 * All methods are thread-safe. Uses shared_mutex internally:
 * - Read operations (HasChannel): Multiple readers allowed
 * - Write operations (RequestChannel, RemoveChannel): Exclusive access
 * 
 * @par Singleton Lifetime
 * Instance uses intentional memory leak to avoid destruction order issues.
 * See Instance() documentation for details.
 */
class MailboxBroker {
public:
    /**
     * @brief Get singleton instance (thread-safe lazy initialization).
     * 
     * Uses Meyer's singleton pattern with intentional memory leak.
     * The broker is never destroyed to avoid undefined behavior from
     * static destruction order fiasco.
     * 
     * @return Reference to global MailboxBroker instance
     * 
     * @par Thread Safety
     * C++11 guarantees thread-safe initialization of local static variables.
     * 
     * @par Lifetime
     * Singleton persists until program termination. Handles MUST be
     * destroyed before main() exits to avoid dangling references.
     * 
     * @warning GLOBAL HANDLES FORBIDDEN
     * 
     * Do NOT create global or static-storage-duration handles.
     * Handles must be destroyed before program exit to avoid
     * undefined behavior with singleton destruction order.
     * 
     * INCORRECT:
     *   static ProducerHandle g_producer = broker.RequestChannel(...)->producer;  // BAD
     * 
     * CORRECT:
     *   std::unique_ptr<ProducerHandle> g_producer;
     *   g_producer = std::make_unique<ProducerHandle>(broker.RequestChannel(...)->producer);
     *   // Explicitly reset before main() returns:
     *   g_producer.reset();  // GOOD
     * 
     * @warning NOT SIGNAL-SAFE (POSIX)
     * 
     * Do NOT call broker methods from POSIX signal handlers.
     * Use atomic flags for signaling instead.
     * 
     * INCORRECT:
     *   void signal_handler(int) {
     *       MailboxBroker::Instance().Shutdown();  // BAD - Deadlock risk
     *   }
     * 
     * CORRECT:
     *   std::atomic<bool> shutdown_requested{false};
     *   void signal_handler(int) {
     *       shutdown_requested.store(true, std::memory_order_release);  // GOOD
     *   }
     *   int main() {
     *       signal(SIGTERM, signal_handler);
     *       while (!shutdown_requested.load(std::memory_order_acquire)) {
     *           // process messages
     *       }
     *       broker.Shutdown();  // Safe in main thread
     *   }
     */
    [[nodiscard]] static MailboxBroker& Instance() noexcept;
    
    /**
     * @brief Create new channel with error reporting.
     * 
     * Atomically checks for existing channel with given name and creates
     * new channel if not found. Configuration is automatically normalized
     * before use.
     * 
     * @param name Unique channel identifier
     * @param config Channel configuration (auto-normalized)
     * @return Pair of (error code, optional channel pair)
     * 
     * @par Preconditions
     * - !name.empty()
     * 
     * @par Postconditions
     * - On success: error = Success, channel contains valid handles
     * - On failure: error indicates reason, channel is nullopt
     * 
     * @par Error Conditions
     * - NameExists: Channel with this name already registered
     * - InvalidConfig: Config invalid even after normalization
     * - AllocationFailed: Memory allocation failed
     * 
     * @par Thread Safety
     * Uses shared_mutex (write lock). Safe to call from multiple threads.
     * 
     * @par Performance
     * O(1) average, O(n) worst-case (hash collision).
     * Mutex contention possible with many concurrent requests.
     * 
     * @par Example
     * @code
     * auto [error, channel] = broker.RequestChannel("my-channel", {
     *     .capacity = 2048,
     *     .max_message_size = 4096
     * });
     * 
     * switch (error) {
     *     case ChannelError::Success:
     *         // Use channel.value()
     *         break;
     *     case ChannelError::NameExists:
     *         // Choose different name or retrieve existing
     *         break;
     *     case ChannelError::InvalidConfig:
     *         // Fix config and retry
     *         break;
     *     case ChannelError::AllocationFailed:
     *         // Free memory and retry
     *         break;
     * }
     * 
     * // Or simple check:
     * if (error == ChannelError::Success) {
     *     auto& [producer, consumer] = channel.value();
     *     // Use handles
     * }
     * @endcode
     */
    [[nodiscard]] std::pair<ChannelError, std::optional<ChannelPair>> RequestChannel(
        std::string_view name,
        const ChannelConfig& config = {}
    ) noexcept;
    
    /**
     * @brief Check if channel exists.
     * 
     * @param name Channel identifier to check
     * @return true if channel exists, false otherwise
     * 
     * @par Thread Safety
     * Uses shared_mutex (read lock). Multiple threads can call simultaneously.
     * 
     * @par Note
     * Result may be stale immediately after return if another thread
     * removes the channel concurrently.
     */
    [[nodiscard]] bool HasChannel(std::string_view name) const noexcept;
    
    /**
     * @brief Remove channel (only if no active handles).
     * 
     * Attempts to remove channel from registry. Fails if either producer
     * or consumer handle is still alive.
     * 
     * @param name Channel identifier to remove
     * @return true if removed, false if handles still alive or not found
     * 
     * @par Thread Safety
     * Uses shared_mutex (write lock).
     * 
     * @par Note
     * Channels are automatically cleaned up when both handles are destroyed.
     * Explicit removal is optional.
     */
    bool RemoveChannel(std::string_view name) noexcept;
    
    /**
     * @brief Channel and messaging statistics.
     * 
     * Approximate counts using relaxed atomics. Values may be stale
     * or inconsistent when read concurrently with channel operations.
     */
    struct Stats {
        size_t active_channels;          ///< Currently registered channels
        size_t total_channels_created;   ///< Lifetime channel count
        size_t total_messages_sent;      ///< Across all channels (approximate)
        size_t total_bytes_transferred;  ///< Across all channels (approximate)
    };
    
    /**
     * @brief Get broker statistics (approximate).
     * 
     * @return Current statistics snapshot
     * 
     * @par Thread Safety
     * Uses relaxed atomics. Values may be stale but reads are safe.
     * 
     * @par Performance
     * O(n) where n = number of active channels (iterates to sum stats).
     */
    [[nodiscard]] Stats GetStats() const noexcept;
    
    /**
     * @brief Shutdown all channels (signals stop, does NOT wait).
     * 
     * Sets all producer_alive and consumer_alive flags to false across
     * all registered channels. This causes any blocking operations to
     * return with appropriate error codes and prevents new operations
     * from succeeding.
     * 
     * @par Limitations (Section 14.5)
     * This method does NOT block waiting for handle destructors.
     * It only signals shutdown by setting liveness flags. Handles
     * may continue to exist after Shutdown() returns.
     * 
     * User must ensure handles are destroyed before calling Shutdown()
     * to avoid undefined behavior with subsequent operations.
     * 
     * @par Thread Safety
     * Uses shared_mutex (write lock).
     * 
     * @par Deadlock Warning
     * DO NOT call while holding channel handles in the same thread.
     * Ensure all handles are destroyed before calling Shutdown().
     * 
     * @par Usage Pattern
     * INCORRECT:
     * @code
     *   auto [error, channel] = broker.RequestChannel("test");
     *   broker.Shutdown();  // BAD - Handles still alive!
     *   // Undefined behavior if handles used after this point
     * @endcode
     * 
     * CORRECT:
     * @code
     *   {
     *       auto [error, channel] = broker.RequestChannel("test");
     *       // Use channel
     *   }  // channel destroyed here
     *   broker.Shutdown();  // GOOD - Safe to shutdown
     * @endcode
     * 
     * @par Implementation Note
     * Current design sets liveness flags but doesn't wait for destructors.
     * This is acceptable for v1.0 as it avoids complex synchronization.
     * Future versions may add blocking behavior if needed.
     * 
     * @see Section 14.5 "Shutdown() Limitations" in design specification
     */
    void Shutdown() noexcept;

private:
    // Private constructor (singleton pattern)
    MailboxBroker();
    
    // Private destructor (singleton pattern)
    ~MailboxBroker();
    
    // Non-copyable, non-movable
    MailboxBroker(const MailboxBroker&) = delete;
    MailboxBroker& operator=(const MailboxBroker&) = delete;
    MailboxBroker(MailboxBroker&&) = delete;
    MailboxBroker& operator=(MailboxBroker&&) = delete;
    
    // Implementation details (pimpl idiom)
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace omni

#endif // OMNI_MAILBOX_BROKER_HPP
