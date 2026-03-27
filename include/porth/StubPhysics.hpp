#pragma once
#include "IPhysicsModel.hpp"

namespace porth {

/**
 * @class StubPhysics
 * @brief A zero-jitter, zero-penalty model for Open Core users.
 */
class StubPhysics : public IPhysicsModel {
public:
    [[nodiscard]] auto calculate_thermal_jitter(uint32_t, uint32_t) const noexcept
        -> uint64_t override {
        return 0; // Public version has no thermal drift modeling
    }

    [[nodiscard]] auto get_fec_penalty(int32_t, double) const noexcept -> uint64_t override {
        return 0; // Public version has no FEC retry modeling
    }

    [[nodiscard]] auto model_name() const noexcept -> const char* override {
        return "Open-Core-Stub";
    }
};

} // namespace porth