#include <gtest/gtest.h>
#include "omni/mailbox_broker.hpp"

// Test that Instance() returns a singleton (same reference every time)
TEST(BrokerTest, Singleton) {
    auto& broker1 = omni::MailboxBroker::Instance();
    auto& broker2 = omni::MailboxBroker::Instance();
    
    // Both references should point to the same object
    EXPECT_EQ(&broker1, &broker2);
}

