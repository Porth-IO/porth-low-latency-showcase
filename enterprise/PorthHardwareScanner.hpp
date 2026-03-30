/**
 * @file PorthHardwareScanner.hpp
 * @brief PCIe bus discovery utility for semiconductor hardware targets.
 *
 * Porth-IO: Low Latency Showcase
 */

#pragma once

#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <pci/pci.h>
}

namespace porth {

/**
 * @struct PciAddress
 * @brief Container for a PCIe Bus/Device/Function (BDF) address.
 *
 * This structure follows the standard Linux PCI naming convention (Domain:Bus:Device.Function).
 */
struct PciAddress {
    uint8_t bus;
    uint8_t device;
    uint8_t function;

    /** @brief Returns the formatted PCI address string (e.g., "0000:01:00.0"). */
    [[nodiscard]] std::string to_string() const {
        return std::format("0000:{:02x}:{:02x}.{:x}", bus, device, function);
    }
};

/**
 * @class PorthHardwareScanner
 * @brief Utility for automated PCIe bus discovery and device identification.
 *
 * This class utilizes the libpci library to scan the system's PCIe topology. 
 * It is used to automatically locate target hardware (such as the Newport 
 * Cluster controllers) by searching for specific Vendor and Device IDs, 
 * streamlining the hardware assignment process for the logic layer.
 */
class PorthHardwareScanner {
public:
    /**
     * @brief Scans the system PCIe bus for a specific hardware target.
     * * Iterates through all detected PCI devices on the bus and attempts to 
     * match the provided Vendor and Device IDs against the hardware's 
     * identification registers.
     *
     * @param vendor_id The 16-bit PCI Vendor ID (e.g., 0x10EE for Xilinx).
     * @param device_id The 16-bit PCI Device ID.
     * @return std::optional<PciAddress> The BDF address if found; otherwise nullopt.
     */
    static std::optional<PciAddress> find_target(uint16_t vendor_id, uint16_t device_id) {
        struct pci_access* pacc = pci_alloc();
        pci_init(pacc);
        pci_scan_bus(pacc);

        std::optional<PciAddress> target = std::nullopt;

        for (struct pci_dev* dev = pacc->devices; dev != nullptr; dev = dev->next) {
            // Fill identification information (Vendor/Device ID)
            pci_fill_info(dev, PCI_FILL_IDENT);

            if (dev->vendor_id == vendor_id && dev->device_id == device_id) {
                target = PciAddress{dev->bus, dev->dev, dev->func};
                std::cout << std::format("[Porth-Scanner] Found Target Hardware at {}\n",
                                         target->to_string());
                break;
            }
        }

        pci_cleanup(pacc);
        return target;
    }
};

} // namespace porth