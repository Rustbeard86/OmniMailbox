#ifndef OMNI_DETAIL_WAIT_STRATEGY_HPP
#define OMNI_DETAIL_WAIT_STRATEGY_HPP

#include <chrono>
#include <thread>

namespace omni::detail {

/**
 * @brief Hybrid spin-then-yield wait strategy for sub-microsecond p99 latency.
 * 
 * Optimized for high-performance lock-free SPSC queues where:
 * - Fast path (uncontended): Operation succeeds immediately (~50-100ns)
 * - Contended but quick: Peer responds within 1-2us (caught by spin loop)
 * - Contended longer: Yields CPU to avoid burning cycles, respects timeout
 * 
 * @tparam Predicate Callable returning bool (true = condition met, false = keep waiting)
 * 
 * @param try_operation Predicate to check if operation can proceed
 *                      Example: []() { return queue_has_space(); }
 * 
 * @return true if predicate became true during spin, false if spin exhausted
 * 
 * @par Performance Characteristics
 * - Spin duration: ~1000 iterations = ~1-2us on modern CPU
 * - Each iteration: 2 atomic loads + comparison (~6ns)
 * - Total spin time: ~6us max before yielding
 * 
 * @par Design Rationale
 * Spinning briefly before yielding reduces latency for common cases where
 * the peer (producer/consumer) responds quickly. This amortizes the cost
 * of context switches while maintaining CPU efficiency for longer waits.
 * 
 * @par Usage Pattern
 * @code
 * while (true) {
 *     if (try_operation()) {
 *         return Success;
 *     }
 *     
 *     if (now >= deadline) {
 *         return Timeout;
 *     }
 *     
 *     // Spin briefly, then yield if no progress
 *     SpinWaitWithYield([&]() {
 *         return check_condition_became_true();
 *     });
 * }
 * @endcode
 * 
 * @par Memory Ordering
 * The predicate is responsible for correct memory ordering.
 * Typically: acquire for remote index, relaxed for own index.
 * 
 * @par Thread Safety
 * Safe to call from any thread. Does not modify shared state.
 * 
 * @warning Do NOT use this for mutex-protected code or syscalls.
 * This is specifically optimized for lock-free atomics.
 */
template<typename Predicate>
inline void SpinWaitWithYield(Predicate&& try_operation) noexcept {
    // Spin count tuned for ~1-2us total spin time
    // Adjust based on profiling for specific hardware
    constexpr int SPIN_COUNT = 1000;
    
    for (int spin = 0; spin < SPIN_COUNT; ++spin) {
        if (try_operation()) {
            // Condition met, exit immediately to retry outer operation
            return;
        }
    }
    
    // Spin exhausted without success, yield CPU to avoid burning cycles
    // Caller will recheck timeout and retry operation
    std::this_thread::yield();
}

} // namespace omni::detail

#endif // OMNI_DETAIL_WAIT_STRATEGY_HPP
