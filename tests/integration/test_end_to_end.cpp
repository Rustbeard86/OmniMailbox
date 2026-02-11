// tests/integration/test_end_to_end.cpp
#include <gtest/gtest.h>
#include <omni/mailbox.hpp>
#include <thread>
#include <vector>
#include <string>
#include <atomic>

TEST(IntegrationTest, ProducerConsumerRoundTrip) {
    auto& broker = omni::MailboxBroker::Instance();
    
    auto [error, channel] = broker.RequestChannel("test-e2e", {
        .capacity = 128,
        .max_message_size = 512
    });
    ASSERT_EQ(error, omni::ChannelError::Success);
    ASSERT_TRUE(channel.has_value());
    
    const size_t num_messages = 10000;
    std::atomic<size_t> received_count{0};
    
    // Consumer thread
    std::thread consumer_thread([&]() {
        auto& consumer = channel->consumer;
        
        while (received_count.load() < num_messages) {
            auto [result, msg] = consumer.BlockingPop(std::chrono::seconds(5));
            
            if (result == omni::PopResult::Success) {
                received_count.fetch_add(1);
            } else if (result == omni::PopResult::ChannelClosed) {
                break;
            }
        }
    });
    
    // Producer thread
    std::thread producer_thread([&]() {
        auto& producer = channel->producer;
        
        for (size_t i = 0; i < num_messages; ++i) {
            std::string payload = "Message " + std::to_string(i);
            std::span<const uint8_t> data(
                reinterpret_cast<const uint8_t*>(payload.data()),
                payload.size()
            );
            
            auto result = producer.BlockingPush(data, std::chrono::seconds(5));
            ASSERT_EQ(result, omni::PushResult::Success);
        }
    });
    
    producer_thread.join();
    consumer_thread.join();
    
    EXPECT_EQ(received_count.load(), num_messages);
    
    // Cleanup: Destroy handles and remove channel from registry
    channel.reset();
    broker.RemoveChannel("test-e2e");
}

TEST(IntegrationTest, MultipleChannels) {
    auto& broker = omni::MailboxBroker::Instance();
    
    // Record baseline channel count (may include channels from other tests)
    const size_t baseline_channels = broker.GetStats().active_channels;
    
    const size_t num_channels = 100;
    std::vector<std::thread> threads;
    
    for (size_t i = 0; i < num_channels; ++i) {
        threads.emplace_back([&broker, i]() {
            std::string name = "channel-" + std::to_string(i);
            auto [error, channel] = broker.RequestChannel(name);
            ASSERT_EQ(error, omni::ChannelError::Success);
            ASSERT_TRUE(channel.has_value());
            
            // Send 1000 messages
            for (size_t j = 0; j < 1000; ++j) {
                std::string payload = std::to_string(j);
                std::span<const uint8_t> data(
                    reinterpret_cast<const uint8_t*>(payload.data()),
                    payload.size()
                );
                auto result = channel->producer.TryPush(data);
                (void)result;  // Ignore result - testing channel creation, not message delivery
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto stats = broker.GetStats();
    // Verify we created exactly num_channels NEW channels
    EXPECT_EQ(stats.active_channels, baseline_channels + num_channels);
    
    // Cleanup: Remove all test channels
    for (size_t i = 0; i < num_channels; ++i) {
        std::string name = "channel-" + std::to_string(i);
        broker.RemoveChannel(name);
    }
}
