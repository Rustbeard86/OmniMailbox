#include <omni/detail/config.hpp>
#include <gtest/gtest.h>

using namespace omni;

TEST(ConfigTest, NormalizeCapacity) {
    // Test: Normalize(1000) should become 1024
    ChannelConfig config1;
    config1.capacity = 1000;
    auto normalized1 = config1.Normalize();
    EXPECT_EQ(normalized1.capacity, 1024);
    
    // Test: Normalize(524288) should stay 524288
    ChannelConfig config2;
    config2.capacity = 524288;
    auto normalized2 = config2.Normalize();
    EXPECT_EQ(normalized2.capacity, 524288);
    
    // Test: Normalize(1000000) should clamp to 524288
    ChannelConfig config3;
    config3.capacity = 1000000;
    auto normalized3 = config3.Normalize();
    EXPECT_EQ(normalized3.capacity, 524288);
}

TEST(ConfigTest, NormalizeCapacityMinimum) {
    // Test: Values below minimum should clamp to 8
    ChannelConfig config;
    config.capacity = 4;
    auto normalized = config.Normalize();
    EXPECT_EQ(normalized.capacity, 8);
}

TEST(ConfigTest, NormalizeCapacityPowerOf2) {
    // Test: Power of 2 values within range should remain unchanged
    ChannelConfig config;
    config.capacity = 256;
    auto normalized = config.Normalize();
    EXPECT_EQ(normalized.capacity, 256);
}

TEST(ConfigTest, NormalizeMaxMessageSize) {
    // Test: max_message_size should clamp to valid range
    ChannelConfig config1;
    config1.max_message_size = 32;  // Below minimum
    auto normalized1 = config1.Normalize();
    EXPECT_EQ(normalized1.max_message_size, 64);
    
    ChannelConfig config2;
    config2.max_message_size = 2000000;  // Above maximum
    auto normalized2 = config2.Normalize();
    EXPECT_EQ(normalized2.max_message_size, 1048576);
}

TEST(ConfigTest, IsValidValid) {
    // Test: Valid configuration
    ChannelConfig config;
    config.capacity = 1024;
    config.max_message_size = 4096;
    EXPECT_TRUE(config.IsValid());
}

TEST(ConfigTest, IsValidInvalidCapacity) {
    // Test: Capacity out of range
    ChannelConfig config1;
    config1.capacity = 4;  // Too small
    EXPECT_FALSE(config1.IsValid());
    
    ChannelConfig config2;
    config2.capacity = 1000000;  // Too large
    EXPECT_FALSE(config2.IsValid());
    
    // Test: Capacity not power of 2
    ChannelConfig config3;
    config3.capacity = 1000;
    EXPECT_FALSE(config3.IsValid());
}

TEST(ConfigTest, IsValidInvalidMessageSize) {
    // Test: Message size out of range
    ChannelConfig config1;
    config1.capacity = 1024;
    config1.max_message_size = 32;  // Too small
    EXPECT_FALSE(config1.IsValid());
    
    ChannelConfig config2;
    config2.capacity = 1024;
    config2.max_message_size = 2000000;  // Too large
    EXPECT_FALSE(config2.IsValid());
}

TEST(ConfigTest, DefaultConfig) {
    // Test: Default configuration should be valid
    ChannelConfig config;
    EXPECT_TRUE(config.IsValid());
    EXPECT_EQ(config.capacity, 1024);
    EXPECT_EQ(config.max_message_size, 4096);
}

TEST(EnumTest, PushResultValues) {
    // Test: Enum values exist and are distinct
    EXPECT_NE(PushResult::Success, PushResult::Timeout);
    EXPECT_NE(PushResult::Success, PushResult::ChannelClosed);
    EXPECT_NE(PushResult::Success, PushResult::InvalidSize);
    EXPECT_NE(PushResult::Success, PushResult::QueueFull);
}

TEST(EnumTest, PopResultValues) {
    // Test: Enum values exist and are distinct
    EXPECT_NE(PopResult::Success, PopResult::Timeout);
    EXPECT_NE(PopResult::Success, PopResult::ChannelClosed);
    EXPECT_NE(PopResult::Success, PopResult::Empty);
}

TEST(EnumTest, ChannelErrorValues) {
    // Test: Enum values exist and are distinct
    EXPECT_NE(ChannelError::Success, ChannelError::NameExists);
    EXPECT_NE(ChannelError::Success, ChannelError::InvalidConfig);
    EXPECT_NE(ChannelError::Success, ChannelError::AllocationFailed);
}
