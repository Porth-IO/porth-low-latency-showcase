/**
 * @file PorthHardwareScanner.hpp
 * @brief PCIe bus discovery utility for semiconductor hardware targets.
 *
 * Porth-IO: Sovereign Logic Layer
 * Copyright (c) 2026 Porth-IO Contributors
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
 * @brief Simple container for a PCIe Bus/Device/Function address.
 */
struct PciAddress {
    uint8_t bus;
    uint8_t device;
    uint8_t function;

    [[nodiscard]] std::string to_string() const {
        return std::format("0000:{:02x}:{:02x}.{:x}", bus, device, function);
    }
};

/**
 * @class PorthHardwareScanner
 * @brief Scans the PCI bus to identify and locate target hardware.
 */
class PorthHardwareScanner {
public:
    /**
     * @brief Scans the PCIe bus for specific Vendor and Device IDs.
     * @param vendor_id The 16-bit Vendor ID to search for.
     * @param device_id The 16-bit Device ID to search for.
     * @return std::optional<PciAddress> The address if found, nullopt otherwise.
     */
    static std::optional<PciAddress> find_target(uint16_t vendor_id, uint16_t device_id) {
        struct pci_access* pacc = pci_alloc();
        pci_init(pacc);
        pci_scan_bus(pacc);

        std::optional<PciAddress> target = std::nullopt;

        for (struct pci_dev* dev = pacc->devices; dev != nullptr; dev = dev->next) {
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