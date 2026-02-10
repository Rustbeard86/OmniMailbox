#ifndef OMNI_DETAIL_QUEUE_HELPERS_HPP
#define OMNI_DETAIL_QUEUE_HELPERS_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>

namespace omni::detail {

/**
 * @brief Inline utility functions for SPSC queue operations.
 * 
 * These functions encapsulate common ring buffer operations:
 * - Queue full/empty detection
 * - Slot index calculation
 * - Size validation
 * - Size prefix read/write
 * - Available space/data counting
 * 
 * All functions are `inline` or `inline constexpr` for zero overhead in hot paths.
 * 
 * @par Design Rationale
 * Extracting these calculations into utilities:
 * 1. Eliminates duplication across Reserve/Commit/BatchPush/BlockingPush
 * 2. Centralizes the "leave 1 slot empty" logic for full/empty distinction
 * 3. Makes the code self-documenting (function names explain intent)
 * 4. Simplifies testing (can unit test these separately)
 * 
 * @par Performance
 * All functions compile to 2-5 CPU instructions with optimization enabled.
 * The `inline` keyword ensures no function call overhead.
 */

// ============================================================================
// Constants
// ============================================================================

/**
 * @brief Maximum safe message size to prevent overflow in calculations.
 * 
 * Accounts for:
 * - 4-byte size prefix
 * - 8-byte alignment padding
 * 
 * SIZE_MAX - 12 ensures slot_size calculations never overflow.
 */
constexpr size_t MAX_SAFE_MESSAGE_SIZE = std::numeric_limits<size_t>::max() - 12;

/**
 * @brief Size of message size prefix in bytes.
 */
constexpr size_t SIZE_PREFIX_BYTES = 4;

// ============================================================================
// Validation Utilities
// ============================================================================

/**
 * @brief Validate message size for queue operations.
 * 
 * Checks:
 * 1. Size is non-zero
 * 2. Size doesn't exceed max_message_size
 * 3. Size doesn't cause overflow (< MAX_SAFE_MESSAGE_SIZE)
 * 
 * @param size Message size in bytes
 * @param max_message_size Maximum allowed message size for this queue
 * @return true if size is valid, false otherwise
 * 
 * @par Example
 * @code
 * if (!IsValidMessageSize(data.size(), queue->max_message_size)) {
 *     return PushResult::InvalidSize;
 * }
 * @endcode
 */
[[nodiscard]] inline constexpr bool IsValidMessageSize(
    size_t size,
    size_t max_message_size) noexcept
{
    return size > 0 
        && size <= max_message_size 
        && size <= MAX_SAFE_MESSAGE_SIZE;
}

// ============================================================================
// Queue State Utilities
// ============================================================================

/**
 * @brief Check if ring buffer is full (producer perspective).
 * 
 * Uses the "leave 1 slot empty" strategy to distinguish full from empty.
 * If (write + 1) equals read (modulo capacity), the queue is full.
 * 
 * @param write_index Current write position (producer-owned)
 * @param read_index Current read position (consumer-owned, acquire-loaded)
 * @param capacity Queue capacity (must be power of 2)
 * @return true if queue is full, false if space available
 * 
 * @par Memory Ordering
 * Caller must ensure read_index loaded with memory_order_acquire.
 * 
 * @par Example
 * @code
 * const uint64_t write = write_index_.load(std::memory_order_relaxed);
 * const uint64_t read = read_index_.load(std::memory_order_acquire);
 * if (IsQueueFull(write, read, capacity)) {
 *     return PushResult::QueueFull;
 * }
 * @endcode
 */
[[nodiscard]] inline constexpr bool IsQueueFull(
    uint64_t write_index,
    uint64_t read_index,
    size_t capacity) noexcept
{
    const uint64_t mask = capacity - 1;
    return ((write_index + 1) & mask) == (read_index & mask);
}

/**
 * @brief Check if ring buffer is empty (consumer perspective).
 * 
 * If write equals read (modulo capacity), no data is available.
 * 
 * @param read_index Current read position (consumer-owned)
 * @param write_index Current write position (producer-owned, acquire-loaded)
 * @param capacity Queue capacity (must be power of 2)
 * @return true if queue is empty, false if data available
 * 
 * @par Memory Ordering
 * Caller must ensure write_index loaded with memory_order_acquire.
 * 
 * @par Example
 * @code
 * const uint64_t read = read_index_.load(std::memory_order_relaxed);
 * const uint64_t write = write_index_.load(std::memory_order_acquire);
 * if (IsQueueEmpty(read, write, capacity)) {
 *     return PopResult::Empty;
 * }
 * @endcode
 */
[[nodiscard]] inline constexpr bool IsQueueEmpty(
    uint64_t read_index,
    uint64_t write_index,
    size_t capacity) noexcept
{
    const uint64_t mask = capacity - 1;
    return (read_index & mask) == (write_index & mask);
}

// ============================================================================
// Index Calculation Utilities
// ============================================================================

/**
 * @brief Calculate slot index from ring buffer index.
 * 
 * Wraps the index using bitwise AND with (capacity - 1), which is
 * equivalent to modulo for power-of-2 capacities but much faster.
 * 
 * @param index Raw index (monotonically increasing)
 * @param capacity Queue capacity (must be power of 2)
 * @return Slot index in range [0, capacity)
 * 
 * @par Performance
 * Bitwise AND is ~20x faster than modulo on most CPUs.
 * For capacity=1024: `index & 1023` vs `index % 1024`
 * 
 * @par Example
 * @code
 * const uint64_t write = write_index_.load(std::memory_order_relaxed);
 * const size_t slot_idx = GetSlotIndex(write, capacity);
 * uint8_t* slot = buffer.get() + (slot_idx * slot_size);
 * @endcode
 */
[[nodiscard]] inline constexpr size_t GetSlotIndex(
    uint64_t index,
    size_t capacity) noexcept
{
    return static_cast<size_t>(index & (capacity - 1));
}

/**
 * @brief Calculate slot pointer from buffer base and index.
 * 
 * Combines GetSlotIndex with pointer arithmetic for common pattern.
 * 
 * @param buffer Base pointer to ring buffer
 * @param index Current index (write or read)
 * @param capacity Queue capacity (must be power of 2)
 * @param slot_size Size of each slot in bytes
 * @return Pointer to slot at given index
 * 
 * @par Example
 * @code
 * uint8_t* slot = GetSlotPointer(buffer.get(), write, capacity, slot_size);
 * // Equivalent to:
 * // uint8_t* slot = buffer.get() + (GetSlotIndex(write, capacity) * slot_size);
 * @endcode
 */
[[nodiscard]] inline uint8_t* GetSlotPointer(
    uint8_t* buffer,
    uint64_t index,
    size_t capacity,
    size_t slot_size) noexcept
{
    const size_t slot_index = GetSlotIndex(index, capacity);
    return buffer + (slot_index * slot_size);
}

/**
 * @brief Calculate number of available slots (producer perspective).
 * 
 * Returns free space in queue, accounting for the "leave 1 slot empty" rule.
 * 
 * @param write_index Current write position (producer-owned)
 * @param read_index Current read position (consumer-owned, acquire-loaded)
 * @param capacity Queue capacity (must be power of 2)
 * @return Number of free slots available for writing
 * 
 * @par Note
 * Result is approximate if read_index not loaded with acquire ordering.
 * For exact count, ensure read_index loaded with memory_order_acquire.
 * 
 * @par Example
 * @code
 * const uint64_t write = write_index_.load(std::memory_order_relaxed);
 * const uint64_t read = read_index_.load(std::memory_order_relaxed);
 * const size_t available = AvailableSlots(write, read, capacity);
 * @endcode
 */
[[nodiscard]] inline constexpr size_t AvailableSlots(
    uint64_t write_index,
    uint64_t read_index,
    size_t capacity) noexcept
{
    const uint64_t mask = capacity - 1;
    const uint64_t used = (write_index - read_index) & mask;
    return capacity - used - 1;  // -1 for "leave 1 slot empty" rule
}

/**
 * @brief Calculate number of available messages (consumer perspective).
 * 
 * Returns count of messages ready to be consumed.
 * 
 * @param read_index Current read position (consumer-owned)
 * @param write_index Current write position (producer-owned, acquire-loaded)
 * @param capacity Queue capacity (must be power of 2)
 * @return Number of messages available for reading
 * 
 * @par Note
 * Result is approximate if write_index not loaded with acquire ordering.
 * For exact count, ensure write_index loaded with memory_order_acquire.
 * 
 * @par Example
 * @code
 * const uint64_t read = read_index_.load(std::memory_order_relaxed);
 * const uint64_t write = write_index_.load(std::memory_order_relaxed);
 * const size_t available = AvailableMessages(read, write, capacity);
 * @endcode
 */
[[nodiscard]] inline constexpr size_t AvailableMessages(
    uint64_t read_index,
    uint64_t write_index,
    size_t capacity) noexcept
{
    const uint64_t mask = capacity - 1;
    return static_cast<size_t>((write_index - read_index) & mask);
}

// ============================================================================
// Size Prefix Utilities
// ============================================================================

/**
 * @brief Write 4-byte size prefix to slot.
 * 
 * Writes message size as little-endian uint32_t at beginning of slot.
 * 
 * @param slot Pointer to slot start
 * @param size Message size in bytes
 * 
 * @par Preconditions
 * - slot must be valid pointer with at least 4 bytes writable
 * - size must fit in uint32_t (< 4GB)
 * 
 * @par Example
 * @code
 * uint8_t* slot = GetSlotPointer(buffer.get(), write, capacity, slot_size);
 * WriteSizePrefix(slot, message_size);
 * // Payload starts at slot + 4
 * @endcode
 */
inline void WriteSizePrefix(uint8_t* slot, size_t size) noexcept {
    const uint32_t size_prefix = static_cast<uint32_t>(size);
    std::memcpy(slot, &size_prefix, SIZE_PREFIX_BYTES);
}

/**
 * @brief Read 4-byte size prefix from slot.
 * 
 * Reads message size as little-endian uint32_t from beginning of slot.
 * 
 * @param slot Pointer to slot start
 * @return Message size in bytes
 * 
 * @par Preconditions
 * - slot must be valid pointer with at least 4 bytes readable
 * - Slot must contain valid size prefix written by WriteSizePrefix
 * 
 * @par Example
 * @code
 * uint8_t* slot = GetSlotPointer(buffer.get(), read, capacity, slot_size);
 * const size_t message_size = ReadSizePrefix(slot);
 * // Payload starts at slot + 4
 * @endcode
 */
[[nodiscard]] inline size_t ReadSizePrefix(const uint8_t* slot) noexcept {
    uint32_t size_prefix = 0;
    std::memcpy(&size_prefix, slot, SIZE_PREFIX_BYTES);
    return static_cast<size_t>(size_prefix);
}

/**
 * @brief Get pointer to payload after size prefix.
 * 
 * Returns pointer to message data, skipping 4-byte size prefix.
 * 
 * @param slot Pointer to slot start
 * @return Pointer to payload (slot + 4)
 * 
 * @par Example
 * @code
 * uint8_t* slot = GetSlotPointer(buffer.get(), write, capacity, slot_size);
 * uint8_t* payload = GetPayloadPointer(slot);
 * std::memcpy(payload, message_data, message_size);
 * @endcode
 */
[[nodiscard]] inline uint8_t* GetPayloadPointer(uint8_t* slot) noexcept {
    return slot + SIZE_PREFIX_BYTES;
}

/**
 * @brief Get const pointer to payload after size prefix.
 * 
 * Const overload for read operations.
 * 
 * @param slot Pointer to slot start
 * @return Const pointer to payload (slot + 4)
 */
[[nodiscard]] inline const uint8_t* GetPayloadPointer(const uint8_t* slot) noexcept {
    return slot + SIZE_PREFIX_BYTES;
}

} // namespace omni::detail

#endif // OMNI_DETAIL_QUEUE_HELPERS_HPP

