/**
 * @file IPhysicsModel.hpp
 * @brief Abstract interface for semiconductor physics modeling.
 * * Part of the Porth-IO Open Core Framework.
 */

#pragma once

#include <cstdint>

namespace porth {

/**
 * @class IPhysicsModel
 * @brief Interface for injecting physical layer behavior into the Logic Layer.
 */
class IPhysicsModel {
public:
    virtual ~IPhysicsModel() = default;

    /** @brief Calculates timing jitter based on thermal state. */
    [[nodiscard]] virtual auto calculate_thermal_jitter(uint32_t temp_mc,
                                                        uint32_t threshold_mc) const noexcept
        -> uint64_t = 0;

    /** @brief Calculates latency penalties for FEC retries based on SNR. */
    [[nodiscard]] virtual auto get_fec_penalty(int32_t current_snr,
                                               double error_rate) const noexcept -> uint64_t = 0;

    /** @brief Returns a baseline name for the model (e.g., "InP-HighFi" or "Stub"). */
    [[nodiscard]] virtual auto model_name() const noexcept -> const char* = 0;
};

} // namespace porth