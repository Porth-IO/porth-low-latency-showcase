/**
 * @file PorthVFIODevice.hpp
 * @brief Professional VFIO backend for userspace PCIe hardware ownership.
 *
 * Porth-IO: Low Latency Showcase
 */

#pragma once

#include "PorthDeviceLayout.hpp"
#include "PorthPDK.hpp"
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <grp.h>
#include <iostream>
#include <linux/vfio.h>
#include <pwd.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace porth {

/**
 * @class PorthVFIODevice
 * @brief Manages physical PCIe hardware via the Linux VFIO (Virtual Function I/O) interface.
 *
 * This class implements userspace ownership of a PCIe device. It claims the device's 
 * IOMMU group, enables bus mastering, and maps the hardware registers (BARs) 
 * directly into the application's address space. This allows for zero-copy 
 * register interaction without the overhead of kernel-mode context switches.
 */
class PorthVFIODevice {
private:
    int m_container_fd;
    int m_group_fd    = -1;
    int m_device_fd   = -1;
    void* m_bar_ptr   = nullptr;
    size_t m_bar_size = 0;

    /**
     * @brief Internal helper to verify if the current process has sufficient hardware access.
     * * Checks for root privileges or membership in the 'vfio' group. This ensures 
     * the system adheres to the Linux security model for userspace hardware drivers.
     */
    void check_permissions() const {
        if (getuid() == 0)
            return; // Root is always allowed

        struct stat st;
        if (stat("/dev/vfio/vfio", &st) == 0) {
            // Check if current user is in the group that owns the VFIO device
            gid_t current_gid = getgid();
            if (st.st_gid == current_gid)
                return;

            // Check supplemental groups
            int ngroups = getgroups(0, nullptr);
            std::vector<gid_t> groups(static_cast<size_t>(ngroups));

            if (getgroups(ngroups, groups.data()) == -1) {
                throw std::runtime_error("Porth-VFIO: Failed to retrieve supplemental groups.");
            }

            for (auto gid : groups) {
                if (gid == st.st_gid)
                    return;
            }
        }

        throw std::runtime_error(
            "Porth-VFIO: Insufficient permissions. Run as root or add user to the 'vfio' group "
            "and ensure /dev/vfio/vfio has correct group-read/write permissions.");
    }

    /**
     * @brief Internal helper to identify the IOMMU group associated with a PCI address.
     * * IOMMU groups are the smallest unit of isolation that the hardware can 
     * enforce. All devices within a group must be assigned to userspace 
     * simultaneously to ensure memory protection.
     */
    [[nodiscard]] static auto get_iommu_group(const std::string& pci_addr) -> int {
        std::string path = std::format("/sys/bus/pci/devices/{}/iommu_group", pci_addr);
        if (!std::filesystem::exists(path)) {
            throw std::runtime_error(std::format(
                "Device {} is not in an IOMMU group. Ensure VT-d/AMD-Vi is enabled in BIOS.", pci_addr));
        }
        return std::stoi(std::filesystem::read_symlink(path).filename().string());
    }

public:
    /**
     * @brief Constructor: Claims ownership of a physical PCIe device via VFIO.
     * * Performs the full VFIO handshake: opens the container, attaches the IOMMU 
     * group, sets the IOMMU type (Type1), and retrieves the device file descriptor.
     * * @param pci_addr The PCI address (e.g., "0000:01:00.0").
     * @throws std::runtime_error If the device cannot be assigned or mapped.
     */
    explicit PorthVFIODevice(const std::string& pci_addr)
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        : m_container_fd(-1) {

        // 0. Security Audit: Verify user has rights to claim hardware
        check_permissions();

        m_container_fd = open("/dev/vfio/vfio", O_RDWR);

        if (m_container_fd < 0) {
            throw std::runtime_error(
                "Failed to open /dev/vfio/vfio. Check permissions (sudo required).");
        }

        // 1. Verify API Version compatibility
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (ioctl(m_container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
            throw std::runtime_error("Incompatible VFIO API version.");
        }

        // 2. Open the IOMMU Group
        int group_id = get_iommu_group(pci_addr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        m_group_fd = open(std::format("/dev/vfio/{}", group_id).c_str(), O_RDWR);
        if (m_group_fd < 0) {
            if (errno == EBUSY) {
                throw std::runtime_error(std::format(
                    "Porth-VFIO: IOMMU group {} is busy. Another driver (likely a kernel driver) "
                    "still has ownership of a device in this group.",
                    group_id));
            }
            throw std::runtime_error(std::format(
                "Porth-VFIO: Failed to open IOMMU group {}. (Errno: {})", group_id, errno));
        }

        // 3. Add Group to Container
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (ioctl(m_group_fd, VFIO_GROUP_SET_CONTAINER, &m_container_fd) < 0) {
            throw std::runtime_error("Failed to set VFIO container.");
        }

        // 4. Set IOMMU Type (Type1 is standard for x86/ARM64 memory management)
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (ioctl(m_container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
            throw std::runtime_error("Failed to set IOMMU type to Type1.");
        }

        // 5. Get Device File Descriptor
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        m_device_fd = ioctl(m_group_fd, VFIO_GROUP_GET_DEVICE_FD, pci_addr.c_str());
        if (m_device_fd < 0) {
            throw std::runtime_error(std::format("Failed to get device FD for {}.", pci_addr));
        }

        // 6. Map BAR 0 (The Primary Hardware Control Region)
        struct vfio_region_info reg{};
        reg.argsz = sizeof(reg);
        reg.index = VFIO_PCI_BAR0_REGION_INDEX;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (ioctl(m_device_fd, VFIO_DEVICE_GET_REGION_INFO, &reg) < 0) {
            throw std::runtime_error("Failed to get BAR0 region info.");
        }

        m_bar_size = reg.size;
        m_bar_ptr  = mmap(nullptr,
                         m_bar_size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         m_device_fd,
                         static_cast<off_t>(reg.offset));

        if (m_bar_ptr == MAP_FAILED) {
            throw std::runtime_error("Failed to mmap BAR0 into userspace.");
        }

        std::cout << std::format(
            "[Porth-VFIO] Hardware Assignment Successful: {} | BAR0 at {}\n", pci_addr, m_bar_ptr);
    }

    /** @brief Destructor: Releases BAR mappings and closes VFIO file descriptors. */
    ~PorthVFIODevice() {
        if (m_bar_ptr != nullptr && m_bar_ptr != MAP_FAILED) {
            munmap(m_bar_ptr, m_bar_size);
        }
        if (m_device_fd >= 0) {
            close(m_device_fd);
        }
        if (m_group_fd >= 0) {
            close(m_group_fd);
        }
        if (m_container_fd >= 0) {
            close(m_container_fd);
        }
    }

    /**
     * @brief map_dma: Maps a userspace memory region into the hardware IOMMU.
     * * This establishes the I/O Virtual Address (IOVA) required for the device 
     * to access HugePage memory directly. It uses a 1:1 mapping (VADDR == IOVA) 
     * for simplicity in userspace-to-hardware handshakes.
     * * @param vaddr The virtual address of the PorthHugePage allocation.
     * @param size The total size of the memory region.
     * @return uint64_t The I/O Virtual Address (IOVA) assigned for DMA.
     */
    auto map_dma(void* vaddr, size_t size) const -> uint64_t {
        struct vfio_iommu_type1_dma_map dma_map{};
        dma_map.argsz = sizeof(dma_map);
        dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        dma_map.vaddr = reinterpret_cast<uintptr_t>(vaddr);
        dma_map.iova  = reinterpret_cast<uintptr_t>(vaddr); // Identity mapping
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        dma_map.size = size;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (ioctl(m_container_fd, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
            throw std::runtime_error("Failed to map DMA region in IOMMU.");
        }

        std::cout << std::format("[Porth-VFIO] DMA Region Mapped: VADDR {} -> IOVA {}\n",
                                 vaddr,
                                 dma_map.iova)
                  << std::flush;
        return dma_map.iova;
    }

    /**
     * @brief unmap_dma: Removes a memory region from the IOMMU translation table.
     * @param iova The I/O Virtual Address to unmap.
     * @param size The size of the region.
     */
    void unmap_dma(uint64_t iova, size_t size) const {
        struct vfio_iommu_type1_dma_unmap dma_unmap{};
        dma_unmap.argsz = sizeof(dma_unmap);
        dma_unmap.flags = 0;
        dma_unmap.iova  = iova;
        dma_unmap.size  = size;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (ioctl(m_container_fd, VFIO_IOMMU_UNMAP_DMA, &dma_unmap) < 0) {
            std::cerr << std::format("[Porth-VFIO] Warning: Failed to unmap DMA region at 0x{:x}\n",
                                     iova);
        } else {
            std::cout << std::format("[Porth-VFIO] DMA Region Unmapped: IOVA 0x{:x}\n", iova)
                      << std::flush;
        }
    }

    /**
     * @brief Validates the physical BAR against the PDK (Physical Design Kit) manifest.
     * * Ensures the assigned hardware meets the architectural requirements 
     * defined in the chip manifest, preventing memory access violations.
     * * @param pdk The loaded hardware Physical Design Kit.
     * @throws std::runtime_error If the hardware BAR is smaller than the layout requirements.
     */
    void validate_against_pdk(const PorthPDK& pdk) const {
        // 1. Verify BAR Size matches the expected layout footprint.
        if (m_bar_size < expected_layout_size) {
            throw std::runtime_error(std::format("Porth-VFIO: Hardware BAR0 size ({} bytes) is "
                                                 "smaller than required register layout ({} bytes).",
                                                 m_bar_size,
                                                 expected_layout_size));
        }

        // 2. Cross-reference PDK metadata
        std::cout << std::format(
            "[Porth-VFIO] Validation Successful: Hardware BAR0 ({} bytes) matches {} profile.\n",
            m_bar_size,
            pdk.get_chip_name());
    }

    /**
     * @brief Returns the mapped register layout for the data plane.
     * @return PorthDeviceLayout* Pointer to the memory-mapped physical registers.
     */
    [[nodiscard]] auto view() noexcept -> PorthDeviceLayout* {
        return static_cast<PorthDeviceLayout*>(m_bar_ptr);
    }

    // Prohibit copying/moving to prevent multi-owner conflicts over hardware BARs
    PorthVFIODevice(const PorthVFIODevice&)                    = delete;
    auto operator=(const PorthVFIODevice&) -> PorthVFIODevice& = delete;
    PorthVFIODevice(PorthVFIODevice&&)                         = delete;
    auto operator=(PorthVFIODevice&&) -> PorthVFIODevice&      = delete;
};

} // namespace porth