#ifndef OMNI_DETAIL_CONFIG_HPP
#define OMNI_DETAIL_CONFIG_HPP

#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace omni {

// Push operation result codes
enum class PushResult {
    Success,
    Timeout,
    ChannelClosed,
    InvalidSize,
    QueueFull
};

// Pop operation result codes
enum class PopResult {
    Success,
    Timeout,
    ChannelClosed,
    Empty
};

// Channel creation error codes
enum class ChannelError {
    Success,
    NameExists,
    InvalidConfig,
    AllocationFailed
};

// Channel configuration parameters
struct ChannelConfig {
    size_t capacity = 1024;             // Ring buffer capacity (will be rounded to power-of-2)
    size_t max_message_size = 4096;     // Maximum message size in bytes
    
    // Normalize configuration to valid values
    [[nodiscard]] ChannelConfig Normalize() const noexcept {
        ChannelConfig normalized = *this;
        
        // CRITICAL: Clamp BEFORE rounding to power-of-2
        normalized.capacity = std::clamp(capacity, size_t(8), size_t(524'288));
        normalized.max_message_size = std::clamp(max_message_size, size_t(64), size_t(1'048'576));
        
        // Then round up to power-of-2
        if ((normalized.capacity & (normalized.capacity - 1)) != 0) {
            normalized.capacity = RoundUpPowerOf2(normalized.capacity);
        }
        
        return normalized;
    }
    
    // Validate configuration
    [[nodiscard]] bool IsValid() const noexcept {
        // Capacity must be in valid range and power-of-2
        if (capacity < 8 || capacity > 524'288) {
            return false;
        }
        if ((capacity & (capacity - 1)) != 0) {
            return false;
        }
        
        // Message size must be in valid range
        if (max_message_size < 64 || max_message_size > 1'048'576) {
            return false;
        }
        
        return true;
    }
    
private:
    // Round up to next power of 2
    static constexpr size_t RoundUpPowerOf2(size_t value) noexcept {
        if (value == 0) return 1;
        
        value--;
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        value |= value >> 32;
        value++;
        
        return value;
    }
};

} // namespace omni

#endif // OMNI_DETAIL_CONFIG_HPP
