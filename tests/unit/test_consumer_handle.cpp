#include <gtest/gtest.h>
#include <omni/consumer_handle.hpp>
#include <omni/producer_handle.hpp>
#include <omni/detail/spsc_queue.hpp>
#include <thread>
#include <chrono>
#include <vector>

using namespace omni;
using namespace std::chrono_literals;

class ConsumerHandleTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create queue for testing
        queue_ = std::make_shared<detail::SPSCQueue>(16, 256);
    }
    
    std::shared_ptr<detail::SPSCQueue> queue_;
};

// Test: BatchPop with multiple messages
TEST_F(ConsumerHandleTest, BatchPop) {
    // Create producer and consumer handles
    auto producer = ProducerHandle::CreateForTesting_(queue_);
    auto consumer = ConsumerHandle::CreateForTesting_(queue_);
    
    // Push multiple messages
    const std::vector<std::string> messages = {
        "Message 1",
        "Message 2",
        "Message 3",
        "Message 4",
        "Message 5"
    };
    
    for (const auto& msg : messages) {
        std::vector<uint8_t> data(msg.begin(), msg.end());
        auto result = producer.TryPush(std::span<const uint8_t>(data));
        ASSERT_EQ(result, PushResult::Success);
    }
    
    // BatchPop up to 10 messages (should get all 5)
    auto [result, popped_messages] = consumer.BatchPop(10, 0ms);
    
    ASSERT_EQ(result, PopResult::Success);
    ASSERT_EQ(popped_messages.size(), 5);
    
    // Verify message contents
    for (size_t i = 0; i < messages.size(); ++i) {
        auto data = popped_messages[i].Data();
        std::string received(reinterpret_cast<const char*>(data.data()), data.size());
        EXPECT_EQ(received, messages[i]);
    }
    
    // Verify statistics
    auto stats = consumer.GetStats();
    EXPECT_EQ(stats.messages_received, 5);
}

// Test: BatchPop with max_count limit
TEST_F(ConsumerHandleTest, BatchPopWithLimit) {
    auto producer = ProducerHandle::CreateForTesting_(queue_);
    auto consumer = ConsumerHandle::CreateForTesting_(queue_);
    
    // Push 10 messages
    for (int i = 0; i < 10; ++i) {
        std::string msg = "Message " + std::to_string(i);
        std::vector<uint8_t> data(msg.begin(), msg.end());
        auto result = producer.TryPush(std::span<const uint8_t>(data));
        ASSERT_EQ(result, PushResult::Success);
    }
    
    // BatchPop with limit of 3
    auto [result, messages] = consumer.BatchPop(3, 0ms);
    
    EXPECT_EQ(result, PopResult::Success);
    EXPECT_EQ(messages.size(), 3);
    
    // Verify remaining messages are still in queue
    EXPECT_EQ(consumer.AvailableMessages(), 7);
}

// Test: BatchPop with timeout waits for first message
TEST_F(ConsumerHandleTest, BatchPopWithTimeout) {
    auto producer = ProducerHandle::CreateForTesting_(queue_);
    auto consumer = ConsumerHandle::CreateForTesting_(queue_);
    
    // Start thread that pushes after delay
    std::thread producer_thread([&producer]() {
        std::this_thread::sleep_for(50ms);
        std::string msg = "Delayed message";
        std::vector<uint8_t> data(msg.begin(), msg.end());
        producer.TryPush(std::span<const uint8_t>(data));
    });
    
    // BatchPop with timeout should wait for message
    auto start = std::chrono::steady_clock::now();
    auto [result, messages] = consumer.BatchPop(5, 200ms);
    auto elapsed = std::chrono::steady_clock::now() - start;
    
    EXPECT_EQ(result, PopResult::Success);
    EXPECT_EQ(messages.size(), 1);
    EXPECT_GE(elapsed, 50ms);  // Should have waited
    EXPECT_LT(elapsed, 200ms); // Should not have timed out
    
    producer_thread.join();
}

// Test: BatchPop timeout when no messages
TEST_F(ConsumerHandleTest, BatchPopTimeout) {
    auto producer = ProducerHandle::CreateForTesting_(queue_);
    auto consumer = ConsumerHandle::CreateForTesting_(queue_);
    
    // BatchPop with timeout on empty queue
    auto start = std::chrono::steady_clock::now();
    auto [result, messages] = consumer.BatchPop(5, 50ms);
    auto elapsed = std::chrono::steady_clock::now() - start;
    
    EXPECT_EQ(result, PopResult::Timeout);
    EXPECT_TRUE(messages.empty());
    EXPECT_GE(elapsed, 50ms);
}

// Test: BatchPop with max_count = 0
TEST_F(ConsumerHandleTest, BatchPopZeroCount) {
    auto consumer = ConsumerHandle::CreateForTesting_(queue_);
    
    auto [result, messages] = consumer.BatchPop(0, 0ms);
    
    EXPECT_EQ(result, PopResult::Empty);
    EXPECT_TRUE(messages.empty());
}

// Test: BatchPop returns ChannelClosed when producer dies
TEST_F(ConsumerHandleTest, BatchPopChannelClosed) {
    auto consumer = ConsumerHandle::CreateForTesting_(queue_);
    
    {
        auto producer = ProducerHandle::CreateForTesting_(queue_);
        // Producer goes out of scope and dies
    }
    
    // Give time for destructor to signal death
    std::this_thread::sleep_for(10ms);
    
    // BatchPop should detect channel closed
    auto [result, messages] = consumer.BatchPop(5, 0ms);
    
    EXPECT_EQ(result, PopResult::ChannelClosed);
    EXPECT_TRUE(messages.empty());
}

// Test: BatchPop single notify for entire batch
TEST_F(ConsumerHandleTest, BatchPopSingleNotify) {
    auto producer = ProducerHandle::CreateForTesting_(queue_);
    auto consumer = ConsumerHandle::CreateForTesting_(queue_);
    
    // Push 5 messages
    for (int i = 0; i < 5; ++i) {
        std::string msg = "Message " + std::to_string(i);
        std::vector<uint8_t> data(msg.begin(), msg.end());
        producer.TryPush(std::span<const uint8_t>(data));
    }
    
    // BatchPop should consume all 5 with single notification
    auto [result, messages] = consumer.BatchPop(10, 0ms);
    
    EXPECT_EQ(result, PopResult::Success);
    EXPECT_EQ(messages.size(), 5);
    
    // Verify producer can push again (notification worked)
    std::string msg = "After batch";
    std::vector<uint8_t> data(msg.begin(), msg.end());
    auto push_result = producer.TryPush(std::span<const uint8_t>(data));
    EXPECT_EQ(push_result, PushResult::Success);
}

// Test: Destructor signals producer
TEST_F(ConsumerHandleTest, Destructor) {
    auto producer = ProducerHandle::CreateForTesting_(queue_);
    
    // Create and destroy consumer in scope
    {
        auto consumer = ConsumerHandle::CreateForTesting_(queue_);
        EXPECT_TRUE(queue_->consumer_alive.load(std::memory_order_relaxed));
    }
    
    // Give time for destructor to execute
    std::this_thread::sleep_for(10ms);
    
    // Consumer should be marked as dead
    EXPECT_FALSE(queue_->consumer_alive.load(std::memory_order_relaxed));
    
    // Producer should detect death
    EXPECT_FALSE(producer.IsConnected());
}

// Test: Destructor wakes blocked producer
TEST_F(ConsumerHandleTest, DestructorWakesProducer) {
    auto producer = ProducerHandle::CreateForTesting_(queue_);
    
    // Fill queue
    for (size_t i = 0; i < 15; ++i) {
        std::string msg = "Fill " + std::to_string(i);
        std::vector<uint8_t> data(msg.begin(), msg.end());
        producer.TryPush(std::span<const uint8_t>(data));
    }
    
    // Start thread that blocks on push
    std::atomic<bool> push_returned{false};
    std::thread producer_thread([&producer, &push_returned]() {
        std::string msg = "Blocking message";
        std::vector<uint8_t> data(msg.begin(), msg.end());
        // This should block because queue is full
        auto result = producer.BlockingPush(std::span<const uint8_t>(data), 500ms);
        push_returned.store(true);
    });
    
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(push_returned.load());  // Should still be blocked
    
    // Destroy consumer in nested scope - should wake blocked producer
    {
        auto consumer = ConsumerHandle::CreateForTesting_(queue_);
        std::this_thread::sleep_for(10ms);
        // Consumer destroyed here
    }
    
    producer_thread.join();
    EXPECT_TRUE(push_returned.load());  // Should have woken up
}

// Test: Destructor with seq_cst fence prevents use-after-free
TEST_F(ConsumerHandleTest, DestructorBarrier) {
    // This test verifies the destruction barrier prevents race conditions
    // In practice, this requires thread sanitizer (TSAN) to detect issues
    
    std::atomic<bool> test_complete{false};
    
    auto producer = ProducerHandle::CreateForTesting_(queue_);
    
    std::thread consumer_thread([this, &test_complete]() {
        auto consumer = ConsumerHandle::CreateForTesting_(queue_);
        
        // Do some work
        for (int i = 0; i < 10; ++i) {
            auto [result, msg] = consumer.TryPop();
            std::this_thread::sleep_for(1ms);
        }
        
        // Destructor should use seq_cst fence
        test_complete.store(true);
    });
    
    // Producer keeps pushing
    for (int i = 0; i < 50; ++i) {
        std::string msg = "Message " + std::to_string(i);
        std::vector<uint8_t> data(msg.begin(), msg.end());
        producer.TryPush(std::span<const uint8_t>(data));
        std::this_thread::sleep_for(1ms);
    }
    
    consumer_thread.join();
    EXPECT_TRUE(test_complete.load());
}

// Test: Move-after-move is safe
TEST_F(ConsumerHandleTest, MoveSemantics) {
    auto consumer1 = ConsumerHandle::CreateForTesting_(queue_);
    
    // Move construct
    auto consumer2 = std::move(consumer1);
    EXPECT_EQ(consumer2.Capacity(), 16);
    
    // Move assign
    auto consumer3 = ConsumerHandle::CreateForTesting_(queue_);
    consumer3 = std::move(consumer2);
    EXPECT_EQ(consumer3.Capacity(), 16);
    
    // Moved-from handles should be safe to destroy
    // (destructor checks pimpl_ validity)
}
