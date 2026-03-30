/**
 * @file PorthEmulatedDevice.hpp
 * @brief RAII management for POSIX Shared Memory hardware emulation.
 *
 * Porth-IO: Low Latency Showcase
 */

#pragma once

#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "porth/PorthDeviceLayout.hpp"

namespace porth {

/**
 * @class PorthEmulatedDevice
 * @brief Manages POSIX Shared Memory (SHM) segments to emulate PCIe BAR mapping.
 *
 * This class provides a memory-mapped interface to the PorthDeviceLayout, 
 * facilitating low-latency communication between the driver and a Digital Twin 
 * process. By emulating MMIO via SHM, the system can be validated on 
 * standard Linux hosts without physical PCIe hardware.
 */
class PorthEmulatedDevice {
private:
    std::string m_name;
    PorthDeviceLayout* m_device_ptr{nullptr};
    bool m_is_owner;

public:
    /**
     * @brief Constructor: Maps or creates a shared memory segment for hardware emulation.
     *
     * In emulator mode (create=true), this initializes the memory segment to 
     * match the physical footprint of the Newport hardware. In driver mode 
     * (create=false), it attaches to an existing segment.
     *
     * @param mem_name Unique identifier for the memory segment (e.g., "porth_bar0").
     * @param create If true, the segment is initialized and sized; if false, it is opened read-write.
     * @throws std::runtime_error If SHM creation, sizing, or mapping fails.
     */
    explicit PorthEmulatedDevice(const std::string& mem_name, bool create = true)
        : m_name("/" + mem_name), m_is_owner(create) {

        if (create) {
            // Ensure a clean state for the emulated BAR region.
            (void)shm_unlink(m_name.c_str());
        }

        int flags = O_RDWR;
        if (create) {
            flags |= O_CREAT;
        }

        const int fd = shm_open(m_name.c_str(), flags, 0666);
        if (fd == -1) {
            throw std::runtime_error(
                std::format("[Porth-Emulated] Error: shm_open failed for {}", m_name));
        }

        if (create) {
            // Size the segment to match the architectural PorthDeviceLayout footprint.
            if (ftruncate(fd, sizeof(PorthDeviceLayout)) == -1) {
                (void)close(fd);
                throw std::runtime_error(
                    "[Porth-Emulated] Error: ftruncate failed during BAR allocation.");
            }
        }

        // Establish the shared memory mapping between the logic layer and emulated hardware.
        void* raw_ptr = mmap(nullptr, sizeof(PorthDeviceLayout), 
                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

        (void)close(fd);

        if (raw_ptr == MAP_FAILED) {
            throw std::runtime_error("[Porth-Emulated] Error: mmap failed for segment.");
        }

        m_device_ptr = static_cast<PorthDeviceLayout*>(raw_ptr);
    }

    /** @brief Destructor: Unmaps the emulated region and cleans up SHM if the process is the owner. */
    ~PorthEmulatedDevice() {
        if (m_device_ptr != nullptr) {
            (void)munmap(m_device_ptr, sizeof(PorthDeviceLayout));
        }

        if (m_is_owner) {
            (void)shm_unlink(m_name.c_str());
        }
    }

    // Hardware-mapped emulators represent a specific architectural contract and 
    // should not be copied or moved to avoid illegal memory access.
    PorthEmulatedDevice(const PorthEmulatedDevice&)                    = delete;
    auto operator=(const PorthEmulatedDevice&) -> PorthEmulatedDevice& = delete;
    PorthEmulatedDevice(PorthEmulatedDevice&&)                         = delete;
    auto operator=(PorthEmulatedDevice&&) -> PorthEmulatedDevice&      = delete;

    /** @brief Returns a pointer to the emulated register layout. */
    [[nodiscard]] auto view() noexcept -> PorthDeviceLayout* { return m_device_ptr; }
    
    /** @brief Returns a const pointer to the emulated register layout. */
    [[nodiscard]] auto view() const noexcept -> const PorthDeviceLayout* { return m_device_ptr; }
    
    /** @brief Pointer access to the emulated register layout. */
    [[nodiscard]] auto operator->() noexcept -> PorthDeviceLayout* { return m_device_ptr; }
};

} // namespace porth