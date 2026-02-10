#include <omni/detail/spsc_queue.hpp>
#include <omni/detail/config.hpp>
#include <gtest/gtest.h>

using namespace omni::detail;

TEST(SPSCQueueCompileTest, BasicConstruction) {
    // Test: SPSCQueue can be constructed with valid parameters
    SPSCQueue queue(1024, 4096);
    
    EXPECT_EQ(queue.capacity, 1024);
    EXPECT_EQ(queue.max_message_size, 4096);
    EXPECT_NE(queue.buffer.get(), nullptr);
}

TEST(SPSCQueueCompileTest, InitialState) {
    // Test: Queue starts with correct initial state
    SPSCQueue queue(256, 1024);
    
    EXPECT_EQ(queue.write_index.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(queue.read_index.load(std::memory_order_relaxed), 0);
    EXPECT_TRUE(queue.producer_alive.load(std::memory_order_relaxed));
    EXPECT_TRUE(queue.consumer_alive.load(std::memory_order_relaxed));
}

TEST(SPSCQueueCompileTest, CacheLineAlignment) {
    // Test: Verify cache line alignment
    SPSCQueue queue(64, 512);
    
    // Check that atomic indices are at expected offsets
    uintptr_t write_addr = reinterpret_cast<uintptr_t>(&queue.write_index);
    uintptr_t read_addr = reinterpret_cast<uintptr_t>(&queue.read_index);
    uintptr_t producer_alive_addr = reinterpret_cast<uintptr_t>(&queue.producer_alive);
    uintptr_t consumer_alive_addr = reinterpret_cast<uintptr_t>(&queue.consumer_alive);
    
    // Write index should be cache-line aligned
    EXPECT_EQ(write_addr % CACHE_LINE_SIZE, 0);
    
    // Read index should be on separate cache line
    EXPECT_EQ(read_addr % CACHE_LINE_SIZE, 0);
    EXPECT_GE(read_addr - write_addr, CACHE_LINE_SIZE);
    
    // Liveness flags should be on separate cache lines
    EXPECT_EQ(producer_alive_addr % CACHE_LINE_SIZE, 0);
    EXPECT_EQ(consumer_alive_addr % CACHE_LINE_SIZE, 0);
}

TEST(SPSCQueueCompileTest, SlotSizeAlignment) {
    // Test: Slot size is properly aligned to 8 bytes
    SPSCQueue queue1(128, 100);
    EXPECT_EQ(queue1.slot_size % 8, 0);
    
    SPSCQueue queue2(128, 123);
    EXPECT_EQ(queue2.slot_size % 8, 0);
    
    // Slot size should be at least 4 (size prefix) + max_message_size
    EXPECT_GE(queue1.slot_size, 4 + 100);
    EXPECT_GE(queue2.slot_size, 4 + 123);
}

TEST(SPSCQueueCompileTest, BufferAllocation) {
    // Test: Buffer is properly allocated
    SPSCQueue queue(512, 2048);
    
    size_t expected_buffer_size = queue.capacity * queue.slot_size;
    EXPECT_NE(queue.buffer.get(), nullptr);
    
    // Verify buffer is zero-initialized (spot check first few bytes)
    for (size_t i = 0; i < std::min(expected_buffer_size, size_t(64)); ++i) {
        EXPECT_EQ(queue.buffer[i], 0);
    }
}
