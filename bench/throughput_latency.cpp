// bench/throughput_latency.cpp
// OmniMailbox Performance Benchmarks
// 
// This file implements throughput and latency benchmarks as specified
// in section 8.1 of the design specification.

#include <benchmark/benchmark.h>
#include <omni/mailbox.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

// Throughput: Single channel, uncontended
// Producer in one thread, consumer in another
// Measures messages per second for different message sizes
static void BM_Throughput_Uncontended(benchmark::State& state) {
    auto& broker = omni::MailboxBroker::Instance();
    
    // Create unique channel name for this benchmark run
    static std::atomic<size_t> channel_counter{0};
    std::string channel_name = "bench-throughput-" + std::to_string(channel_counter.fetch_add(1));
    
    auto [error, channel] = broker.RequestChannel(channel_name, {
        .capacity = 2048,
        .max_message_size = 8192
    });
    
    if (error != omni::ChannelError::Success) {
        state.SkipWithError("Failed to create channel");
        return;
    }
    
    const size_t msg_size = state.range(0);
    std::vector<uint8_t> payload(msg_size, 0xAB);
    
    std::atomic<bool> consumer_running{true};
    std::atomic<size_t> messages_consumed{0};
    
    // Consumer thread - runs continuously
    std::thread consumer([&]() {
        while (consumer_running.load(std::memory_order_relaxed)) {
            auto [result, msg] = channel->consumer.TryPop();
            if (result == omni::PopResult::Success) {
                benchmark::DoNotOptimize(msg->Data());
                messages_consumed.fetch_add(1, std::memory_order_relaxed);
            } else if (result == omni::PopResult::Empty) {
                // Brief pause when empty to avoid spinning
                std::this_thread::yield();
            } else if (result == omni::PopResult::ChannelClosed) {
                break;
            }
        }
    });
    
    // Producer thread - benchmark loop
    for (auto _ : state) {
        auto result = channel->producer.TryPush(payload);
        if (result != omni::PushResult::Success) {
            // Queue full - pause briefly
            state.PauseTiming();
            std::this_thread::sleep_for(std::chrono::microseconds(1));
            state.ResumeTiming();
        }
    }
    
    // Signal consumer to stop
    consumer_running.store(false, std::memory_order_relaxed);
    consumer.join();
    
    // Report metrics
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * msg_size);
}

// Register throughput benchmark with different message sizes
BENCHMARK(BM_Throughput_Uncontended)
    ->Arg(64)      // Target: >= 5M msg/sec
    ->Arg(256)
    ->Arg(1024)
    ->Arg(4096)
    ->Unit(benchmark::kMicrosecond);

// Latency: Round-trip ping-pong
// Measures round-trip time between two threads
// Uses 64-byte messages
static void BM_Latency_RoundTrip(benchmark::State& state) {
    auto& broker = omni::MailboxBroker::Instance();
    
    // Create unique channel names for this benchmark run
    static std::atomic<size_t> channel_counter{0};
    size_t run_id = channel_counter.fetch_add(1);
    std::string ping_name = "ping-" + std::to_string(run_id);
    std::string pong_name = "pong-" + std::to_string(run_id);
    
    auto [error1, ping] = broker.RequestChannel(ping_name);
    auto [error2, pong] = broker.RequestChannel(pong_name);
    
    if (error1 != omni::ChannelError::Success || error2 != omni::ChannelError::Success) {
        state.SkipWithError("Failed to create channels");
        return;
    }
    
    std::vector<uint8_t> payload(64, 0xCD);
    
    std::atomic<bool> responder_running{true};
    
    // Responder thread - echoes messages back
    std::thread responder([&]() {
        while (responder_running.load(std::memory_order_relaxed)) {
            auto [result, msg] = ping->consumer.BlockingPop(std::chrono::milliseconds(100));
            if (result == omni::PopResult::Success) {
                // Echo back on pong channel
                pong->producer.TryPush(msg->Data());
            } else if (result == omni::PopResult::ChannelClosed) {
                break;
            }
            // Timeout is normal when benchmark ends
        }
    });
    
    // Ping-pong loop with manual timing
    for (auto _ : state) {
        auto start = std::chrono::high_resolution_clock::now();
        
        // Send ping
        ping->producer.BlockingPush(payload);
        
        // Wait for pong
        auto [result, msg] = pong->consumer.BlockingPop();
        
        auto end = std::chrono::high_resolution_clock::now();
        
        if (result != omni::PopResult::Success) {
            state.SkipWithError("Pong failed");
            break;
        }
        
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        state.SetIterationTime(elapsed.count() / 1e9);
    }
    
    // Signal responder to stop
    responder_running.store(false, std::memory_order_relaxed);
    responder.join();
}

// Register latency benchmark
// Target: p50 < 200ns, p99 < 500ns
BENCHMARK(BM_Latency_RoundTrip)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
