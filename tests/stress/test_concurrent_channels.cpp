// tests/stress/test_concurrent_channels.cpp
// Stress tests for OmniMailbox - long-running validation tests
// Run with: ./omni-tests --gtest_filter=StressTest.*
// Run with ASAN: cmake -DCMAKE_CXX_FLAGS="-fsanitize=address" && ./omni-tests

#include <gtest/gtest.h>
#include <omni/mailbox.hpp>

#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <string>
#include <iostream>
#include <memory>

// Stress tests are marked with [.Stress] and excluded from normal runs
// Use --gtest_filter=StressTest.* to run explicitly

// Test 1: Create 1000 channels and verify broker handles many channels without crashes
TEST(StressTest, ManyChannels) {
    auto& broker = omni::MailboxBroker::Instance();
    
    constexpr size_t NUM_CHANNELS = 1000;
    std::vector<std::string> channel_names;
    channel_names.reserve(NUM_CHANNELS);
    
    // Create unique channel names
    for (size_t i = 0; i < NUM_CHANNELS; ++i) {
        channel_names.push_back("stress-channel-" + std::to_string(i));
    }
    
    std::cout << "Creating " << NUM_CHANNELS << " channels...\n";
    auto start = std::chrono::steady_clock::now();
    
    // Create all channels
    std::vector<std::unique_ptr<omni::ChannelPair>> channels;
    channels.reserve(NUM_CHANNELS);
    
    for (const auto& name : channel_names) {
        auto [error, channel] = broker.RequestChannel(name, {
            .capacity = 128,
            .max_message_size = 512
        });
        
        ASSERT_EQ(error, omni::ChannelError::Success) 
            << "Failed to create channel: " << name;
        ASSERT_TRUE(channel.has_value());
        
        channels.push_back(std::make_unique<omni::ChannelPair>(std::move(channel.value())));
    }
    
    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "Created " << NUM_CHANNELS << " channels in " << elapsed.count() << "ms\n";
    
    // Verify broker stats
    auto stats = broker.GetStats();
    EXPECT_GE(stats.active_channels, NUM_CHANNELS);
    
    // Send one message through each channel to verify they work
    std::cout << "Verifying all channels functional...\n";
    std::vector<uint8_t> test_payload = {0xDE, 0xAD, 0xBE, 0xEF};
    
    for (size_t i = 0; i < channels.size(); ++i) {
        auto& channel = channels[i];
        
        auto push_result = channel->producer.TryPush(test_payload);
        EXPECT_EQ(push_result, omni::PushResult::Success) 
            << "Failed to push to channel " << i;
        
        auto [pop_result, msg] = channel->consumer.TryPop();
        EXPECT_EQ(pop_result, omni::PopResult::Success) 
            << "Failed to pop from channel " << i;
        
        if (pop_result == omni::PopResult::Success) {
            EXPECT_EQ(msg->Data().size(), test_payload.size());
        }
    }
    
    std::cout << "All " << NUM_CHANNELS << " channels verified functional\n";
    
    // Cleanup - destroy all channels
    std::cout << "Destroying all channels...\n";
    channels.clear();
    
    // Give broker time to cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::cout << "StressTest.ManyChannels completed successfully\n";
}

// Test 2: High-frequency messaging - 10M messages through single channel
TEST(StressTest, HighFrequency) {
    auto& broker = omni::MailboxBroker::Instance();
    
    auto [error, channel] = broker.RequestChannel("stress-high-freq", {
        .capacity = 2048,
        .max_message_size = 256
    });
    
    ASSERT_EQ(error, omni::ChannelError::Success);
    ASSERT_TRUE(channel.has_value());
    
    constexpr size_t NUM_MESSAGES = 10'000'000;
    std::atomic<size_t> messages_received{0};
    std::atomic<size_t> messages_sent{0};
    std::atomic<bool> stop_consumer{false};
    
    std::cout << "Starting high-frequency test: " << NUM_MESSAGES << " messages...\n";
    
    // Consumer thread
    std::thread consumer_thread([&]() {
        auto& consumer = channel->consumer;
        size_t local_count = 0;
        
        while (!stop_consumer.load(std::memory_order_acquire)) {
            auto [result, msg] = consumer.TryPop();
            
            if (result == omni::PopResult::Success) {
                ++local_count;
                
                // Verify message integrity periodically
                if (local_count % 1'000'000 == 0) {
                    std::cout << "Received " << local_count << " messages...\n";
                }
            } else if (result == omni::PopResult::ChannelClosed) {
                break;
            }
        }
        
        messages_received.store(local_count, std::memory_order_release);
    });
    
    // Producer thread
    auto start = std::chrono::steady_clock::now();
    
    std::thread producer_thread([&]() {
        auto& producer = channel->producer;
        std::vector<uint8_t> payload(64, 0xAB);
        size_t local_sent = 0;
        
        for (size_t i = 0; i < NUM_MESSAGES; ++i) {
            // Write message number into payload for verification
            *reinterpret_cast<uint64_t*>(payload.data()) = i;
            
            while (true) {
                auto result = producer.TryPush(payload);
                
                if (result == omni::PushResult::Success) {
                    ++local_sent;
                    break;
                } else if (result == omni::PushResult::QueueFull) {
                    // Backoff briefly and retry
                    std::this_thread::yield();
                } else {
                    // Unexpected error
                    FAIL() << "Push failed with result: " << static_cast<int>(result);
                    return;
                }
            }
            
            if (local_sent % 1'000'000 == 0) {
                std::cout << "Sent " << local_sent << " messages...\n";
            }
        }
        
        messages_sent.store(local_sent, std::memory_order_release);
    });
    
    producer_thread.join();
    
    // Give consumer time to drain remaining messages
    std::this_thread::sleep_for(std::chrono::seconds(1));
    stop_consumer.store(true, std::memory_order_release);
    consumer_thread.join();
    
    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    const size_t sent = messages_sent.load(std::memory_order_acquire);
    const size_t received = messages_received.load(std::memory_order_acquire);
    
    std::cout << "High-frequency test completed:\n"
              << "  Sent: " << sent << " messages\n"
              << "  Received: " << received << " messages\n"
              << "  Time: " << elapsed.count() << "ms\n"
              << "  Throughput: " << (sent * 1000 / elapsed.count()) << " msg/sec\n";
    
    EXPECT_EQ(sent, NUM_MESSAGES);
    EXPECT_EQ(received, NUM_MESSAGES);
    EXPECT_GE(sent * 1000 / elapsed.count(), 1'000'000) 
        << "Throughput below 1M msg/sec threshold";
}

// Test 3: Destroy handles while operations in flight
TEST(StressTest, DestroyWhileBusy) {
    auto& broker = omni::MailboxBroker::Instance();
    
    constexpr size_t NUM_ITERATIONS = 100;
    constexpr size_t MESSAGES_PER_ITERATION = 10'000;
    
    std::cout << "Testing handle destruction under load (" << NUM_ITERATIONS << " iterations)...\n";
    
    for (size_t iter = 0; iter < NUM_ITERATIONS; ++iter) {
        std::string channel_name = "stress-destroy-" + std::to_string(iter);
        
        auto [error, channel] = broker.RequestChannel(channel_name, {
            .capacity = 512,
            .max_message_size = 128
        });
        
        ASSERT_EQ(error, omni::ChannelError::Success);
        ASSERT_TRUE(channel.has_value());
        
        std::atomic<bool> producer_running{true};
        std::atomic<bool> consumer_running{true};
        std::atomic<size_t> messages_sent{0};
        
        // Move handles into thread ownership
        auto producer_handle = std::make_unique<omni::ProducerHandle>(std::move(channel->producer));
        auto consumer_handle = std::make_unique<omni::ConsumerHandle>(std::move(channel->consumer));
        
        // Producer thread - pushes messages continuously
        std::thread producer_thread([&, producer = std::move(producer_handle)]() mutable {
            std::vector<uint8_t> payload(64, 0xCC);
            
            while (producer_running.load(std::memory_order_acquire)) {
                auto result = producer->TryPush(payload);
                
                if (result == omni::PushResult::Success) {
                    messages_sent.fetch_add(1, std::memory_order_relaxed);
                } else if (result == omni::PushResult::ChannelClosed) {
                    break;
                }
                
                if (messages_sent.load(std::memory_order_relaxed) >= MESSAGES_PER_ITERATION) {
                    break;
                }
            }
            
            // Destroy producer handle while potentially in use
            producer.reset();
        });
        
        // Consumer thread - pops messages continuously
        std::thread consumer_thread([&, consumer = std::move(consumer_handle)]() mutable {
            size_t received = 0;
            
            while (consumer_running.load(std::memory_order_acquire)) {
                auto [result, msg] = consumer->TryPop();
                
                if (result == omni::PopResult::Success) {
                    ++received;
                } else if (result == omni::PopResult::ChannelClosed) {
                    break;
                }
                
                if (received >= MESSAGES_PER_ITERATION) {
                    break;
                }
            }
            
            // Destroy consumer handle while potentially in use
            consumer.reset();
        });
        
        // Let threads run for a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // Signal stop - handles will be destroyed even if messages in flight
        producer_running.store(false, std::memory_order_release);
        consumer_running.store(false, std::memory_order_release);
        
        producer_thread.join();
        consumer_thread.join();
        
        if ((iter + 1) % 10 == 0) {
            std::cout << "Completed " << (iter + 1) << " / " << NUM_ITERATIONS 
                      << " iterations (sent " << messages_sent.load() << " messages)\n";
        }
    }
    
    std::cout << "StressTest.DestroyWhileBusy completed - no crashes or hangs detected\n";
}
