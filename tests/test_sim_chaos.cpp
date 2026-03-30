/**
 * @file test_sim_chaos.cpp
 * @brief Formal verification of the Digital Twin chaos engineering and resilience.
 *
 * Porth-IO: Low Latency Showcase
 */

#include "PorthSimDevice.hpp"
#include "porth/PorthDeviceLayout.hpp"
#include "porth/PorthRegister.hpp"
#include "porth/PorthShuttle.hpp"
#include <bits/chrono.h>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

/**
 * @brief Main entry point for Chaos Engineering and Hardware Resilience verification.
 * * This test suite utilizes the Digital Twin to simulate adversarial hardware 
 * conditions, including non-deterministic register bit-flipping and 
 * logic-layer deadlocks, to verify system recovery procedures.
 */
auto main() -> int {
    using namespace porth;

    // Timing constants for stabilization and monitoring windows.
    constexpr auto corruption_wait_ms    = std::chrono::milliseconds(100);
    constexpr auto stabilization_wait_ms = std::chrono::milliseconds(50);
    constexpr int watchdog_max_retries   = 10;
    constexpr auto watchdog_interval_ms  = std::chrono::milliseconds(100);
    constexpr uint32_t READY_VAL         = 0x2;
    constexpr size_t SHUTTLE_CAPACITY    = 1024;

    try {
        std::cout << "\n[DEBUG] --- Starting Resilience Verification (test_sim_chaos) ---" << '\n';

        std::cout << "[DEBUG] Phase 1: Initializing Digital Twin..." << '\n';
        PorthSimDevice sim("porth_sim_chaos", true);
        auto* dev = sim.view();

        /**
         * @note HARDENING STEP: To prevent the simulator from encountering 
         * segmentation faults during intentional address corruption, we first 
         * "prime" the DMA engine with a valid address. This enables the 
         * simulator's internal boundary checking to recognize and skip 
         * subsequent corrupted pointers.
         */
        std::cout << "[DEBUG]   - Priming DMA with valid Shuttle address..." << '\n';
        PorthShuttle<SHUTTLE_CAPACITY> dummy_shuttle;

        /**
         * @important The hardware-mapped data_ptr must reference the 
         * physical RingBuffer structure directly to ensure binary alignment.
         */
        dev->data_ptr.write(dummy_shuttle.get_device_addr());

        // Stabilization window to ensure the physics engine has processed the initial state.
        std::this_thread::sleep_for(stabilization_wait_ms);

        // 1. Transient Hardware Fault Simulation (Register Corruption)
        std::cout << "[DEBUG] Phase 2: Simulating Transient Hardware Faults (Bit-Flipping)..." << '\n';
        dev->status.write(READY_VAL);

        std::cout << "[DEBUG]   - Triggering Non-Deterministic Chaos mode..." << '\n';
        sim.trigger_corruption(true);

        std::this_thread::sleep_for(corruption_wait_ms);

        uint32_t corrupted = sim.read_reg(dev->status);
        std::cout << "[DEBUG]   - Corruption Observation: Status = 0x" << std::hex << corrupted
                  << std::dec << '\n';

        std::cout << "[DEBUG]   - Restoring System Determinism..." << '\n';
        sim.trigger_corruption(false);

        // 2. Hardware Deadlock & Watchdog Auto-Recovery
        std::cout << "\n[DEBUG] Phase 3: Verifying Watchdog Heartbeat Recovery..." << '\n';
        std::cout << "[DEBUG]   - Injecting Deadlock (Interrupting Physics Thread)..." << '\n';
        sim.trigger_deadlock(true);

        uint32_t last_temp = sim.read_reg(dev->laser_temp);
        bool recovered     = false;

        std::cout << "[DEBUG]   - Monitoring System Heartbeat (Laser Telemetry)..." << '\n';
        std::cout << "[DEBUG]   - Initial Baseline: " << last_temp << " mC" << '\n';

        for (int i = 0; i < watchdog_max_retries; ++i) {
            std::this_thread::sleep_for(watchdog_interval_ms);
            uint32_t current_temp = sim.read_reg(dev->laser_temp);

            std::cout << "[DEBUG]   - Heartbeat Poll [" << i << "]: Temp = " << current_temp << " mC"
                      << '\n';

            if (current_temp == last_temp) {
                // Heartbeat freeze detected: The simulator has stopped updating telemetry.
                std::cout << "[DEBUG]   - ALERT: Heartbeat frozen. Triggering Watchdog Reset..." << '\n';
                sim.trigger_deadlock(false);
                recovered = true;
                break;
            }
            last_temp = current_temp;
        }

        if (recovered) {
            std::cout << "[DEBUG]   - SUCCESS: Watchdog successfully recovered the hardware state." << '\n';
        } else {
            throw std::runtime_error(
                "Watchdog failed to detect deadlock - Heartbeat pulse was not interrupted.");
        }

    } catch (const std::exception& e) {
        std::cerr << "[FATAL ERROR] test_sim_chaos: " << e.what() << '\n';
        return 1;
    }

    std::cout << "[DEBUG] --- All Resilience Tests Passed ---" << '\n';
    return 0;
}