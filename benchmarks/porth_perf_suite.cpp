/**
 * @file porth_perf_suite.cpp
 * @brief Professional High-Frequency Benchmark Suite using Google Benchmark.
 *
 * Porth-IO: The Sovereign Logic Layer
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <benchmark/benchmark.h>
#include <atomic>
#include <thread>
#include "porth/PorthRingBuffer.hpp"
#include "porth/PorthUtil.hpp"

using namespace porth;

constexpr size_t BENCH_RING_SIZE = 1024;
constexpr uint64_t BENCH_ADDR    = 0x1000;
constexpr uint32_t BENCH_LEN     = 64;

// 1. Single-Threaded Uncontended Latency (Baseline L1 Cache speed)
static void bm_spsc_uncontended_latency(benchmark::State& state) {
    PorthRingBuffer<BENCH_RING_SIZE> ring;
    PorthDescriptor desc = {.addr = BENCH_ADDR, .len = BENCH_LEN};
    PorthDescriptor out;

    for (auto _ : state) {
        benchmark::DoNotOptimize(ring.push(desc));
        benchmark::DoNotOptimize(ring.pop(out));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(bm_spsc_uncontended_latency)->Unit(benchmark::kNanosecond);

// 2. Multi-Threaded Contended Throughput (Real-World HFT Scenario)
static void bm_spsc_contended_throughput(benchmark::State& state) {
    PorthRingBuffer<BENCH_RING_SIZE> ring;
    std::atomic<bool> running{true};

    // Dedicated Consumer Thread
    std::thread consumer([&]() {
        PorthDescriptor out;
        while (running.load(std::memory_order_relaxed)) {
            if (ring.pop(out)) {
                benchmark::DoNotOptimize(out);
            }
        }
    });

    PorthDescriptor desc = {.addr = BENCH_ADDR, .len = BENCH_LEN};
    
    // Producer Loop
    for (auto _ : state) {
        // Spin until push is successful (simulating real hardware backpressure)
        while (!ring.push(desc)) {
            benchmark::DoNotOptimize(desc);
        }
    }

    state.SetItemsProcessed(state.iterations());
    
    // Clean up
    running.store(false, std::memory_order_relaxed);
    consumer.join();
}
// Use RealTime to accurately measure cross-thread wall-clock performance
BENCHMARK(bm_spsc_contended_throughput)->Unit(benchmark::kNanosecond)->UseRealTime();

BENCHMARK_MAIN();