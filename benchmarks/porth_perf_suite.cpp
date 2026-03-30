/**
 * @file porth_perf_suite.cpp
 * @brief Professional High-Frequency Benchmark Suite using Google Benchmark.
 *
 * Porth-IO: Low Latency Showcase
 */

#include <benchmark/benchmark.h>
#include <atomic>
#include <thread>
#include "porth/PorthRingBuffer.hpp"
#include "porth/PorthUtil.hpp"

using namespace porth;

/** @brief Benchmarking constants for standardized hardware profiling. */
constexpr size_t BENCH_RING_SIZE = 1024;
constexpr uint64_t BENCH_ADDR    = 0x1000;
constexpr uint32_t BENCH_LEN     = 64;

/**
 * @brief Benchmark: Single-Threaded Uncontended Latency.
 * * Measures the raw baseline performance of the PorthRingBuffer when the 
 * producer and consumer share the same L1 cache and execution core. 
 * This establishes the theoretical minimum overhead of the ring buffer logic.
 */
static void bm_spsc_uncontended_latency(benchmark::State& state) {
    PorthRingBuffer<BENCH_RING_SIZE> ring;
    PorthDescriptor desc = {.addr = BENCH_ADDR, .len = BENCH_LEN};
    PorthDescriptor out;

    for (auto _ : state) {
        // Use DoNotOptimize to prevent the compiler from eliding the 
        // push/pop operations during aggressive optimization.
        benchmark::DoNotOptimize(ring.push(desc));
        benchmark::DoNotOptimize(ring.pop(out));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(bm_spsc_uncontended_latency)->Unit(benchmark::kNanosecond);

/**
 * @brief Benchmark: Multi-Threaded Contended Throughput.
 * * Simulates a real-world high-frequency data plane scenario where the 
 * producer and consumer are isolated on different physical cores. 
 * This test stresses the cache-line separation (alignas) and the 
 * Acquire/Release memory barriers under heavy interconnect contention.
 */
static void bm_spsc_contended_throughput(benchmark::State& state) {
    PorthRingBuffer<BENCH_RING_SIZE> ring;
    std::atomic<bool> running{true};

    // Dedicated Consumer Thread: Simulates a hardware driver or network ingress.
    std::thread consumer([&]() {
        PorthDescriptor out;
        while (running.load(std::memory_order_relaxed)) {
            if (ring.pop(out)) {
                benchmark::DoNotOptimize(out);
            }
        }
    });

    PorthDescriptor desc = {.addr = BENCH_ADDR, .len = BENCH_LEN};
    
    // Producer Loop: Simulates the logic layer pushing work to the hardware.
    for (auto _ : state) {
        // Spin until push is successful, simulating real-world backpressure handling.
        while (!ring.push(desc)) {
            benchmark::DoNotOptimize(desc);
        }
    }

    state.SetItemsProcessed(state.iterations());
    
    // Graceful cleanup of the background thread.
    running.store(false, std::memory_order_relaxed);
    consumer.join();
}
// Use RealTime to accurately measure wall-clock performance across different physical cores.
BENCHMARK(bm_spsc_contended_throughput)->Unit(benchmark::kNanosecond)->UseRealTime();

BENCHMARK_MAIN();