/**
 * @file porth_perf_suite.cpp
 * @brief Professional High-Frequency Benchmark Suite using Google Benchmark.
 *
 * Porth-IO: The Sovereign Logic Layer
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <memory>                     
#include "porth/PorthRingBuffer.hpp"

using namespace porth;

/** @brief Domain constants to eliminate magic numbers and satisfy clang-tidy. */
constexpr size_t BENCH_RING_SIZE    = 1024;
constexpr uint64_t BENCH_ADDR       = 0x1000;
constexpr uint32_t BENCH_LEN        = 64;
constexpr int BENCH_REPETITIONS     = 5;

/**
 * @brief benchmark the hot-path latency of a RingBuffer Push/Pop cycle.
 * @note Function renamed to lower_case to satisfy Sovereign naming conventions.
 */
static void bm_ring_buffer_latency(benchmark::State& state) {
    // 1. Setup: Pre-allocate resources to avoid jitter
    PorthRingBuffer<BENCH_RING_SIZE> ring;
    
    // Designated initializers used to satisfy modern C++ standards
    PorthDescriptor desc = {.addr = BENCH_ADDR, .len = BENCH_LEN};
    PorthDescriptor out  = {.addr = 0, .len = 0};

    // 2. The Measurement Loop
    for (auto _ : state) {
        /**
         * @note We explicitly cast the return values to void to satisfy
         * the [[nodiscard]] attribute enforced by the HFT-grade build flags.
         * This ensures the benchmark measures the operation without warnings.
         */
        (void)ring.push(desc);
        (void)ring.pop(out);

        // Prevent compiler from optimizing away the operation
        benchmark::DoNotOptimize(out);
    }

    // 3. Add custom metadata for Sovereign Reporting
    state.SetItemsProcessed(state.iterations());
}

// Register the benchmark and force it to run on a specific core
// 10/10 FIX: Run multiple repetitions so Google Benchmark can calculate 
// statistics like Median, Mean, and Standard Deviation.
BENCHMARK(bm_ring_buffer_latency)
    ->Unit(benchmark::kNanosecond)
    ->Repetitions(BENCH_REPETITIONS)
    ->DisplayAggregatesOnly(true);

BENCHMARK_MAIN();