/**
 * @file main_demo.cpp
 * @brief Integrated showcase for hardware-software logic validation.
 *
 * Porth-IO: Low Latency Showcase
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
 * @brief Container for parsed CLI arguments to facilitate flexible deployment.
 */
struct DemoConfig {
    bool lab_mode = false;        ///< If true, attempts to claim physical VFIO hardware.
    std::string scenario_path;     ///< Path to a JSON-based simulation scenario.
    size_t iterations = 50000;    ///< Total cycles for the telemetry stress test.
    bool is_audit = false;        ///< Enables high-precision timing audits.
    int parking_duration = 60;    ///< Post-test hardware stabilization period (seconds).
};

/**
 * @brief Helper to parse command line arguments for the showcase environment.
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
 * * This function represents the "Hot Path." It bridges network signals from the 
 * AF_XDP portal to the hardware DMA shuttle and records end-to-end latency 
 * metrics with nanosecond precision.
 */
template <size_t Cap>
static void run_telemetry_stress_test(size_t iterations,
                                      bool is_audit,
                                      porth::PorthDeviceLayout* regs,
                                      porth::Driver<Cap>& driver,
                                      porth::PorthXDPPortal& xdp_portal,
                                      porth::PorthStats* telemetry_stats,
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

        // Bridge network signals into the zero-copy hardware memory fabric.
        xdp_portal.bridge_to_shuttle(*driver.get_shuttle(), telemetry_stats);

        const uint64_t t1 = PorthClock::now_precise();

        // Push a descriptor to the hardware-mapped DMA ring buffer.
        if (driver.transmit({.addr = test_addr, .len = test_len}) != PorthStatus::SUCCESS) {
            break;
        }

        if (!is_audit) {
            std::this_thread::sleep_for(packet_wait_us);
        } else {
            // High-precision spin-wait for audit scenarios to minimize scheduling jitter.
            const uint64_t spin_start = PorthClock::now_precise();
            while (PorthClock::now_precise() - spin_start < 50) { 
                porth::cpu_relax();
            }
        }

        // Simulate physical propagation delay across the Newport fabric.
        const uint64_t start_delay = PorthClock::now_precise();
        while (PorthClock::now_precise() - start_delay < propagation_delay_cycles) {
            porth::cpu_relax();
        }

        const uint64_t t2 = PorthClock::now_precise();
        metric.record(t2 - t1);

        // Synchronize real-time telemetry to the Shared Memory Hub for external dashboards.
        if (i % 10000 == 0) {
            const uint32_t current_temp = regs->laser_temp.load();
            if (current_temp > telemetry_stats->max_temp_mc.load()) {
                telemetry_stats->max_temp_mc.store(current_temp);
            }
            telemetry_stats->current_temp_mc.store(current_temp);
        }
    }
}

/** @brief Periodically updates telemetry stats during hardware stabilization. */
static void run_telemetry_parking(porth::PorthDeviceLayout* regs,
                                  porth::PorthStats* telemetry_stats,
                                  int duration_seconds) {
    for (int i = 0; i < duration_seconds; ++i) {
        telemetry_stats->current_temp_mc.store(regs->laser_temp.load());
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

/**
 * @brief Integrated entry point for the Porth-IO hardware-software showcase.
 */
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

    std::cout << "--- Porth-IO: Integrated Logic Showcase ---\n";

    try {
        // 1. Load the Physical Design Kit (PDK) manifest for hardware profiling.
        PorthPDK pdk;
        if (!pdk.load_manifest("configs/newport_default.json")) {
            throw std::runtime_error("Could not load PDK manifest.");
        }

        PorthDeviceLayout* regs = nullptr;
        std::unique_ptr<PorthSimDevice> sim;
        std::unique_ptr<PorthVFIODevice> physical_hw;
        std::unique_ptr<Driver<shuttle_size>> driver;
        auto physics = std::make_unique<PorthSimPHY>();

        // 2. Hardware Discovery: Search for Newport Cluster PCIe targets.
        auto pci_info = PorthHardwareScanner::find_target(pdk.get_vendor_id(), pdk.get_device_id());

        if (pci_info.has_value() && cfg.lab_mode) {
            // Lab Mode: Claim physical hardware ownership via VFIO.
            std::cout << "[System] Physical Newport Hardware detected. Initializing VFIO interconnect...\n";
            physical_hw = std::make_unique<PorthVFIODevice>(pci_info->to_string());
            physical_hw->validate_against_pdk(pdk);
            regs   = physical_hw->view();
            driver = std::make_unique<Driver<shuttle_size>>(regs, pdk, physics.get(), cfg.lab_mode);
            // Map the userspace HugePage memory into the IOMMU for hardware-visible DMA.
            std::ignore = physical_hw->map_dma(driver->get_shuttle()->get_raw_memory_ptr(),
                                               driver->get_shuttle()->get_raw_memory_size());
        } else {
            // Simulation Mode: Launch the high-fidelity Digital Twin.
            std::cout << "[System] Initializing Digital Twin Simulation Environment...\n";
            sim = std::make_unique<PorthSimDevice>("porth_newport_0", true);
            if (!cfg.scenario_path.empty()) {
                sim->load_scenario(cfg.scenario_path);
            }
            regs   = sim->view();
            driver = std::make_unique<Driver<shuttle_size>>(regs, pdk, physics.get(), cfg.lab_mode);
        }

        // 3. Network Ingress: Bind AF_XDP portal to the DMA memory fabric.
        PorthXDPPortal xdp_portal("lo", 0);
        xdp_portal.bind_shuttle_memory(*driver->get_shuttle());
        xdp_portal.open_portal();

        // 4. Thread Isolation: Establish scheduling sovereignty on dedicated cores.
        std::ignore = pin_thread_to_core(1);
        std::ignore = set_realtime_priority();

        // 5. Hardware Handshake: Execute power-on sequence.
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

        // 6. Observability: Link to the POSIX Shared Memory telemetry hub.
        PorthTelemetryHub telemetry_hub("porth_stats_0", true, cfg.lab_mode);
        driver->set_stats_link(telemetry_hub.view());

        // 7. Execution: Run high-frequency telemetry loops and record performance data.
        PorthMetric metric(metric_samples);
        std::cout << "[Driver] Executing " << metric_samples << " Zero-Copy cycles...\n";

        run_telemetry_stress_test(cfg.iterations, cfg.is_audit, regs, *driver, xdp_portal, telemetry_hub.view(), metric);

        std::cout << "\n[System] Test Complete. Stabilizing hardware state...\n";
        run_telemetry_parking(regs, telemetry_hub.view(), parking_duration_s);

        // 8. Analysis: Output nanosecond-scale jitter statistics.
        metric.print_stats(cycles_per_ns_newport);
        metric.save_markdown_report("BENCHMARKS.md", "End-to-End Integrated Telemetry (Host Validation)", cycles_per_ns_newport);

        std::cout << "[Success] Project validation complete.\n";

    } catch (const std::exception& e) {
        std::cerr << std::format("[Fatal] Logic Layer Exception: {}\n", e.what());
        return 1;
    }
    return 0;
}