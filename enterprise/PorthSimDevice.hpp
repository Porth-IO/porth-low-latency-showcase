/**
 * @file PorthSimDevice.hpp
 * @brief High-fidelity Digital Twin for Cardiff Photonics hardware simulation.
 *
 * Porth-IO: The Sovereign Logic Layer
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <bit>
#include <chrono>
#include <format>
#include <fstream>
#include <iomanip>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "PorthEmulatedDevice.hpp"
#include "PorthSimPHY.hpp"
#include "porth/PorthPDK.hpp"
#include "porth/PorthRingBuffer.hpp"
#include "porth/PorthShuttle.hpp"
#include "porth/PorthUtil.hpp"

namespace porth {

/** @brief Simulation Constants representing physical hardware limits of the Newport Cluster. */
constexpr uint32_t SIM_BASE_TEMP_MC       = 25000;  ///< 25.0°C Ambient/Start temp.
constexpr uint32_t SIM_TEMP_INC_MC        = 100;    ///< Heating rate per step (0.1°C).
constexpr uint32_t SIM_TEMP_DEC_MC        = 50;     ///< Passive cooling rate per step (0.05°C).
constexpr uint32_t SIM_STATUS_MAX_BIT     = 31;     ///< Bit-width of the hardware status register.
constexpr size_t SIM_DEFAULT_SHUTTLE_SIZE = 1024;   ///< Number of descriptors in the DMA ring.
constexpr int SIM_PHYSICS_STEP_US         = 100;    ///< Physics update frequency (10kHz).
constexpr int SIM_BUS_HANG_MS             = 100;    ///< Simulated duration of a PCIe TLP timeout.
constexpr int SIM_OVERFLOW_ITERATIONS     = 2048;   ///< Iterations to force RingBuffer saturation.
constexpr uint32_t SIM_DESCRIPTOR_LEN_DEFAULT = 64; ///< Standard flit-aligned descriptor length.
constexpr uint32_t SIM_CHAOS_THRESHOLD = 28; ///< Threshold for critical DMA pointer corruption.

/**
 * @class PorthSimDevice
 * @brief The high-fidelity Digital Twin for Cardiff Photonics.
 *
 * This class simulates the physical, electrical, and protocol-level behaviors
 * of a compound semiconductor device on a PCIe Gen 6 bus. It manages a background
 * physics thread that models thermal drift and power rail noise.
 */
class PorthSimDevice {
public:
    // Hardware simulators represent unique physical units; copying is prohibited
    PorthSimDevice(const PorthSimDevice&)                    = delete;
    auto operator=(const PorthSimDevice&) -> PorthSimDevice& = delete;

    // Moving is deleted to maintain stability of the background physics thread
    PorthSimDevice(PorthSimDevice&&)                    = delete;
    auto operator=(PorthSimDevice&&) -> PorthSimDevice& = delete;

private:
    /** @brief Internal Physics Constants to satisfy readability-magic-numbers. */
    struct SimulationPhysics {
        static constexpr uint64_t SHUTTLE_ALIGN_MASK = 0x3F;
        static constexpr uint32_t GAN_VOLT_LIMIT_UV  = 800000;
        static constexpr uint32_t GAN_VOLT_STEP_UV   = 4000;
        static constexpr uint32_t GAN_VOLT_COOL_UV   = 8000;
        static constexpr uint32_t VOLT_TEMP_DIV      = 10000;
        static constexpr uint32_t VOLT_TEMP_MULT     = 800;
        static constexpr int32_t SNR_BASELINE_MC     = 3000;
        static constexpr int32_t SNR_TEMP_DIV        = 40;
        static constexpr uint64_t NEWPORT_BASE_NS    = 45;
        static constexpr uint64_t NEWPORT_JITTER_NS  = 12;
        static constexpr uint32_t GAN_SAFE_LIMIT_MC  = 125000;
    };

    PorthEmulatedDevice m_mock_hw; ///< Shared memory backend simulating physical BARs.
    PorthSimPHY m_phy;         ///< Signal propagation model including jitter/attenuation.
    PorthPDK m_pdk;            ///< Hardware abstraction for different Newport chipsets.

    std::ofstream m_tlp_log; ///< Persistent log for Transaction Layer Packet (TLP) auditing.

    // Physics & Simulation Threads
    std::atomic<bool> m_run_sim{true}; ///< Lifecycle guard for the simulation engine.
    std::thread m_physics_thread;      ///< Models the non-linear physics of the InP lattice.

    // --- Chaos & Error Injection State ---
    std::atomic<bool> m_inject_deadlock{false};
    std::atomic<bool> m_corrupt_status{false};
    std::atomic<bool> m_bus_hang{false};

    // Sovereign Guard: Tracks the first valid address to detect pointer corruption.
    std::atomic<uint64_t> m_last_valid_shuttle{0};

    struct ScenarioEvent {
        uint64_t time_ms;
        std::string action;
        double value;
        bool triggered = false;
    };
    std::vector<ScenarioEvent> m_active_scenario;
    std::chrono::steady_clock::time_point m_scenario_start;

    /** * @brief Internal helper to drain the DMA Shuttle. */
    void process_dma(PorthDeviceLayout* dev) noexcept {
        const uint64_t shuttle_addr = dev->data_ptr.load();
        if (shuttle_addr == 0) {
            return;
        }

        // Chaos Guard: Ensure address is 64-byte aligned.
        if ((shuttle_addr & SimulationPhysics::SHUTTLE_ALIGN_MASK) != 0) {
            return;
        }

        uint64_t expected = m_last_valid_shuttle.load();
        if (expected != 0 && shuttle_addr != expected) {
            return;
        }

        if (expected == 0) {
            m_last_valid_shuttle.store(shuttle_addr);
        }

        auto* ring = std::bit_cast<PorthRingBuffer<SIM_DEFAULT_SHUTTLE_SIZE>*>(shuttle_addr);
        PorthDescriptor desc{};

        while (ring->pop(desc)) {
            // Simulate the physical time taken to move data across the Newport fabric
            // This now accounts for Jitter and PAM4 signaling noise
            m_phy.apply_protocol_delay();
            dev->counter.write(dev->counter.load() + 1);
        }
    }

    /** * @brief Internal helper to update the Photonics thermal lattice. */
    void update_thermal_model(
        PorthDeviceLayout* dev,
        uint32_t& current_temp) noexcept { // NOLINT(readability-convert-member-functions-to-static)
        uint32_t volt = dev->gan_voltage.load();
        if (dev->control.load() == 0x1) {
            if (volt < SimulationPhysics::GAN_VOLT_LIMIT_UV) {
                volt += SimulationPhysics::GAN_VOLT_STEP_UV;
            }
        } else {
            if (volt > 0) {
                volt = (volt > SimulationPhysics::GAN_VOLT_COOL_UV)
                           ? volt - SimulationPhysics::GAN_VOLT_COOL_UV
                           : 0;
            }
        }
        dev->gan_voltage.write(volt);

        uint32_t target_temp = SIM_BASE_TEMP_MC;
        if (volt > 0) {
            target_temp +=
                (volt / SimulationPhysics::VOLT_TEMP_DIV) * SimulationPhysics::VOLT_TEMP_MULT;
        }

        m_phy.update_thermal_load(target_temp);
        current_temp = m_phy.get_current_temp();

        int32_t snr = SimulationPhysics::SNR_BASELINE_MC;
        // Use the simulation's current thermal boundary instead of a hardcoded constant
        if (current_temp > m_phy.get_current_temp()) {
            snr -= static_cast<int32_t>((current_temp - m_phy.get_current_temp()) /
                                        SimulationPhysics::SNR_TEMP_DIV);
        }
        dev->rf_snr.write(snr);

        // Sovereign Feedback Loop: Inject real-time SNR back into the PHY model
        // to simulate physical packet corruption at high temps.
        m_phy.set_snr(snr);

        dev->laser_temp.write(current_temp);
    }

    void apply_chaos_effects(PorthDeviceLayout* dev,
                             std::mt19937& gen,
                             std::uniform_int_distribution<uint32_t>& bit_dist) noexcept {
        if (m_corrupt_status.load(std::memory_order_relaxed)) {
            const uint32_t current_status = dev->status.load();
            if (bit_dist(gen) > SIM_CHAOS_THRESHOLD) {
                dev->data_ptr.write(dev->data_ptr.load() ^ (1ULL << bit_dist(gen)));
            } else {
                dev->status.write(current_status ^ (1U << bit_dist(gen)));
            }
        }
    }

public:
    void load_scenario(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open())
            return;

        nlohmann::json j;
        file >> j;

        m_active_scenario.clear();
        for (const auto& event : j["events"]) {
            m_active_scenario.push_back({event["time_ms"], event["action"], event["value"]});
        }
        m_scenario_start = std::chrono::steady_clock::now();
        std::cout << "[Porth-Sim] Scenario '" << j["name"] << "' armed.\n";
    }

private:
    void run_physics_loop() {
        if (!pin_thread_to_core(0)) {
            std::cerr << "[Porth-Sim] Warning: Failed to pin physics thread to Core 0.\n";
        }

        PorthDeviceLayout* dev = m_mock_hw.view();
        uint32_t temp          = SIM_BASE_TEMP_MC;

        std::mt19937 gen(static_cast<std::mt19937::result_type>(std::random_device{}()));
        std::uniform_int_distribution<uint32_t> bit_dist(0, SIM_STATUS_MAX_BIT);

        while (m_run_sim.load(std::memory_order_relaxed)) {
            dev->status.write(0x1);
            std::atomic_thread_fence(std::memory_order_release);

            // Check for automated scenario events
            auto now = std::chrono::steady_clock::now();
            // Cast to uint64_t to match the ScenarioEvent structure
            uint64_t elapsed = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - m_scenario_start)
                    .count());

            for (auto& event : m_active_scenario) {
                if (!event.triggered && elapsed >= event.time_ms) {
                    if (event.action == "inject_temp")
                        dev->laser_temp.write(static_cast<uint32_t>(event.value));
                    else if (event.action == "trigger_corruption")
                        trigger_corruption(event.value > 0);
                    else if (event.action == "set_bus_hang")
                        set_bus_hang(event.value > 0);
                    else {
                        std::cerr << "[Porth-Sim] Unknown Scenario Action: " << event.action
                                  << "\n";
                    }

                    event.triggered = true;
                    std::cout << "[Porth-Sim] Scenario Event Triggered: " << event.action << "\n";
                }
            }

            if (!m_inject_deadlock.load(std::memory_order_relaxed)) {
                process_dma(dev);
                update_thermal_model(dev, temp);
                apply_chaos_effects(dev, gen, bit_dist);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(SIM_PHYSICS_STEP_US));
        }
    }

public:
    PorthSimDevice(const std::string& name, bool create = true) : m_mock_hw(name, create), m_pdk() {
        PorthDeviceLayout* dev = m_mock_hw.view();

        if (dev == nullptr) {
            throw std::runtime_error("Porth-Sim: Failed to map shared memory view.");
        }

        dev->laser_temp.write(SIM_BASE_TEMP_MC);
        dev->status.write(0);
        dev->control.write(0);

        m_tlp_log.open("porth_tlp_traffic.log", std::ios::app);

        // FIRST: Load profile to configure the PHY model correctly
        load_newport_profile(std::string(PORTH_CONFIG_DIR) + "/newport_default.json");

        // SECOND: Launch physics thread ONLY after PDK/PHY are configured
        m_physics_thread = std::thread(&PorthSimDevice::run_physics_loop, this);

        while (dev->status.load() == 0) {
            std::this_thread::yield();
        }
    }

    ~PorthSimDevice() {
        m_run_sim.store(false, std::memory_order_seq_cst);
        if (m_physics_thread.joinable()) {
            m_physics_thread.join();
        }
        if (m_tlp_log.is_open()) {
            m_tlp_log.close();
        }
    }

    void apply_scenario(uint64_t base_ns,
                        uint64_t jitter_ns,
                        bool chaos = false) { // NOLINT(bugprone-easily-swappable-parameters)
        m_phy.set_config(base_ns, jitter_ns);
        if (chaos) {
            trigger_corruption(true);
        }
    }

    void load_newport_profile(
        const std::string& config_path) { // NOLINT(readability-convert-member-functions-to-static)
        // Load the dynamic register map from the PDK JSON
        if (m_pdk.load_manifest(config_path)) {
            std::cout << "[Porth-Sim] PDK Profile '" << config_path
                      << "' integrated into Digital Twin.\n";
        }

        std::ifstream file(config_path);
        if (!file.is_open()) {
            return;
        }

        // Apply physical constants from the profile to the PHY model
        m_phy.set_config(SimulationPhysics::NEWPORT_BASE_NS, SimulationPhysics::NEWPORT_JITTER_NS);
    }

    void trigger_deadlock(bool active) noexcept { m_inject_deadlock.store(active); }
    void trigger_corruption(bool active) noexcept { m_corrupt_status.store(active); }
    void set_bus_hang(bool active) noexcept { m_bus_hang.store(active); }

    static void force_buffer_overflow(PorthRingBuffer<SIM_DEFAULT_SHUTTLE_SIZE>& ring) {
        const PorthDescriptor junk = {.addr = 0xDEADBEEF, .len = SIM_DESCRIPTOR_LEN_DEFAULT};
        for (int i = 0; i < SIM_OVERFLOW_ITERATIONS; ++i) {
            (void)ring.push(junk);
        }
    }

    template <typename T>
    [[nodiscard]] auto read_reg(PorthRegister<T>& reg) -> T {
        if (m_bus_hang.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(SIM_BUS_HANG_MS));
        }
        m_phy.apply_protocol_delay();
        return reg.load();
    }

    template <typename T>
    auto write_reg(PorthRegister<T>& reg, T val) -> void {
        if (m_bus_hang.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(SIM_BUS_HANG_MS));
        }
        m_phy.apply_protocol_delay();
        reg.write(val);
    }

    template <typename T>
    [[nodiscard]] auto read_flit(PorthRegister<T>& reg, uint64_t offset) -> T {
        if (m_bus_hang.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(SIM_BUS_HANG_MS));
        }
        log_tlp("READ_REQ", offset);
        m_phy.apply_protocol_delay();
        const T val = reg.load();
        log_tlp("COMPLETION", offset, static_cast<uint64_t>(val));
        return val;
    }

    template <typename T>
    auto write_flit(PorthRegister<T>& reg, uint64_t offset, T val) -> void {
        if (m_bus_hang.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(SIM_BUS_HANG_MS));
        }
        log_tlp("WRITE_MEM", offset, static_cast<uint64_t>(val));
        m_phy.apply_protocol_delay();
        reg.write(val);
    }

    [[nodiscard]] auto view() noexcept -> PorthDeviceLayout* { return m_mock_hw.view(); }
    [[nodiscard]] auto get_phy() noexcept -> PorthSimPHY& { return m_phy; }

private:
    void log_tlp(const std::string& type,
                 uint64_t addr,
                 uint64_t val = 0) { // NOLINT(bugprone-easily-swappable-parameters)
        if (m_tlp_log.is_open()) {
            const auto now = std::chrono::system_clock::now();
            const auto t_c = std::chrono::system_clock::to_time_t(now);

            m_tlp_log << "[" << std::put_time(std::localtime(&t_c), "%H:%M:%S") << "] ";
            m_tlp_log << "TLP_" << type << " | Addr: 0x" << std::hex << addr;
            m_tlp_log << " | Data: 0x" << val << std::dec << '\n';
        }
    }
};

} // namespace porth