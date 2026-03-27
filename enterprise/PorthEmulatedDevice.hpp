/**
 * @file PorthEmulatedDevice.hpp
 * @brief RAII management for POSIX Shared Memory hardware emulation.
 *
 * Porth-IO: Sovereign Logic Layer
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
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
 * @brief Manages POSIX Shared Memory segments to emulate PCIe BAR mapping.
 *
 * Provides a memory-mapped interface to the PorthDeviceLayout, allowing
 * the driver to interact with a Digital Twin as if it were physical hardware.
 */
class PorthEmulatedDevice {
private:
    std::string m_name;
    PorthDeviceLayout* m_device_ptr{nullptr};
    bool m_is_owner;

public:
    /**
     * @brief Constructor: Maps or creates the shared memory segment.
     * @param mem_name Unique identifier for the memory segment.
     * @param create If true, initializes the segment (Emulator side).
     */
    explicit PorthEmulatedDevice(const std::string& mem_name, bool create = true)
        : m_name("/" + mem_name), m_is_owner(create) {

        if (create) {
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
            if (ftruncate(fd, sizeof(PorthDeviceLayout)) == -1) {
                (void)close(fd);
                throw std::runtime_error(
                    "[Porth-Emulated] Error: ftruncate failed during BAR allocation.");
            }
        }

        void* raw_ptr = mmap(nullptr, sizeof(PorthDeviceLayout), 
                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

        (void)close(fd);

        if (raw_ptr == MAP_FAILED) {
            throw std::runtime_error("[Porth-Emulated] Error: mmap failed for segment.");
        }

        m_device_ptr = static_cast<PorthDeviceLayout*>(raw_ptr);
    }

    ~PorthEmulatedDevice() {
        if (m_device_ptr != nullptr) {
            (void)munmap(m_device_ptr, sizeof(PorthDeviceLayout));
        }

        if (m_is_owner) {
            (void)shm_unlink(m_name.c_str());
        }
    }

    PorthEmulatedDevice(const PorthEmulatedDevice&)                    = delete;
    auto operator=(const PorthEmulatedDevice&) -> PorthEmulatedDevice& = delete;
    PorthEmulatedDevice(PorthEmulatedDevice&&)                         = delete;
    auto operator=(PorthEmulatedDevice&&) -> PorthEmulatedDevice&      = delete;

    [[nodiscard]] auto view() noexcept -> PorthDeviceLayout* { return m_device_ptr; }
    [[nodiscard]] auto view() const noexcept -> const PorthDeviceLayout* { return m_device_ptr; }
    [[nodiscard]] auto operator->() noexcept -> PorthDeviceLayout* { return m_device_ptr; }
};

} // namespace porth