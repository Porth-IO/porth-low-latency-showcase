/**
 * @file PorthClock.hpp
 * @brief Hardware-abstracted access to high-precision CPU cycle counters.
 *
 * Porth-IO: The Sovereign Logic Layer
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

// Only include architecture-specific headers if we are on a supported platform.
// Required for __rdtsc and __rdtscp intrinsics on x86 platforms.
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace porth {

/**
 * @class PorthClock
 * @brief Provides low-latency access to hardware timing primitives.
 *
 * This class abstracts the differences between Intel TSC (Time Stamp Counter)
 * and the ARM64 Generic Timer (CNTVCT_EL0). It is designed for sub-microsecond
 * latency measurements in HFT and high-speed interconnect environments.
 * * @note All methods assume an invariant TSC/Timer frequency. On the Newport Cluster,
 * this prevents timing drift caused by P-state transitions or thermal throttling.
 */
class PorthClock {
public:
    /**
     * @brief Reads the hardware cycle counter without serialization.
     * * Fastest timing path available. On modern out-of-order CPUs, this read
     * may be reordered by the front-end fetch unit.
     * * @return uint64_t Current CPU clock cycles.
     * @note Use this for high-frequency telemetry where the overhead of a pipeline
     * stall (serialization) outweighs the need for exact instruction-boundary precision.
     */
    static auto now() noexcept -> uint64_t {
#if defined(__x86_64__) || defined(__i386__)
        // Intel/AMD Path: RDTSC reads the 64-bit time-stamp counter.
        // We use the intrinsic to allow the compiler to optimize register allocation.
        return __rdtsc();
#elif defined(__aarch64__)
        uint64_t virtual_timer_value = 0;
        // ARM64 Path: Read the virtual count register (CNTVCT_EL0).
        // This register provides a uniform view of time across all cores in the
        // Newport Cluster, essential for cross-core event correlation.
        asm volatile("mrs %0, cntvct_el0" : "=r"(virtual_timer_value));
        return virtual_timer_value;
#else
#error "Porth-IO: Unsupported CPU architecture for high-precision timing."
#endif
    }

    /**
     * @brief Reads the hardware cycle counter with instruction serialization.
     * * Ensures that all previous instructions have reached the "retirement" stage
     * before the counter is sampled. Essential for measuring the execution delta
     * of critical code paths.
     *
     * @return uint64_t Current CPU clock cycles (serialized).
     * @note On x86, this also returns the processor ID via the aux parameter,
     * though it is discarded here to maintain API parity with ARM64.
     */
    static auto now_precise() noexcept -> uint64_t {
#if defined(__x86_64__) || defined(__i386__)
        unsigned int aux = 0;
        // RDTSCP is a serializing instruction; it forces the CPU to wait until
        // all previous instructions in the stream have executed.
        return __rdtscp(&aux);
#elif defined(__aarch64__)
        uint64_t val = 0;
        // ISB (Instruction Synchronization Barrier) flushes the pipeline and
        // ensures that the MRS read is not speculatively executed ahead of
        // previous logic. This is the ARM equivalent of x86 serialization.
        asm volatile("isb; mrs %0, cntvct_el0" : "=r"(val));
        return val;
#endif
    }

    /**
     * @brief Injects a hardware memory barrier (Fence).
     * * Prevents the CPU and compiler from reordering memory operations across
     * this point. This is the "Sovereign Guard" for lock-free concurrency.
     * * @note Critical for SPSC/MPMC queues to ensure that data written to a buffer
     * is globally visible before the 'tail' pointer update is committed.
     */
    static void fence() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        // LFENCE acts as a load-load and load-store fence on Intel.
        // The "memory" clobber prevents the C++ compiler from moving loads/stores.
        asm volatile("lfence" ::: "memory");
#elif defined(__aarch64__)
        // DMB ISH (Data Memory Barrier, Inner Shareable) ensures that all
        // memory accesses within the cluster share domain are completed.
        // Necessary for GaN-based hardware interconnects to maintain cache coherency.
        asm volatile("dmb ish" ::: "memory");
#endif
    }
};

} // namespace porth