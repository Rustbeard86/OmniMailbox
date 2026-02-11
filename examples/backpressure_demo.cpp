// examples/backpressure_demo.cpp
/**
 * @file backpressure_demo.cpp
 * @brief Demonstrates backpressure handling when consumer can't keep up
 * 
 * This example shows:
 * - What happens when queue fills up
 * - Producer handling QueueFull condition
 * - Different strategies: blocking, dropping, retrying
 * - Monitoring queue saturation
 */

#include <omni/mailbox.hpp>
#include <iostream>
#include <thread>
#include <string>
#include <chrono>
#include <atomic>

int main() {
    std::cout << "=== OmniMailbox Backpressure Demo ===\n\n";
    
    auto& broker = omni::MailboxBroker::Instance();
    
    // SMALL queue to demonstrate saturation quickly
    auto [error, channel] = broker.RequestChannel("backpressure-demo", {
        .capacity = 8,            // Small buffer - fills quickly!
        .max_message_size = 256
    });
    
    if (error != omni::ChannelError::Success) {
        std::cerr << "Failed to create channel\n";
        return 1;
    }
    
    std::cout << "Channel created with capacity=8 (small buffer)\n\n";
    
    std::atomic<int> messages_sent{0};
    std::atomic<int> messages_dropped{0};
    std::atomic<int> messages_received{0};
    std::atomic<bool> producer_finished{false};  // Signal when producer is done
    
    // SLOW CONSUMER - simulates processing delay
    std::thread consumer_thread([&channel, &messages_received, &producer_finished]() {
        auto& consumer = channel->consumer;
        
        std::cout << "[Consumer] Started (SLOW - 200ms per message)\n\n";
        
        // Continue until producer finishes AND queue is drained
        while (!producer_finished.load() || consumer.AvailableMessages() > 0) {
            auto [result, msg] = consumer.BlockingPop(std::chrono::milliseconds(500));
            
            if (result == omni::PopResult::Success) {
                auto data = msg->Data();
                std::string received(
                    reinterpret_cast<const char*>(data.data()),
                    data.size()
                );
                
                int msg_num = messages_received.fetch_add(1) + 1;
                std::cout << "[Consumer] Received #" << msg_num 
                          << " (queue has " 
                          << consumer.AvailableMessages() << " more)\n";
                
                // SIMULATE SLOW PROCESSING
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                
            } else if (result == omni::PopResult::ChannelClosed) {
                std::cout << "[Consumer] Producer disconnected\n";
                break;
            } else if (result == omni::PopResult::Timeout) {
                // Check if producer is done and queue is empty
                if (producer_finished.load() && consumer.AvailableMessages() == 0) {
                    break;
                }
            }
        }
        
        std::cout << "[Consumer] Finished\n";
    });
    
    // Give consumer time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // FAST PRODUCER - sends faster than consumer can process
    std::thread producer_thread([&channel, &messages_sent, &messages_dropped, &producer_finished]() {
        auto& producer = channel->producer;
        
        std::cout << "[Producer] Started (FAST - 50ms per message)\n";
        std::cout << "[Producer] Strategy: Drop messages when queue full\n\n";
        
        for (int i = 0; i < 50; ++i) {
            std::string message = "Message #" + std::to_string(i + 1);
            std::span<const uint8_t> data(
                reinterpret_cast<const uint8_t*>(message.data()),
                message.size()
            );
            
            // TRY non-blocking push
            auto result = producer.TryPush(data);
            
            if (result == omni::PushResult::Success) {
                int sent = messages_sent.fetch_add(1) + 1;
                std::cout << "[Producer] Sent #" << sent 
                          << " (queue has " 
                          << producer.AvailableSlots() << " free slots)\n";
                          
            } else if (result == omni::PushResult::QueueFull) {
                int dropped = messages_dropped.fetch_add(1) + 1;
                std::cout << "[Producer] ⚠ DROPPED #" << (i + 1) 
                          << " - Queue full! (total dropped: " << dropped << ")\n";
                          
            } else if (result == omni::PushResult::ChannelClosed) {
                std::cout << "[Producer] Consumer disconnected\n";
                break;
            }
            
            // Send faster than consumer processes
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        producer_finished.store(true);  // Signal consumer we're done
        std::cout << "\n[Producer] Finished (sent " << messages_sent.load() 
                  << ", dropped " << messages_dropped.load() << ")\n";
    });
    
    producer_thread.join();
    consumer_thread.join();
    
    // Results
    std::cout << "\n=== Results ===\n";
    std::cout << "Messages sent: " << messages_sent.load() << "\n";
    std::cout << "Messages dropped: " << messages_dropped.load() << "\n";
    std::cout << "Messages received: " << messages_received.load() << "\n";
    
    int lost = messages_sent.load() - messages_received.load();
    std::cout << "Messages lost in transit: " << lost << "\n";
    
    double drop_rate = (messages_dropped.load() / 50.0) * 100.0;
    std::cout << "Drop rate: " << drop_rate << "%\n";
    
    std::cout << "\n=== Analysis ===\n";
    std::cout << "Producer rate: ~20 msg/sec (1 per 50ms)\n";
    std::cout << "Consumer rate: ~5 msg/sec (1 per 200ms)\n";
    std::cout << "Queue capacity: 8 messages\n";
    std::cout << "Result: Consumer 4x slower → Queue saturates → Drops occur\n";
    
    std::cout << "\n=== Solutions ===\n";
    std::cout << "1. Increase queue capacity (8 → 128)\n";
    std::cout << "2. Use BlockingPush() instead of TryPush() (apply backpressure)\n";
    std::cout << "3. Speed up consumer (parallel processing)\n";
    std::cout << "4. Batch consumer pops (reduce per-message overhead)\n";
    std::cout << "5. Implement priority dropping (keep important messages)\n";
    
    channel.reset();
    broker.RemoveChannel("backpressure-demo");
    
    std::cout << "\n=== Demo Complete ===\n";
    return 0;
}
