/**
 * @file PorthDriver.hpp
 * @brief Orchestration layer for Newport Cluster hardware control and data planes.
 *
 * Porth-IO: Low Latency Showcase
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
 * @brief High-level orchestration engine for the Newport Cluster hardware.
 *
 * This class encapsulates the Hardware Abstraction Layer (HAL) and the DMA 
 * data plane. It manages the lifecycle of the memory fabric, thread isolation, 
 * and automates the hardware-software handshake protocols.
 *
 * @tparam RingSize Depth of the DMA ring buffer.
 */
template <size_t RingSize = DEFAULT_RING_SIZE>
class Driver {
private:
    PorthDeviceLayout* m_regs;
    PorthVFIODevice* m_device_ptr{nullptr};

    /** @brief Reference to the physics-based behavioral model.
     * Defaults to StubPhysics if no high-fidelity model is provided.
     */
    IPhysicsModel* m_physics_model{nullptr};

    std::vector<std::unique_ptr<PorthShuttle<RingSize>>> m_shuttles;
    std::thread m_watchdog_thread;
    std::atomic<bool> m_run_watchdog{true};
    std::atomic<bool> m_watchdog_ready{false};

    uint32_t m_thermal_limit_mc;
    static constexpr uint64_t WATCHDOG_SLEEP_US = 20;
    PorthStats* m_stats{nullptr};
    bool m_strict{false}; // Integrity Guard: Enforces deterministic memory allocation.

    /** * @brief Asynchronous safety monitor. 
     * Polls hardware telemetry (temperature and SNR) and executes an 
     * emergency halt if the PDK-defined physical boundaries are breached.
     */
    void watchdog_loop() {
        auto* local_regs = m_regs;
        if (local_regs == nullptr)
            return;

        try {
            // Isolates the safety monitor to a dedicated core to prevent
            // interference with the primary data plane.
            [[maybe_unused]] auto status = pin_thread_to_core(2);
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
                std::cerr << std::format("\n!! [System-Watchdog] THERMAL BREACH: {}mC. Emergency Halt.\n",
                                         temp)
                          << std::flush;
                local_regs->control.write(0x0); // Direct hardware shutdown
                m_run_watchdog.store(false, std::memory_order_release);
                break;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(WATCHDOG_SLEEP_US));
        }
    }

    /** * @brief Allocates and maps DMA memory channels. */
    void initialize_shuttles(const PorthPDK& pdk, bool is_physical) {
        const uint32_t num_channels = pdk.get_num_channels();
        m_shuttles.reserve(num_channels);

        for (uint32_t i = 0; i < num_channels; ++i) {
            auto shuttle = std::make_unique<PorthShuttle<RingSize>>(0, m_strict);

            if (is_physical && m_device_ptr) {
                // Maps the HugePage-backed memory into the IOMMU for hardware visibility.
                const uint64_t iova = m_device_ptr->map_dma(shuttle->get_raw_memory_ptr(),
                                                            shuttle->get_raw_memory_size());
                shuttle->set_device_iova(iova);
            }

            m_shuttles.push_back(std::move(shuttle));
            if (i == 0 && m_regs) {
                // Commits the first DMA address to the hardware data pointer register.
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
     * @brief Physical Constructor: Orchestrates logic-to-physical handshake via VFIO.
     */
    explicit Driver(PorthVFIODevice& device,
                    const PorthPDK& pdk,
                    IPhysicsModel* physics = nullptr,
                    bool strict            = true)
        : m_regs(device.view()), m_device_ptr(&device), m_strict(strict) {

        static StubPhysics default_stub;
        m_physics_model = (physics != nullptr) ? physics : &default_stub;

        m_thermal_limit_mc = pdk.get_thermal_limit();
        device.validate_against_pdk(pdk);
        initialize_shuttles(pdk, true);
    }

    /**
     * @brief Simulation Constructor: Enables Digital Twin validation on standard hosts.
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

    /** @brief Destructor: Ensures graceful hardware shutdown and memory unmapping. */
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

    /** @brief Links the driver to the Shared Memory telemetry hub. */
    void set_stats_link(PorthStats* stats) noexcept { m_stats = stats; }

    /** * @brief Transmits a descriptor via the zero-copy DMA ring.
     * @param desc The DMA descriptor containing the buffer address and length.
     * @param channel_id The physical hardware channel index.
     * @return PorthStatus SUCCESS if the descriptor was pushed to the ring.
     */
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

    /** * @brief Retrieves a processed descriptor from the hardware ring.
     * @param out_desc Reference to populate with the retrieved descriptor.
     * @param channel_id The physical hardware channel index.
     * @return PorthStatus SUCCESS if a descriptor was popped from the ring.
     */
    [[nodiscard]] auto receive(PorthDescriptor& out_desc, uint32_t channel_id = 0) noexcept
        -> PorthStatus {
        if (channel_id >= m_shuttles.size())
            return PorthStatus::EMPTY;
        return m_shuttles[channel_id]->ring()->pop(out_desc) ? PorthStatus::SUCCESS
                                                             : PorthStatus::EMPTY;
    }

    /** @brief Accesses the raw MMIO register layout. */
    [[nodiscard]] auto get_regs() const noexcept -> PorthDeviceLayout* { return m_regs; }
    
    /** @brief Retrieves the DMA shuttle for a specific channel. */
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