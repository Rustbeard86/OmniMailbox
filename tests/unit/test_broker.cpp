#include <gtest/gtest.h>
#include "omni/mailbox_broker.hpp"

// Test that Instance() returns a singleton (same reference every time)
TEST(BrokerTest, Singleton) {
    auto& broker1 = omni::MailboxBroker::Instance();
    auto& broker2 = omni::MailboxBroker::Instance();
    
    // Both references should point to the same object
    EXPECT_EQ(&broker1, &broker2);
}

// Test successful channel creation
TEST(BrokerTest, RequestChannelSuccess) {
    auto& broker = omni::MailboxBroker::Instance();
    
    // Create channel with valid config
    auto [error, channel] = broker.RequestChannel("test-channel-success", {
        .capacity = 1024,
        .max_message_size = 4096
    });
    
    // Should succeed
    ASSERT_EQ(error, omni::ChannelError::Success);
    ASSERT_TRUE(channel.has_value());
    
    // Verify both handles are valid
    EXPECT_TRUE(channel->producer.IsConnected());
    EXPECT_TRUE(channel->consumer.IsConnected());
    
    // Verify configuration was normalized correctly
    auto config = channel->producer.GetConfig();
    EXPECT_EQ(config.capacity, 1024);  // Already power-of-2
    EXPECT_EQ(config.max_message_size, 4096);
}

// Test duplicate channel name
TEST(BrokerTest, RequestChannelDuplicate) {
    auto& broker = omni::MailboxBroker::Instance();
    
    // Create first channel
    auto [error1, channel1] = broker.RequestChannel("test-channel-duplicate", {
        .capacity = 512,
        .max_message_size = 2048
    });
    
    ASSERT_EQ(error1, omni::ChannelError::Success);
    ASSERT_TRUE(channel1.has_value());
    
    // Attempt to create second channel with same name
    auto [error2, channel2] = broker.RequestChannel("test-channel-duplicate", {
        .capacity = 1024,
        .max_message_size = 4096
    });
    
    // Should fail with NameExists
    EXPECT_EQ(error2, omni::ChannelError::NameExists);
    EXPECT_FALSE(channel2.has_value());
}

// Test invalid configuration
TEST(BrokerTest, RequestChannelInvalidConfig) {
    auto& broker = omni::MailboxBroker::Instance();
    
    // According to spec, configs are auto-normalized, so out-of-range values
    // get clamped to valid ranges. InvalidConfig only occurs if normalization
    // produces an invalid result (which shouldn't happen with proper Normalize()).
    
    // Test that small capacity gets normalized up to minimum (8)
    auto [error1, channel1] = broker.RequestChannel("test-normalized-1", {
        .capacity = 4,  // Below minimum, should be clamped to 8
        .max_message_size = 1024
    });
    
    EXPECT_EQ(error1, omni::ChannelError::Success);
    ASSERT_TRUE(channel1.has_value());
    EXPECT_EQ(channel1->producer.GetConfig().capacity, 8);  // Clamped to minimum
    
    // Test that large capacity gets normalized down to maximum (524'288)
    auto [error2, channel2] = broker.RequestChannel("test-normalized-2", {
        .capacity = 1'000'000,  // Above maximum, should be clamped to 524'288
        .max_message_size = 1024
    });
    
    EXPECT_EQ(error2, omni::ChannelError::Success);
    ASSERT_TRUE(channel2.has_value());
    EXPECT_EQ(channel2->producer.GetConfig().capacity, 524'288);  // Clamped to maximum
    
    // Test that small message size gets normalized up to minimum (64)
    auto [error3, channel3] = broker.RequestChannel("test-normalized-3", {
        .capacity = 512,
        .max_message_size = 32  // Below minimum, should be clamped to 64
    });
    
    EXPECT_EQ(error3, omni::ChannelError::Success);
    ASSERT_TRUE(channel3.has_value());
    EXPECT_EQ(channel3->producer.GetConfig().max_message_size, 64);  // Clamped to minimum
    
    // Test that large message size gets normalized down to maximum (1'048'576)
    auto [error4, channel4] = broker.RequestChannel("test-normalized-4", {
        .capacity = 512,
        .max_message_size = 2'000'000  // Above maximum, should be clamped to 1'048'576
    });
    
    EXPECT_EQ(error4, omni::ChannelError::Success);
    ASSERT_TRUE(channel4.has_value());
    EXPECT_EQ(channel4->producer.GetConfig().max_message_size, 1'048'576);  // Clamped to maximum
}


