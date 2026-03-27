/**
 * @file PorthPDK.hpp
 * @brief Dynamic register map loader for the "Universal Translator" vision.
 *
 * Porth-IO: The Sovereign Logic Layer (Open Core)
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

namespace porth {

/**
 * @class PorthPDK
 * @brief Handles dynamic loading of semiconductor foundry register maps.
 *
 * This class parses JSON manifests that define the physical boundaries and
 * register offsets of a specific chip. It allows Porth-IO to adapt to
 * different hardware (InP, GaN, etc.) without recompilation.
 */
class PorthPDK {
private:
    std::map<std::string, uint64_t> m_offsets;
    std::string m_chip_name{"Generic_Mock"};
    uint64_t m_base_delay_ns{0};
    uint64_t m_jitter_ns{0};
    uint32_t m_thermal_threshold_mc{0};
    uint64_t m_fec_penalty_ns{0};
    uint32_t m_num_channels{1};
    uint16_t m_vendor_id{0};
    uint16_t m_device_id{0};

public:
    /** * @brief Load a JSON manifest defining the hardware register map.
     * @param path Filepath to the PDK JSON manifest.
     * @return true if loaded and validated, false otherwise.
     */
    auto load_manifest(const std::string& path) -> bool {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::cerr << std::format("[Porth-PDK] Error: Could not open manifest at {}\n", path);
            return false;
        }

        try {
            nlohmann::json data = nlohmann::json::parse(f);

            // 1. Parse Metadata and Delay Floor
            m_chip_name     = data.at("chip_name").get<std::string>();
            m_base_delay_ns = data.at("base_delay_ns").get<uint64_t>();
            m_jitter_ns     = data.at("jitter_ns").get<uint64_t>();

            m_vendor_id = static_cast<uint16_t>(
                std::stoi(data.at("vendor_id").get<std::string>(), nullptr, 16));
            m_device_id = static_cast<uint16_t>(
                std::stoi(data.at("device_id").get<std::string>(), nullptr, 16));

            // 2. Parse Physics Boundaries (Required for Watchdog logic)
            const auto& physics    = data.at("physics");
            m_thermal_threshold_mc = physics.at("thermal_threshold_mc").get<uint32_t>();
            m_fec_penalty_ns       = physics.at("fec_retry_penalty_ns").get<uint64_t>();

            // 3. Parse Channel Configuration (Optional, defaults to 1)
            if (data.contains("num_channels")) {
                m_num_channels = data.at("num_channels").get<uint32_t>();
            }

            // 4. Parse Register Map (Foundry Offsets)
            const auto& regs = data.at("registers");
            m_offsets.clear();
            for (auto it = regs.begin(); it != regs.end(); ++it) {
                // Supports hex string format (e.g., "0x4000")
                m_offsets[it.key()] = std::stoull(it.value().get<std::string>(), nullptr, 16);
            }

            std::cout << std::format("[Porth-PDK] Success: Profile '{}' active (Channels: {})\n",
                                     m_chip_name,
                                     m_num_channels);
            return true;

        } catch (const std::exception& e) {
            std::cerr << std::format("[Porth-PDK] Validation Error in {}: {}\n", path, e.what());
            return false;
        }
    }

    [[nodiscard]] auto get_chip_name() const noexcept -> std::string { return m_chip_name; }
    [[nodiscard]] auto get_base_delay() const noexcept -> uint64_t { return m_base_delay_ns; }
    [[nodiscard]] auto get_jitter() const noexcept -> uint64_t { return m_jitter_ns; }
    [[nodiscard]] auto get_thermal_limit() const noexcept -> uint32_t {
        return m_thermal_threshold_mc;
    }
    [[nodiscard]] auto get_fec_penalty() const noexcept -> uint64_t { return m_fec_penalty_ns; }
    [[nodiscard]] auto get_num_channels() const noexcept -> uint32_t { return m_num_channels; }

    [[nodiscard]] auto get_vendor_id() const noexcept -> uint16_t { return m_vendor_id; }
    [[nodiscard]] auto get_device_id() const noexcept -> uint16_t { return m_device_id; }

    /** @brief Retrieves the byte offset for a specific named register. */
    [[nodiscard]] auto get_offset(const std::string& name) const -> uint64_t {
        if (auto it = m_offsets.find(name); it != m_offsets.end()) {
            return it->second;
        }
        return 0;
    }
};

} // namespace porth