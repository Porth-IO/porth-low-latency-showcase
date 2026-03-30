/**
 * @file IPhysicsModel.hpp
 * @brief Abstract interface for semiconductor physics modeling.
 *
 * Porth-IO: Low Latency Showcase
 */

#pragma once

#include <cstdint>

namespace porth {

/**
 * @class IPhysicsModel
 * @brief Abstract interface for injecting physical-layer behavioral models into the Logic Layer.
 *
 * This interface allows the Porth-IO Logic Layer to account for non-deterministic 
 * hardware behaviors (such as thermal lattice drift and signal attenuation) 
 * within a deterministic simulation environment.
 */
class IPhysicsModel {
public:
    /** @brief Virtual destructor to ensure proper cleanup of derived physics models. */
    virtual ~IPhysicsModel() = default;

    /** * @brief Calculates timing jitter induced by the Indium Phosphide (InP) thermal state. 
     * @param temp_mc Current laser temperature in milli-Celsius.
     * @param threshold_mc The PDK-defined thermal boundary before lattice instability occurs.
     * @return uint64_t The calculated jitter penalty in nanoseconds.
     */
    [[nodiscard]] virtual auto calculate_thermal_jitter(uint32_t temp_mc,
                                                        uint32_t threshold_mc) const noexcept
        -> uint64_t = 0;

    /** * @brief Models the latency overhead of Forward Error Correction (FEC) based on SNR.
     * @param current_snr The Signal-to-Noise Ratio in milli-dB.
     * @param error_rate The baseline bit-error-rate (BER) for the physical interconnect.
     * @return uint64_t The total delay penalty in nanoseconds if an FEC retry is triggered.
     */
    [[nodiscard]] virtual auto get_fec_penalty(int32_t current_snr,
                                               double error_rate) const noexcept -> uint64_t = 0;

    /** * @brief Retrieves the architectural identifier for the active physics model.
     * @return const char* A string descriptor (e.g., "Newport-InP-HighFi" or "Open-Core-Stub"). 
     */
    [[nodiscard]] virtual auto model_name() const noexcept -> const char* = 0;
};

} // namespace porth