/**
 * @file PorthDriver.hpp
 * @brief Orchestration layer for Newport Cluster hardware control and data planes.
 *
 * Porth-IO: The Sovereign Logic Layer
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "IPhysicsModel.hpp"
#include "PorthDeviceLayout.hpp"
#include "PorthPDK.hpp"
#include "PorthShuttle.hpp"
#include "PorthTelemetry.hpp"
#include "PorthUtil.hpp"
#include "PorthVFIODevice.hpp"
#include "StubPhysics.hpp"
#include <atomic>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace porth {

/** * @brief Default capacity for the DMA ring buffer.
 * Set to 1024 to balance memory pressure with throughput burst capabilities.
 */
constexpr size_t DEFAULT_RING_SIZE = 1024;

/**
 * @class Driver
 * @brief The high-level Master Driver for the Newport Cluster.
 *
 * This class encapsulates the HAL (Registers) and the Shuttle (Data Plane).
 * It manages the lifecycle of the memory fabric and automates the hardware
 * handshake during initialization.
 *
 * @tparam RingSize Depth of the DMA ring buffer.
 */
template <size_t RingSize = DEFAULT_RING_SIZE>
class Driver {
private:
    PorthDeviceLayout* m_regs;
    PorthVFIODevice* m_device_ptr{nullptr};

    /** @brief Reference to the physics model (Interface).
     * Defaults to StubPhysics if not provided by an Enterprise module.
     */
    IPhysicsModel* m_physics_model{nullptr};

    std::vector<std::unique_ptr<PorthShuttle<RingSize>>> m_shuttles;
    std::thread m_watchdog_thread;
    std::atomic<bool> m_run_watchdog{true};
    std::atomic<bool> m_watchdog_ready{false};

    uint32_t m_thermal_limit_mc;
    static constexpr uint64_t WATCHDOG_SLEEP_US = 20;
    PorthStats* m_stats{nullptr};
    bool m_strict{false}; // Sovereignty Guard: Forces HugePage perfection in the lab.

    void watchdog_loop() {
        auto* local_regs = m_regs;
        if (local_regs == nullptr)
            return;

        try {
            [[maybe_unused]] auto status = pin_thread_to_core(0);
        } catch (...) {
        }

        m_watchdog_ready.store(true, std::memory_order_release);

        while (m_run_watchdog.load(std::memory_order_acquire)) {
            const uint32_t temp = local_regs->laser_temp.load();
            const int32_t snr   = local_regs->rf_snr.load();

            if (m_stats != nullptr) {
                m_stats->current_temp_mc.store(temp, std::memory_order_relaxed);
                m_stats->current_snr_mdb.store(snr, std::memory_order_relaxed);
                if (temp > m_stats->max_temp_mc.load()) {
                    m_stats->max_temp_mc.store(temp, std::memory_order_relaxed);
                }
            }

            if (temp > m_thermal_limit_mc) {
                std::cerr << std::format("\n!! [Sovereign-Watchdog] THERMAL BREACH: {}mC. Halt.\n",
                                         temp)
                          << std::flush;
                local_regs->control.write(0x0);
                m_run_watchdog.store(false, std::memory_order_release);
                break;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(WATCHDOG_SLEEP_US));
        }
    }

    void initialize_shuttles(const PorthPDK& pdk, bool is_physical) {
        const uint32_t num_channels = pdk.get_num_channels();
        m_shuttles.reserve(num_channels);

        for (uint32_t i = 0; i < num_channels; ++i) {
            auto shuttle = std::make_unique<PorthShuttle<RingSize>>(0, m_strict);

            if (is_physical && m_device_ptr) {
                const uint64_t iova = m_device_ptr->map_dma(shuttle->get_raw_memory_ptr(),
                                                            shuttle->get_raw_memory_size());
                shuttle->set_device_iova(iova);
            }

            m_shuttles.push_back(std::move(shuttle));
            if (i == 0 && m_regs) {
                m_regs->data_ptr.write(m_shuttles[0]->get_device_addr());
            }
        }

        m_watchdog_thread = std::thread(&Driver::watchdog_loop, this);
        while (!m_watchdog_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::cout << std::format(
            "[Porth-Driver] Handshake Complete ({} Mode | Physics: {}). {} Shuttles active.\n",
            is_physical ? "Physical" : "Sim",
            m_physics_model->model_name(),
            m_shuttles.size());
    }

public:
    /**
     * @brief Physical Constructor: Orchestrates logic-to-physical handshake.
     */
    explicit Driver(PorthVFIODevice& device,
                    const PorthPDK& pdk,
                    IPhysicsModel* physics = nullptr,
                    bool strict            = true)
        : m_regs(device.view()), m_device_ptr(&device), m_strict(strict) {

        // Default to StubPhysics if no model provided
        static StubPhysics default_stub;
        m_physics_model = (physics != nullptr) ? physics : &default_stub;

        m_thermal_limit_mc = pdk.get_thermal_limit();
        device.validate_against_pdk(pdk);
        initialize_shuttles(pdk, true);
    }

    /**
     * @brief Simulation Constructor: Allows testing on Mac/Orbstack.
     */
    explicit Driver(PorthDeviceLayout* sim_regs,
                    const PorthPDK& pdk,
                    IPhysicsModel* physics = nullptr,
                    bool strict            = false)
        : m_regs(sim_regs), m_device_ptr(nullptr), m_strict(strict) {

        static StubPhysics default_stub;
        m_physics_model = (physics != nullptr) ? physics : &default_stub;

        m_thermal_limit_mc = pdk.get_thermal_limit();
        initialize_shuttles(pdk, false);
    }

    ~Driver() {
        if (m_regs != nullptr) {
            m_regs->data_ptr.write(0);
            if (m_device_ptr) {
                for (auto& shuttle : m_shuttles) {
                    m_device_ptr->unmap_dma(shuttle->get_device_addr(),
                                            shuttle->get_raw_memory_size());
                }
            }
        }

        m_run_watchdog.store(false, std::memory_order_release);
        if (m_watchdog_thread.joinable()) {
            m_watchdog_thread.join();
        }
    }

    void set_stats_link(PorthStats* stats) noexcept { m_stats = stats; }

    [[nodiscard]] auto transmit(const PorthDescriptor& desc, uint32_t channel_id = 0) noexcept
        -> PorthStatus {
        if (channel_id >= m_shuttles.size())
            return PorthStatus::FULL;
        if (m_shuttles[channel_id]->ring()->push(desc)) {
            if (m_stats != nullptr) {
                m_stats->total_packets.fetch_add(1, std::memory_order_relaxed);
                m_stats->total_bytes.fetch_add(desc.len, std::memory_order_relaxed);
            }
            return PorthStatus::SUCCESS;
        }
        if (m_stats != nullptr)
            m_stats->dropped_packets.fetch_add(1, std::memory_order_relaxed);
        return PorthStatus::FULL;
    }

    [[nodiscard]] auto receive(PorthDescriptor& out_desc, uint32_t channel_id = 0) noexcept
        -> PorthStatus {
        if (channel_id >= m_shuttles.size())
            return PorthStatus::EMPTY;
        return m_shuttles[channel_id]->ring()->pop(out_desc) ? PorthStatus::SUCCESS
                                                             : PorthStatus::EMPTY;
    }

    [[nodiscard]] auto get_regs() const noexcept -> PorthDeviceLayout* { return m_regs; }
    [[nodiscard]] auto get_shuttle(uint32_t channel_id = 0) const noexcept
        -> PorthShuttle<RingSize>* {
        if (channel_id >= m_shuttles.size())
            return nullptr;
        return m_shuttles[channel_id].get();
    }

    Driver(const Driver&)                    = delete;
    Driver(Driver&&)                         = delete;
    auto operator=(const Driver&) -> Driver& = delete;
    auto operator=(Driver&&) -> Driver&      = delete;
};

} // namespace porth