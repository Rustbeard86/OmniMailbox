#include <gtest/gtest.h>
#include <omni/detail/spsc_queue.hpp>
#include <string>
#include <cstring>

TEST(SPSCQueueTest, InitialState) {
    omni::detail::SPSCQueue queue(16, 256);
    
    EXPECT_EQ(queue.capacity, 16);
    EXPECT_EQ(queue.max_message_size, 256);
    EXPECT_EQ(queue.write_index.load(), 0);
    EXPECT_EQ(queue.read_index.load(), 0);
    EXPECT_TRUE(queue.producer_alive.load());
    EXPECT_TRUE(queue.consumer_alive.load());
}

TEST(SPSCQueueTest, SingleMessage) {
    omni::detail::SPSCQueue queue(16, 256);
    
    // Producer writes
    const std::string payload = "Hello, World!";
    const uint64_t write_idx = 0;
    uint8_t* slot = queue.buffer.get();
    
    *reinterpret_cast<uint32_t*>(slot) = payload.size();
    std::memcpy(slot + 4, payload.data(), payload.size());
    queue.write_index.store(1, std::memory_order_release);
    
    // Consumer reads
    const uint64_t read_idx = queue.read_index.load(std::memory_order_relaxed);
    const uint64_t write_idx_read = queue.write_index.load(std::memory_order_acquire);
    
    ASSERT_NE(read_idx, write_idx_read);  // Data available
    
    uint8_t* read_slot = queue.buffer.get();
    const uint32_t size = *reinterpret_cast<uint32_t*>(read_slot);
    std::string received(reinterpret_cast<char*>(read_slot + 4), size);
    
    EXPECT_EQ(received, payload);
    queue.read_index.store(1, std::memory_order_release);
}

TEST(SPSCQueueTest, QueueFull) {
    omni::detail::SPSCQueue queue(4, 64);  // Capacity 4 = 3 usable slots
    
    // Fill queue (capacity - 1 messages)
    for (size_t i = 0; i < 3; ++i) {
        const uint64_t write = queue.write_index.load(std::memory_order_relaxed);
        const uint64_t read = queue.read_index.load(std::memory_order_acquire);
        const uint64_t mask = queue.capacity - 1;
        
        ASSERT_NE(((write + 1) & mask), (read & mask));  // Not full
        
        uint8_t* slot = queue.buffer.get() + (write & mask) * queue.slot_size;
        *reinterpret_cast<uint32_t*>(slot) = 10;
        queue.write_index.store(write + 1, std::memory_order_release);
    }
    
    // Next write should detect full queue
    const uint64_t write = queue.write_index.load(std::memory_order_relaxed);
    const uint64_t read = queue.read_index.load(std::memory_order_acquire);
    const uint64_t mask = queue.capacity - 1;
    
    EXPECT_EQ(((write + 1) & mask), (read & mask));  // Full!
}

TEST(SPSCQueueTest, PowerOfTwoCapacity) {
    // Valid power-of-2 capacities should construct successfully
    EXPECT_NO_THROW({
        omni::detail::SPSCQueue queue1(2, 64);
        omni::detail::SPSCQueue queue2(4, 64);
        omni::detail::SPSCQueue queue3(16, 64);
        omni::detail::SPSCQueue queue4(1024, 64);
    });
    
    // Non-power-of-2 capacities should trigger assertion
    // Note: In debug builds, this will assert. In release, behavior is undefined.
    // We document the requirement rather than testing assertion failure.
}
