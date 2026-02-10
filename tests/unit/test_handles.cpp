#include <gtest/gtest.h>
#include "omni/producer_handle.hpp"
#include "omni/detail/spsc_queue.hpp"
#include <memory>
#include <limits>

// Test fixture with friend access to ProducerHandle
class ProducerHandleTestFixture : public ::testing::Test {
protected:
    // Helper to create a ProducerHandle for testing
    static omni::ProducerHandle CreateTestProducer(size_t capacity = 16, size_t max_msg_size = 256) {
        auto queue = std::make_shared<omni::detail::SPSCQueue>(capacity, max_msg_size);
        // Call private constructor - this requires friend access
        return omni::ProducerHandle(queue);
    }
};

// Test basic Reserve functionality
TEST_F(ProducerHandleTestFixture, ReserveBasic) {
    auto producer = CreateTestProducer(16, 256);
    
    // Reserve should succeed with valid parameters
    auto result = producer.Reserve(128);
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->data, nullptr);
    EXPECT_EQ(result->capacity, 256);  // Should return max_message_size as capacity
}

// Test overflow protection
TEST_F(ProducerHandleTestFixture, ReserveOverflowProtection) {
    auto producer = CreateTestProducer(16, 256);
    
    // Test MAX_SAFE_SIZE boundary (SIZE_MAX - 12)
    constexpr size_t MAX_SAFE_SIZE = std::numeric_limits<size_t>::max() - 12;
    
    // Requesting MAX_SAFE_SIZE + 1 should fail
    auto result = producer.Reserve(MAX_SAFE_SIZE + 1);
    EXPECT_FALSE(result.has_value());
    
    // Requesting SIZE_MAX should fail
    auto result2 = producer.Reserve(std::numeric_limits<size_t>::max());
    EXPECT_FALSE(result2.has_value());
}

// Test zero byte reservation
TEST_F(ProducerHandleTestFixture, ReserveZeroBytes) {
    auto producer = CreateTestProducer(16, 256);
    
    // Zero bytes should fail
    auto result = producer.Reserve(0);
    EXPECT_FALSE(result.has_value());
}

// Test exceeding max_message_size
TEST_F(ProducerHandleTestFixture, ReserveExceedsMaxSize) {
    auto producer = CreateTestProducer(16, 256);
    
    // Requesting more than max_message_size should fail
    auto result = producer.Reserve(257);
    EXPECT_FALSE(result.has_value());
    
    // Requesting exactly max_message_size should succeed
    auto result2 = producer.Reserve(256);
    EXPECT_TRUE(result2.has_value());
}

// Test queue full detection
TEST_F(ProducerHandleTestFixture, ReserveQueueFull) {
    auto producer = CreateTestProducer(4, 64);  // Capacity 4 = 3 usable slots
    
    // Reserve 3 messages (fill the queue)
    auto r1 = producer.Reserve(32);
    EXPECT_TRUE(r1.has_value());
    
    auto r2 = producer.Reserve(32);
    EXPECT_TRUE(r2.has_value());
    
    auto r3 = producer.Reserve(32);
    EXPECT_TRUE(r3.has_value());
    
    // Fourth reservation should fail (queue full)
    auto r4 = producer.Reserve(32);
    EXPECT_FALSE(r4.has_value());
}

// Test consumer_alive check
TEST_F(ProducerHandleTestFixture, ReserveConsumerDead) {
    auto queue = std::make_shared<omni::detail::SPSCQueue>(16, 256);
    auto producer = omni::ProducerHandle(queue);
    
    // Mark consumer as dead
    queue->consumer_alive.store(false, std::memory_order_release);
    
    // Reserve should fail
    auto result = producer.Reserve(128);
    EXPECT_FALSE(result.has_value());
}

// Test multiple reservations without commit
TEST_F(ProducerHandleTestFixture, ReserveMultipleWithoutCommit) {
    auto producer = CreateTestProducer(16, 256);
    
    // First reservation should succeed
    auto r1 = producer.Reserve(128);
    EXPECT_TRUE(r1.has_value());
    
    // Second reservation without committing first should fail
    auto r2 = producer.Reserve(64);
    EXPECT_FALSE(r2.has_value());
}

// Test full Reserve->Commit cycle
TEST_F(ProducerHandleTestFixture, ReserveCommit) {
    auto queue = std::make_shared<omni::detail::SPSCQueue>(16, 256);
    auto producer = omni::ProducerHandle(queue);
    
    // Reserve space
    auto result = producer.Reserve(128);
    ASSERT_TRUE(result.has_value());
    
    // Write some test data to the reserved space
    uint8_t* data = result->data;
    for (size_t i = 0; i < 64; ++i) {
        data[i] = static_cast<uint8_t>(i);
    }
    
    // Commit with 64 bytes (less than reserved 128)
    bool committed = producer.Commit(64);
    EXPECT_TRUE(committed);
    
    // Verify write_index was advanced
    uint64_t write_index = queue->write_index.load(std::memory_order_acquire);
    EXPECT_EQ(write_index, 1);
    
    // Verify size prefix was written correctly
    uint8_t* slot = queue->buffer.get();
    uint32_t size_prefix = 0;
    std::memcpy(&size_prefix, slot, 4);
    EXPECT_EQ(size_prefix, 64);
    
    // Verify payload data
    for (size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(slot[4 + i], static_cast<uint8_t>(i));
    }
    
    // Should be able to reserve again after commit
    auto result2 = producer.Reserve(64);
    EXPECT_TRUE(result2.has_value());
}

// Test Commit with zero bytes (should fail)
TEST_F(ProducerHandleTestFixture, CommitZeroBytes) {
    auto producer = CreateTestProducer(16, 256);
    
    // Reserve space
    auto result = producer.Reserve(128);
    ASSERT_TRUE(result.has_value());
    
    // Commit with 0 bytes should fail
    bool committed = producer.Commit(0);
    EXPECT_FALSE(committed);
}

// Test Commit exceeding max_message_size (should fail)
TEST_F(ProducerHandleTestFixture, CommitExceedsMaxSize) {
    auto producer = CreateTestProducer(16, 256);
    
    // Reserve space
    auto result = producer.Reserve(128);
    ASSERT_TRUE(result.has_value());
    
    // Commit with more than max_message_size should fail
    bool committed = producer.Commit(257);
    EXPECT_FALSE(committed);
}

// Test Commit without Reserve (should fail)
TEST_F(ProducerHandleTestFixture, CommitWithoutReserve) {
    auto producer = CreateTestProducer(16, 256);
    
    // Commit without reserving should fail
    bool committed = producer.Commit(64);
    EXPECT_FALSE(committed);
}

// Test Commit updates statistics
TEST_F(ProducerHandleTestFixture, CommitUpdatesStats) {
    auto producer = CreateTestProducer(16, 256);
    
    // Initial stats should be zero
    auto stats = producer.GetStats();
    EXPECT_EQ(stats.messages_sent, 0);
    EXPECT_EQ(stats.bytes_sent, 0);
    
    // Reserve and commit
    auto result = producer.Reserve(128);
    ASSERT_TRUE(result.has_value());
    producer.Commit(64);
    
    // Check updated stats
    stats = producer.GetStats();
    EXPECT_EQ(stats.messages_sent, 1);
    EXPECT_EQ(stats.bytes_sent, 64);
    
    // Reserve and commit again
    auto result2 = producer.Reserve(128);
    ASSERT_TRUE(result2.has_value());
    producer.Commit(32);
    
    // Check cumulative stats
    stats = producer.GetStats();
    EXPECT_EQ(stats.messages_sent, 2);
    EXPECT_EQ(stats.bytes_sent, 96);
}

// Placeholder for handle tests
TEST(HandlesTest, Placeholder) {
    EXPECT_TRUE(true);
}
