#ifndef OMNI_DETAIL_SPSC_QUEUE_HPP
#define OMNI_DETAIL_SPSC_QUEUE_HPP

#include <atomic>
#include <memory>
#include <cstring>
#include <cassert>

namespace omni::detail {

// Cache line size detection
#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 64;
#endif

// Suppress MSVC warning about intentional padding for cache-line alignment
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324)  // Structure was padded due to alignment specifier
#endif

struct SPSCQueue {
    // Producer-owned cache line (relaxed for own index, acquire for remote)
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> write_index{0};
    char padding1[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];
    
    // Consumer-owned cache line (relaxed for own index, acquire for remote)
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> read_index{0};
    char padding2[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];
    
    // Liveness tracking (separate cache line to avoid false sharing with indices)
    alignas(CACHE_LINE_SIZE) std::atomic<bool> producer_alive{true};
    alignas(CACHE_LINE_SIZE) std::atomic<bool> consumer_alive{true};
    
    // Configuration (immutable after construction)
    const size_t capacity;          // Must be power of 2
    const size_t max_message_size;
    const size_t slot_size;         // 4 (size prefix) + max_message_size + alignment
    
    // Buffer storage
    std::unique_ptr<uint8_t[]> buffer;
    
    // Constructor
    SPSCQueue(size_t cap, size_t max_msg_size)
        : capacity(cap)
        , max_message_size(max_msg_size)
        , slot_size(AlignUp(4 + max_msg_size, 8))
        , buffer(new uint8_t[capacity * slot_size])
    {
        assert((capacity & (capacity - 1)) == 0);  // Power of 2
        std::memset(buffer.get(), 0, capacity * slot_size);
    }
    
private:
    static constexpr size_t AlignUp(size_t val, size_t align) {
        return (val + align - 1) & ~(align - 1);
    }
};

} // namespace omni::detail

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // OMNI_DETAIL_SPSC_QUEUE_HPP
