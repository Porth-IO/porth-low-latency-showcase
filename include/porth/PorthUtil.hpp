/**
 * @file PorthUtil.hpp
 * @brief System-level utility functions for thread isolation and RT scheduling.
 *
 * Porth-IO: The Sovereign Logic Layer
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <iostream>
#include <numa.h>
#include <pthread.h>
#include <sched.h>
#include <string>

namespace porth {

/** * @brief Maximum priority for Real-Time FIFO scheduling.
 * Level 99 ensures the Porth driver preempts all other system tasks,
 * including standard kernel threads, to maintain GaN switching determinism.
 */
constexpr int MAX_PTHREAD_FIFO_PRIORITY = 99;

/**
 * @enum PorthStatus
 * @brief Explicit status codes for high-speed I/O operations.
 *
 * These codes bridge the gap between abstract C++ logic and physical hardware
 * reality, allowing the Sovereign Logic Layer to communicate specific failures
 * back to the financial execution engines or command-and-control layers.
 */
enum class PorthStatus : uint8_t {
    SUCCESS = 0,           ///< Operation completed within the defined latency window.
    BUSY,                  ///< Hardware state machine is locked (e.g., InP lattice stabilizing).
    EMPTY,                 ///< Consumer-side: No new descriptors available in the RingBuffer.
    FULL,                  ///< Producer-side: RingBuffer saturated; must drop to preserve latency.
    ERROR_AFFINITY,        ///< Failed to isolate logic on the requested physical core.
    ERROR_PRIORITY,        ///< Failed to achieve Real-Time sovereignty (permissions error).
    ERROR_HARDWARE_TIMEOUT ///< Hardware failed to respond within the Newport Cluster PDK limits.
};

/**
 * @brief pin_thread_to_core: Locks a thread to a specific physical CPU core.
 *
 * Essential for the "Jitter Shield." By pinning the driver thread, we prevent
 * the Linux scheduler from migrating the process. This ensures the CPU's
 * L1/L2 caches remain "warm" with Newport Cluster logic, eliminating
 * the 500ns–2000ns latency spikes associated with cache-misses and TLB flushes.
 *
 * @param core_id The logical index of the physical core (0-indexed).
 * @return std::expected containing void on success, or PorthStatus on failure.
 * @note This is a "Performance Guard" against kernel-level non-determinism.
 */
[[nodiscard]] inline auto pin_thread_to_core(int core_id) noexcept
    -> std::expected<void, PorthStatus> {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    pthread_t current_thread = pthread_self();
    // pthread_setaffinity_np is used to enforce hard-affinity at the pthread level.
    int result = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);

    if (result != 0) {
        std::cerr << std::format("[Porth-Util] Warning: Could not pin thread to core {}\n",
                                 core_id);
        return std::unexpected(PorthStatus::ERROR_AFFINITY);
    }

    std::cout << std::format("[Porth-Util] Thread successfully pinned to core {}\n", core_id);
    return {};
}

/**
 * @brief set_realtime_priority: Elevates the calling thread to SCHED_FIFO.
 *
 * This function establishes "Scheduling Sovereignty." It ensures the Porth
 * driver is never preempted by standard OS tasks (like logging or networking
 * interrupts). Priority 99 grants the logic layer total control over the
 * CPU cycle budget, critical for nanosecond-scale telemetry.
 *
 * @return std::expected containing void on success, or PorthStatus on failure.
 * @note REQUIRES: CAP_SYS_NICE or root privileges. Without this, the
 * system remains vulnerable to millisecond-scale OS jitter.
 */
[[nodiscard]] inline auto set_realtime_priority() noexcept -> std::expected<void, PorthStatus> {
    struct sched_param param{};
    param.sched_priority = MAX_PTHREAD_FIFO_PRIORITY;

    // SCHED_FIFO: First-In-First-Out real-time scheduling.
    // This prevents the thread from being pushed out of the CPU until it yields.
    int result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

    if (result != 0) {
        std::cerr << "[Porth-Util] Warning: Could not set SCHED_FIFO (requires sudo/root)\n";
        return std::unexpected(PorthStatus::ERROR_PRIORITY);
    }

    std::cout << "[Porth-Util] Thread priority elevated to Real-Time (SCHED_FIFO)\n";
    return {};
}

/**
 * @brief get_current_numa_node: Identifies the physical NUMA node of the caller.
 * @return The NUMA node ID (0-indexed).
 */
[[nodiscard]] inline auto get_current_numa_node() noexcept -> int {
    int node = numa_node_of_cpu(sched_getcpu());
    return (node < 0) ? 0 : node; // Fallback to Node 0 if virtualized
}

} // namespace porth