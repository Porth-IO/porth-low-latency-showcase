/**
 * @file StubPhysics.hpp
 * @brief A zero-jitter, zero-penalty model for Open Core users.
 *
 * Porth-IO: Low Latency Showcase
 */

#pragma once
#include "IPhysicsModel.hpp"

namespace porth {

/**
 * @class StubPhysics
 * @brief Ideal-case baseline implementation of the physical layer model.
 *
 * This class provides a deterministic, zero-jitter environment. It is primarily 
 * used during the initial calibration phase of the Porth-IO framework or for 
 * "Digital Twin" validation where hardware-induced noise is excluded to measure 
 * the raw efficiency of the software logic layer.
 */
class StubPhysics : public IPhysicsModel {
public:
    /** * @brief Returns zero jitter, representing a thermally stable lattice. 
     * Used for measuring the theoretical minimum latency of the data plane.
     */
    [[nodiscard]] auto calculate_thermal_jitter(uint32_t, uint32_t) const noexcept
        -> uint64_t override {
        return 0; // Open-source baseline has no thermal drift modeling
    }

    /** * @brief Returns zero penalty, representing an ideal signal path with no FEC retries. 
     * This establishes the floor for packet-to-logic propagation time.
     */
    [[nodiscard]] auto get_fec_penalty(int32_t, double) const noexcept -> uint64_t override {
        return 0; // Open-source baseline has no FEC retry modeling
    }

    /** @brief Returns the identifier for this idealized simulation model. */
    [[nodiscard]] auto model_name() const noexcept -> const char* override {
        return "Open-Core-Stub";
    }
};

} // namespace porth