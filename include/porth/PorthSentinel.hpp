/**
 * @file PorthSentinel.hpp
 * @brief Isolated background monitor for physical hardware safety.
 *
 * Porth-IO: The Sovereign Logic Layer
 */

#pragma once

#include "PorthDeviceLayout.hpp"
#include "PorthUtil.hpp"
#include <atomic>
#include <iostream>
#include <thread>

namespace porth {

class PorthSentinel {
private:
    PorthDeviceLayout* m_layout;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    int m_core_id;

    // Safety Thresholds based on Porth PDK limits
    // Max Laser Temp: 45,000 mC (45C)
    static constexpr uint32_t MAX_TEMP_MC = 45000;
    static constexpr uint32_t TRIP_CODE   = 0xDEADBEEF;

public:
    explicit PorthSentinel(PorthDeviceLayout* layout, int core_id = 1)
        : m_layout(layout), m_core_id(core_id) {}

    // Sentinel threads represent unique physical monitoring resources; copying/moving is
    // prohibited.
    PorthSentinel(const PorthSentinel&)                    = delete;
    auto operator=(const PorthSentinel&) -> PorthSentinel& = delete;
    PorthSentinel(PorthSentinel&&)                         = delete;
    auto operator=(PorthSentinel&&) -> PorthSentinel&      = delete;

    void start() {
        m_running = true;
        m_thread  = std::thread([this]() {
            // Establish Sovereignty on the isolated core.
            // Explicitly checking the return value satisfies clang-tidy
            // bugprone-unused-return-value.
            if (!pin_thread_to_core(m_core_id)) {
                std::cerr << "[Sentinel] Warning: Core affinity could not be established.\n";
            }
            if (!set_realtime_priority()) {
                std::cerr << "[Sentinel] Warning: Real-time priority could not be established.\n";
            }

            std::cout << "[Sentinel] Lattice-Guard active on Core " << m_core_id << "\n";

            while (m_running.load(std::memory_order_relaxed)) {
                // Read from the Photonics Laser Temp register (Offset 0x100)
                uint32_t current_temp = m_layout->laser_temp.load();

                if (current_temp > MAX_TEMP_MC) {
                    // EMERGENCY TRIP: Sub-microsecond reaction
                    m_layout->safety_trip.write(TRIP_CODE);
                    std::cerr << "!! [Sentinel] LATTICE DRIFT DETECTED: " << current_temp
                              << " mC. EMERGENCY TRIP EXECUTED.\n";
                    break;
                }

                // Ensure CPU doesn't reorder these safety checks
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }
        });
    }

    void stop() {
        m_running = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    ~PorthSentinel() { stop(); }
};

} // namespace porth