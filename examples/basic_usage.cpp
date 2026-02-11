// examples/basic_usage.cpp
/**
 * @file basic_usage.cpp
 * @brief Simple demonstration of OmniMailbox core features
 * 
 * This example shows:
 * - Getting the broker singleton
 * - Requesting a channel with error handling
 * - Producer thread sending messages
 * - Consumer thread receiving messages
 * - Clean shutdown
 */

#include <omni/mailbox.hpp>
#include <iostream>
#include <thread>
#include <string>
#include <chrono>

int main() {
    std::cout << "=== OmniMailbox Basic Usage Example ===\n\n";
    
    // Step 1: Get the singleton broker instance
    // The broker manages all channels in the application
    auto& broker = omni::MailboxBroker::Instance();
    std::cout << "Step 1: Got broker instance\n";
    
    // Step 2: Request a channel with configuration
    // Channels are identified by unique names
    auto [error, channel] = broker.RequestChannel("demo-channel", {
        .capacity = 16,           // Ring buffer slots (will be rounded to power of 2)
        .max_message_size = 256   // Maximum message size in bytes
    });
    
    // Step 3: Handle potential errors
    if (error != omni::ChannelError::Success) {
        switch (error) {
            case omni::ChannelError::NameExists:
                std::cerr << "Error: Channel 'demo-channel' already exists\n";
                break;
            case omni::ChannelError::InvalidConfig:
                std::cerr << "Error: Invalid configuration\n";
                break;
            case omni::ChannelError::AllocationFailed:
                std::cerr << "Error: Memory allocation failed\n";
                break;
            default:
                std::cerr << "Error: Unknown error\n";
        }
    }
    
    std::cout << "Step 2-3: Channel created successfully\n\n";
    
    // NOTE: This example uses a small queue (capacity=16) with slow message rate
    // to demonstrate basic functionality. In production:
    // - If consumer is slower than producer, queue will fill and messages may be dropped
    // - Use larger capacity (e.g., 1024+) for high-throughput scenarios
    // - Monitor AvailableSlots() to detect saturation
    // - See backpressure_demo.cpp for handling queue saturation
    
    // Step 4: Create consumer thread to receive messages
    // The consumer will block until messages arrive
    std::thread consumer_thread([&channel]() {
        auto& consumer = channel->consumer;
        
        std::cout << "[Consumer] Started, waiting for messages...\n";
        
        for (int i = 0; i < 10; ++i) {
            // BlockingPop waits until a message is available or timeout occurs
            auto [result, msg] = consumer.BlockingPop(std::chrono::seconds(5));
            
            if (result == omni::PopResult::Success) {
                // Convert received bytes to string for display
                auto data = msg->Data();
                std::string received(
                    reinterpret_cast<const char*>(data.data()),
                    data.size()
                );
                
                std::cout << "[Consumer] Received message " << (i + 1) 
                          << ": " << received << "\n";
                          
            } else if (result == omni::PopResult::ChannelClosed) {
                std::cout << "[Consumer] Producer disconnected\n";
                break;
                
            } else if (result == omni::PopResult::Timeout) {
                std::cout << "[Consumer] Timeout waiting for message\n";
                break;
            }
        }
        
        std::cout << "[Consumer] Finished\n";
    });
    
    // Give consumer time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Step 5: Create producer thread to send messages
    std::thread producer_thread([&channel]() {
        auto& producer = channel->producer;
        
        std::cout << "[Producer] Started, sending 10 messages...\n\n";
        
        for (int i = 0; i < 10; ++i) {
            // Create a simple text message
            std::string message = "Hello from OmniMailbox #" + std::to_string(i + 1);
            
            // Convert string to byte span for sending
            std::span<const uint8_t> data(
                reinterpret_cast<const uint8_t*>(message.data()),
                message.size()
            );
            
            // BlockingPush will wait if queue is full
            auto result = producer.BlockingPush(data, std::chrono::seconds(5));
            
            if (result == omni::PushResult::Success) {
                std::cout << "[Producer] Sent message " << (i + 1) << "\n";
                
            } else if (result == omni::PushResult::ChannelClosed) {
                std::cout << "[Producer] Consumer disconnected\n";
                break;
                
            } else if (result == omni::PushResult::Timeout) {
                std::cout << "[Producer] Timeout sending message\n";
                break;
            }
            
            // Small delay between messages for readability
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        std::cout << "\n[Producer] Finished\n";
    });
    
    // Step 6: Wait for both threads to complete
    producer_thread.join();
    consumer_thread.join();
    
    // Step 7: Clean shutdown
    // Handles are automatically destroyed when channel goes out of scope
    // This signals the other side that the peer has disconnected
    std::cout << "\n=== Cleanup ===\n";
    std::cout << "Destroying channel handles...\n";
    channel.reset();
    
    // Remove the channel from the broker's registry
    if (broker.RemoveChannel("demo-channel")) {
        std::cout << "Channel removed from broker\n";
    }
    
    // Display final statistics
    auto stats = broker.GetStats();
    std::cout << "\nBroker Statistics:\n";
    std::cout << "  Active channels: " << stats.active_channels << "\n";
    std::cout << "  Total created: " << stats.total_channels_created << "\n";
    std::cout << "  Total messages sent: " << stats.total_messages_sent << "\n";
    std::cout << "  Total bytes transferred: " << stats.total_bytes_transferred << "\n";
    
    std::cout << "\n=== Example Complete ===\n";
    return 0;
}
