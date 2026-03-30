/**
 * @file PorthSentinel.hpp
 * @brief Isolated background monitor for physical hardware safety.
 *
 * Porth-IO: Low Latency Showcase
 */

#pragma once

#include "PorthDeviceLayout.hpp"
#include "PorthUtil.hpp"
#include <atomic>
#include <iostream>
#include <thread>

namespace porth {

/**
 * @class PorthSentinel
 * @brief Asynchronous hardware safety monitor for protecting physical substrates.
 *
 * This class implements a dedicated monitor thread that polls hardware telemetry 
 * at the register level. It is designed to execute an emergency shutdown (trip) 
 * in sub-microsecond timeframes if the Indium Phosphide (InP) lattice temperature 
 * exceeds Physical Design Kit (PDK) safety boundaries.
 */
class PorthSentinel {
private:
    PorthDeviceLayout* m_layout;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    int m_core_id;

    /** @brief Safety Thresholds based on Porth PDK limits.
     * Max Laser Temp: 45,000 mC (45C). Beyond this threshold, lattice drift 
     * leads to irreversible hardware SNR degradation.
     */
    static constexpr uint32_t MAX_TEMP_MC = 45000;
    static constexpr uint32_t TRIP_CODE   = 0xDEADBEEF;

public:
    /** @brief Construct a new Porth-Sentinel. 
     * @param layout Pointer to the memory-mapped register layout.
     * @param core_id The physical core ID to isolate the safety thread on.
     */
    explicit PorthSentinel(PorthDeviceLayout* layout, int core_id = 1)
        : m_layout(layout), m_core_id(core_id) {}

    // Sentinel threads represent unique physical monitoring resources; copying/moving is
    // prohibited to maintain stable monitoring of hardware state.
    PorthSentinel(const PorthSentinel&)                    = delete;
    auto operator=(const PorthSentinel&) -> PorthSentinel& = delete;
    PorthSentinel(PorthSentinel&&)                         = delete;
    auto operator=(PorthSentinel&&) -> PorthSentinel&      = delete;

    /** @brief Starts the background monitoring thread.
     * Establishes thread isolation and real-time priority to ensure that safety 
     * checks are never delayed by standard OS scheduling jitter.
     */
    void start() {
        m_running = true;
        m_thread  = std::thread([this]() {
            // Establish execution isolation on the dedicated monitor core.
            // Explicitly checking the return value ensures deterministic setup.
            if (!pin_thread_to_core(m_core_id)) {
                std::cerr << "[Sentinel] Warning: Core affinity could not be established.\n";
            }
            if (!set_realtime_priority()) {
                std::cerr << "[Sentinel] Warning: Real-time priority could not be established.\n";
            }

            std::cout << "[Sentinel] Lattice-Guard active on Core " << m_core_id << "\n";

            while (m_running.load(std::memory_order_relaxed)) {
                // Read from the Photonics Laser Temp register (Offset 0x100).
                // Polling at sub-microsecond intervals for thermal drift.
                uint32_t current_temp = m_layout->laser_temp.load();

                if (current_temp > MAX_TEMP_MC) {
                    // EMERGENCY TRIP: Executes a hardware-level shutdown by writing 
                    // the trip code to the safety register.
                    m_layout->safety_trip.write(TRIP_CODE);
                    std::cerr << "!! [Sentinel] LATTICE DRIFT DETECTED: " << current_temp
                              << " mC. EMERGENCY TRIP EXECUTED.\n";
                    break;
                }

                // Memory barrier ensures the CPU doesn't reorder these safety checks
                // across the polling loop iterations.
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }
        });
    }

    /** @brief Stops the monitor thread and ensures graceful shutdown. */
    void stop() {
        m_running = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    /** @brief Destructor: Ensures the monitor is stopped before cleanup. */
    ~PorthSentinel() { stop(); }
};

} // namespace porth