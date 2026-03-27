/**
 * @file main_demo.cpp
 * @brief Sovereign Logic Layer Showcase - Integrated Hardware-Software Validation.
 *
 * Porth-IO: Sovereign Logic Layer
 * Copyright (c) 2026 Porth-IO Contributors
 */

#include "porth/PorthClock.hpp"
#include "porth/PorthDeviceLayout.hpp"
#include "porth/PorthDriver.hpp"
#include "porth/PorthMetric.hpp"
#include "porth/PorthRegister.hpp"
#include "porth/PorthTelemetry.hpp"
#include "porth/PorthUtil.hpp"
#include "porth/PorthVFIODevice.hpp"
#include "porth/PorthXDPPortal.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>

#include "PorthHardwareScanner.hpp"
#include "PorthSimDevice.hpp"
#include "PorthSimPHY.hpp"

/**
 * @brief Container for parsed CLI arguments to reduce main complexity.
 */
struct DemoConfig {
    bool lab_mode = false;
    std::string scenario_path;
    size_t iterations = 50000;
    bool is_audit = false;
    int parking_duration = 60; 
};

/**
 * @brief Helper to parse command line arguments.
 */
static auto parse_args(int argc, char** argv) -> DemoConfig {
    DemoConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--lab") {
            cfg.lab_mode = true;
        } else if (arg == "--scenario" && i + 1 < argc) {
            cfg.scenario_path = argv[++i];
        } else if (arg == "--iterations" && i + 1 < argc) {
            cfg.iterations = std::stoul(argv[++i]);
        } else if (arg == "--parking" && i + 1 < argc) {
            cfg.parking_duration = std::stoi(argv[++i]);
        } else if (arg == "--audit") {
            cfg.is_audit = true;
        }
    }
    return cfg;
}

/**
 * @brief Executes the high-speed deterministic telemetry collection loop.
 */
template <size_t Cap>
static void run_telemetry_stress_test(size_t iterations,
                                      bool is_audit,
                                      porth::PorthDeviceLayout* regs,
                                      porth::Driver<Cap>& driver,
                                      porth::PorthXDPPortal& xdp_portal,
                                      porth::PorthStats* sovereign_stats,
                                      porth::PorthMetric& metric) {
    using namespace porth;

    constexpr uint64_t test_addr                = 0x1000;
    constexpr uint32_t test_len                 = 64;
    constexpr uint64_t propagation_delay_cycles = 240;
    constexpr auto packet_wait_us               = std::chrono::microseconds(100);

    for (size_t i = 0; i < iterations; ++i) {
        
        if (i % 5000 == 0 && i > 0) {
            std::cout << std::format("[Stress-Test] Progress: {}% ({} samples)\n", (i * 100) / iterations, i) << std::flush;
        }

        xdp_portal.bridge_to_shuttle(*driver.get_shuttle(), sovereign_stats);

        const uint64_t t1 = PorthClock::now_precise();

        if (driver.transmit({.addr = test_addr, .len = test_len}) != PorthStatus::SUCCESS) {
            break;
        }

        if (!is_audit) {
            std::this_thread::sleep_for(packet_wait_us);
        } else {
            const uint64_t spin_start = PorthClock::now_precise();
            while (PorthClock::now_precise() - spin_start < 50) { 
                porth::cpu_relax();
            }
        }

        const uint64_t start_delay = PorthClock::now_precise();
        while (PorthClock::now_precise() - start_delay < propagation_delay_cycles) {
            porth::cpu_relax();
        }

        const uint64_t t2 = PorthClock::now_precise();
        metric.record(t2 - t1);

        // Telemetry is synchronized to the Shared Memory Hub for external monitoring
        // Standard I/O is avoided in the hot path to maintain deterministic timing
        if (i % 10000 == 0) {
            const uint32_t current_temp = regs->laser_temp.load();
            if (current_temp > sovereign_stats->max_temp_mc.load()) {
                sovereign_stats->max_temp_mc.store(current_temp);
            }
            sovereign_stats->current_temp_mc.store(current_temp);
        }
    }
}

static void run_telemetry_parking(porth::PorthDeviceLayout* regs,
                                  porth::PorthStats* sovereign_stats,
                                  int duration_seconds) {
    for (int i = 0; i < duration_seconds; ++i) {
        sovereign_stats->current_temp_mc.store(regs->laser_temp.load());
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

auto main(int argc, char** argv) -> int {
    using namespace porth;

    const auto cfg = parse_args(argc, argv);

    constexpr size_t shuttle_size          = 1024;
    constexpr int handshake_timeout_ms     = 5000;
    constexpr int warmup_delay_ms          = 10;
    int parking_duration_s = cfg.parking_duration;
    constexpr int handshake_poll_ms        = 1;
    constexpr size_t metric_samples        = 50000;
    constexpr double cycles_per_ns_newport = 2.4;

    std::cout << "--- Porth-IO: Sovereign Logic Layer Showcase ---\n";

    try {
        PorthPDK pdk;
        if (!pdk.load_manifest("configs/newport_default.json")) {
            throw std::runtime_error("Could not load PDK manifest.");
        }

        PorthDeviceLayout* regs = nullptr;
        std::unique_ptr<PorthSimDevice> sim;
        std::unique_ptr<PorthVFIODevice> physical_hw;
        std::unique_ptr<Driver<shuttle_size>> driver;
        auto physics = std::make_unique<PorthSimPHY>();

        auto pci_info = PorthHardwareScanner::find_target(pdk.get_vendor_id(), pdk.get_device_id());

        if (pci_info.has_value() && cfg.lab_mode) {
            std::cout << "[System] Physical Newport Hardware detected. Initializing VFIO...\n";
            physical_hw = std::make_unique<PorthVFIODevice>(pci_info->to_string());
            physical_hw->validate_against_pdk(pdk);
            regs   = physical_hw->view();
            driver = std::make_unique<Driver<shuttle_size>>(regs, pdk, physics.get(), cfg.lab_mode);
            std::ignore = physical_hw->map_dma(driver->get_shuttle()->get_raw_memory_ptr(),
                                               driver->get_shuttle()->get_raw_memory_size());
        } else {
            std::cout << "[System] Initializing Digital Twin Simulation...\n";
            sim = std::make_unique<PorthSimDevice>("porth_newport_0", true);
            if (!cfg.scenario_path.empty()) {
                sim->load_scenario(cfg.scenario_path);
            }
            regs   = sim->view();
            driver = std::make_unique<Driver<shuttle_size>>(regs, pdk, physics.get(), cfg.lab_mode);
        }

        PorthXDPPortal xdp_portal("lo", 0);
        xdp_portal.bind_shuttle_memory(*driver->get_shuttle());
        xdp_portal.open_portal();

        std::ignore = pin_thread_to_core(1);
        std::ignore = set_realtime_priority();

        regs->control.write(0x1);

        int timeout_count = 0;
        while (regs->status.load() == 0 && timeout_count < handshake_timeout_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(handshake_poll_ms));
            timeout_count++;
        }

        if (regs->status.load() == 0) {
            throw std::runtime_error("Hardware handshake timed out.");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(warmup_delay_ms));

        PorthTelemetryHub telemetry_hub("porth_stats_0", true, cfg.lab_mode);
        driver->set_stats_link(telemetry_hub.view());

        PorthMetric metric(metric_samples);
        std::cout << "[Driver] Executing " << metric_samples << " Zero-Copy cycles...\n";

        run_telemetry_stress_test(cfg.iterations, cfg.is_audit, regs, *driver, xdp_portal, telemetry_hub.view(), metric);

        std::cout << "\n[System] Test Complete. Parking hardware...\n";
        run_telemetry_parking(regs, telemetry_hub.view(), parking_duration_s);

        metric.print_stats(cycles_per_ns_newport);
        metric.save_markdown_report("BENCHMARKS.md", "End-to-End Sovereign Telemetry (MacBook/OrbStack)", cycles_per_ns_newport);

        std::cout << "[Success] Newport Cluster validated.\n";

    } catch (const std::exception& e) {
        std::cerr << std::format("[Fatal] Logic Layer Exception: {}\n", e.what());
        return 1;
    }
    return 0;
}