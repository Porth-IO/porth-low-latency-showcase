/**
 * @file PorthTelemetry.hpp
 * @brief Cross-process telemetry hub utilizing POSIX Shared Memory.
 *
 * Porth-IO: Low Latency Showcase
 */

#pragma once
#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace porth {

/**
 * @struct PorthStats
 * @brief Shared memory structure for real-time system observability.
 *
 * This structure is designed to reside in a POSIX Shared Memory segment, 
 * allowing high-speed telemetry data to be shared across process boundaries 
 * (e.g., between the Driver and a monitoring dashboard) with near-zero 
 * overhead. All fields utilize std::atomic to ensure thread-safe and 
 * process-safe access without mutex contention.
 */
struct PorthStats {
    std::atomic<uint64_t> total_packets{0};
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint64_t> dropped_packets{0};
    std::atomic<uint32_t> current_temp_mc{0};
    std::atomic<uint64_t> last_latency_ns{0};
    std::atomic<uint32_t> max_temp_mc{0};
    std::atomic<int32_t> current_snr_mdb{0}; // RF Signal-to-Noise Ratio in milli-dB
};

/**
 * @class PorthTelemetryHub
 * @brief Orchestrator for POSIX Shared Memory (SHM) telemetry links.
 *
 * This class manages the lifecycle of shared memory segments used for 
 * inter-process communication (IPC). It handles segment creation, 
 * permission mapping (fchmod), and size initialization (ftruncate). 
 * By utilizing SHM, the system avoids the latency penalties associated 
 * with sockets or pipes for real-time monitoring.
 */
class PorthTelemetryHub {
private:
    static constexpr mode_t SHM_PERMISSIONS = 0666;

    std::string m_name;
    PorthStats* m_stats{nullptr};
    bool m_owner;

public:
    /**
     * @brief Construct a new Telemetry Hub.
     * @param name The unique identifier for the SHM segment (e.g., "porth_stats").
     * @param create If true, the segment is created and initialized.
     * @param strict If true, throws an exception if the SHM link fails to establish.
     */
    explicit PorthTelemetryHub(const std::string& name, bool create = true, bool strict = false)
        : m_name("/" + name), m_owner(create) {

        int flags = O_RDWR | (create ? O_CREAT : 0);
        int fd    = shm_open(m_name.c_str(), flags, SHM_PERMISSIONS);
        if (fd == -1) {
            if (strict) {
                // Fatal error for performance-critical environments where
                // telemetry is required for hardware safety audits.
                throw std::runtime_error(
                    "[Fatal] Porth-IO: Failed to open Telemetry SHM in Strict Mode.");
            }
            // Non-strict fallback: Log a warning for non-root/simulation environments.
            std::cerr
                << "[Porth-IO] Warning: Telemetry SHM unavailable. Dashboard link will be inactive.\n";
            return; 
        }

        if (create) {
            // 1. Allocate the physical memory block for the stats structure.
            if (ftruncate(fd, sizeof(PorthStats)) == -1) {
                close(fd);
                if (strict) {
                    throw std::runtime_error(
                        "[Fatal] Porth-IO: Failed to size Telemetry SHM in Strict Mode.");
                }
                std::cerr << "[Porth-IO] Warning: Could not size Telemetry SHM.\n";
                return;
            }

            // 2. Adjust segment permissions to allow non-privileged monitoring 
            // tools to read the telemetry data written by the root-level driver.
            if (fchmod(fd, SHM_PERMISSIONS) == -1) {
                close(fd);
                throw std::runtime_error("Failed to set SHM permissions");
            }
        }

        // Map the shared memory segment into the process address space.
        void* ptr = mmap(nullptr, sizeof(PorthStats), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        m_stats = static_cast<PorthStats*>(ptr);
        if (create) {
            // Placement New: Initializes the atomic members directly within the 
            // shared memory segment to ensure consistent state for all attached processes.
            new (m_stats) PorthStats(); 
        }
    }

    /** @brief Destructor: Unmaps the telemetry segment and unlinks if the owner. */
    ~PorthTelemetryHub() {
        if (m_stats != nullptr) {
            munmap(m_stats, sizeof(PorthStats));
        }
        if (m_owner) {
            shm_unlink(m_name.c_str());
        }
    }

    // SHM handles are unique to the process mapping; copying or moving is 
    // prohibited to maintain stable telemetry link integrity.
    PorthTelemetryHub(const PorthTelemetryHub&)                    = delete;
    auto operator=(const PorthTelemetryHub&) -> PorthTelemetryHub& = delete;
    PorthTelemetryHub(PorthTelemetryHub&&)                         = delete;
    auto operator=(PorthTelemetryHub&&) -> PorthTelemetryHub&      = delete;

    /** @brief Returns a pointer to the mapped statistics structure. */
    auto view() noexcept -> PorthStats* { return m_stats; }
};

} // namespace porth