/**
 * @file PorthUtil.hpp
 * @brief System-level utility functions for thread isolation and RT scheduling.
 *
 * Porth-IO: Low Latency Showcase
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
 * Level 99 ensures the Porth-IO execution thread preempts all other system tasks,
 * including standard kernel workers and non-critical interrupts, to maintain 
 * deterministic cycle budgets for hardware switching.
 */
constexpr int MAX_PTHREAD_FIFO_PRIORITY = 99;

/**
 * @enum PorthStatus
 * @brief Explicit status codes for high-speed I/O and system operations.
 *
 * These codes bridge the gap between abstract software logic and physical hardware 
 * state, providing precise feedback for error handling in performance-critical 
 * execution environments.
 */
enum class PorthStatus : uint8_t {
    SUCCESS = 0,           ///< Operation completed within the defined latency window.
    BUSY,                  ///< Hardware state machine is locked (e.g., InP lattice stabilizing).
    EMPTY,                 ///< Consumer-side: No new descriptors available in the RingBuffer.
    FULL,                  ///< Producer-side: RingBuffer saturated; must drop to preserve latency.
    ERROR_AFFINITY,        ///< Failed to isolate logic on the requested physical core.
    ERROR_PRIORITY,        ///< Failed to achieve Real-Time scheduling (permissions error).
    ERROR_HARDWARE_TIMEOUT ///< Hardware failed to respond within defined PDK time-limits.
};

/**
 * @brief pin_thread_to_core: Locks a thread to a specific physical CPU core.
 *
 * This is a fundamental "Jitter Shield" technique. By pinning the execution thread, 
 * we prevent the Linux scheduler from migrating the process across sockets or cores. 
 * This ensures L1/L2 caches remain "warm" and eliminates the 500ns–2000ns latency 
 * spikes associated with context switches and TLB flushes.
 *
 * @param core_id The logical index of the physical core (0-indexed).
 * @return std::expected containing void on success, or PorthStatus on failure.
 */
[[nodiscard]] inline auto pin_thread_to_core(int core_id) noexcept
    -> std::expected<void, PorthStatus> {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    pthread_t current_thread = pthread_self();
    // pthread_setaffinity_np enforces hard-affinity at the pthread level, 
    // bypassing standard kernel-level distribution logic.
    int result = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);

    if (result != 0) {
        std::cerr << std::format("[Porth-Util] Warning: Could not pin thread to core {}\n",
                                 core_id);
        return std::unexpected(PorthStatus::ERROR_AFFINITY);
    }

    std::cout << std::format("[Porth-Util] Thread successfully isolated on core {}\n", core_id);
    return {};
}

/**
 * @brief set_realtime_priority: Elevates the calling thread to SCHED_FIFO.
 *
 * This function establishes "Scheduling Sovereignty," ensuring the thread is 
 * never preempted by standard OS tasks. Priority 99 grants the logic layer 
 * total control over the CPU cycle budget, which is critical for 
 * nanosecond-scale telemetry and deterministic hardware handshaking.
 *
 * @return std::expected containing void on success, or PorthStatus on failure.
 * @note REQUIRES: CAP_SYS_NICE or root privileges to override standard 
 * completely fair scheduler (CFS) policies.
 */
[[nodiscard]] inline auto set_realtime_priority() noexcept -> std::expected<void, PorthStatus> {
    struct sched_param param{};
    param.sched_priority = MAX_PTHREAD_FIFO_PRIORITY;

    // SCHED_FIFO: First-In-First-Out real-time scheduling. 
    // The thread will not be pushed out of the CPU until it yields or blocks.
    int result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

    if (result != 0) {
        std::cerr << "[Porth-Util] Warning: Could not set SCHED_FIFO (requires sudo/root)\n";
        return std::unexpected(PorthStatus::ERROR_PRIORITY);
    }

    std::cout << "[Porth-Util] Thread priority elevated to Real-Time (SCHED_FIFO)\n";
    return {};
}

/**
 * @brief get_current_numa_node: Identifies the physical NUMA node of the calling thread.
 * @return The NUMA node ID (0-indexed).
 */
[[nodiscard]] inline auto get_current_numa_node() noexcept -> int {
    int node = numa_node_of_cpu(sched_getcpu());
    return (node < 0) ? 0 : node; // Fallback to Node 0 if kernel reporting is unavailable.
}

/**
 * @brief Architecture-specific CPU relax hint.
 *
 * Prevents "Pipeline Sizzling" during busy-wait polling loops, reducing CPU 
 * power consumption and preventing speculative execution from polluting 
 * the data caches while waiting for hardware state changes.
 */
inline void cpu_relax() noexcept {
#if defined(__i386__) || defined(__x86_64__)
    // PAUSE: Notifies the CPU that the current thread is in a spin-loop.
    asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
    // ISB: Instruction Synchronization Barrier flushes the pipeline on ARM64.
    asm volatile("isb" ::: "memory");
#endif
}

} // namespace porth