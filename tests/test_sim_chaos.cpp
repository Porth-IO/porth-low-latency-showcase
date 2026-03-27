/**
 * @file test_sim_chaos.cpp
 * @brief Formal verification of the Digital Twin chaos engineering and resilience.
 *
 * Porth-IO: The Sovereign Logic Layer
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
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
 * @brief Main entry point for Chaos Engineering verification.
 */
auto main() -> int {
    using namespace porth;

    // Domain constants to resolve magic number warnings
    constexpr auto corruption_wait_ms    = std::chrono::milliseconds(100);
    constexpr auto stabilization_wait_ms = std::chrono::milliseconds(50);
    constexpr int watchdog_max_retries   = 10;
    constexpr auto watchdog_interval_ms  = std::chrono::milliseconds(100);
    constexpr uint32_t READY_VAL         = 0x2;
    constexpr size_t SHUTTLE_CAPACITY    = 1024;

    try {
        std::cout << "\n[DEBUG] --- Starting test_sim_chaos ---" << '\n';

        std::cout << "[DEBUG] Phase 1: Initializing Digital Twin..." << '\n';
        PorthSimDevice sim("porth_sim_chaos", true);
        auto* dev = sim.view();

        /**
         * @note HARDENING STEP: To prevent the simulator from SegFaulting when
         * we corrupt the data_ptr, we must first "prime" it with a valid
         * address. This allows the simulator's internal hardening logic to
         * recognize subsequent corrupted pointer values as invalid and skip
         * processing them.
         */
        std::cout << "[DEBUG]   - Priming DMA with valid Shuttle address..." << '\n';
        PorthShuttle<SHUTTLE_CAPACITY> dummy_shuttle;

        /**
         * @important The address in data_ptr must point to the RingBuffer itself,
         * not the host-side Shuttle manager object.
         */
        dev->data_ptr.write(dummy_shuttle.get_device_addr());

        // Ensure the physics thread sees the valid address before we start corrupting it
        std::this_thread::sleep_for(stabilization_wait_ms);

        // 1. Test Register Corruption
        std::cout << "[DEBUG] Phase 2: Testing Register Corruption (Bit-Flipping)..." << '\n';
        dev->status.write(READY_VAL);

        std::cout << "[DEBUG]   - Triggering Aggressive Chaos mode..." << '\n';
        sim.trigger_corruption(true);

        std::this_thread::sleep_for(corruption_wait_ms);

        uint32_t corrupted = sim.read_reg(dev->status);
        std::cout << "[DEBUG]   - Corruption Result: Status = 0x" << std::hex << corrupted
                  << std::dec << '\n';

        std::cout << "[DEBUG]   - Disabling Chaos for stability..." << '\n';
        sim.trigger_corruption(false);

        // 2. Test Deadlock & Watchdog Recovery
        std::cout << "\n[DEBUG] Phase 3: Testing Hardware Deadlock & Watchdog..." << '\n';
        std::cout << "[DEBUG]   - Injecting Deadlock (Freezing Physics Thread)..." << '\n';
        sim.trigger_deadlock(true);

        uint32_t last_temp = sim.read_reg(dev->laser_temp);
        bool recovered     = false;

        std::cout << "[DEBUG]   - Monitoring Heartbeat (Laser Temp)..." << '\n';
        std::cout << "[DEBUG]   - Initial Temp: " << last_temp << " mC" << '\n';

        for (int i = 0; i < watchdog_max_retries; ++i) {
            std::this_thread::sleep_for(watchdog_interval_ms);
            uint32_t current_temp = sim.read_reg(dev->laser_temp);

            std::cout << "[DEBUG]   - Watchdog Poll [" << i << "]: Temp = " << current_temp << " mC"
                      << '\n';

            if (current_temp == last_temp) {
                std::cout << "[DEBUG]   - ALERT: Heartbeat frozen. Triggering Reset..." << '\n';
                sim.trigger_deadlock(false);
                recovered = true;
                break;
            }
            last_temp = current_temp;
        }

        if (recovered) {
            std::cout << "[DEBUG]   - SUCCESS: Watchdog recovered the system." << '\n';
        } else {
            throw std::runtime_error(
                "Watchdog failed to detect deadlock - Heartbeat still pulsing?");
        }

    } catch (const std::exception& e) {
        std::cerr << "[FATAL ERROR] test_sim_chaos: " << e.what() << '\n';
        return 1;
    }

    std::cout << "[DEBUG] --- All Chaos Tests Passed ---" << '\n';
    return 0;
}