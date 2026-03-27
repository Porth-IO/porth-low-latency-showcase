/**
 * @file PorthSimPHY.hpp
 * @brief Physics-based emulator for the Newport Cluster physical layer (PHY).
 *
 * Porth-IO: The Sovereign Logic Layer
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "porth/IPhysicsModel.hpp"
#include "porth/PorthClock.hpp"
#include <algorithm>
#include <atomic>
#include <random>

namespace porth {

/** @brief PHY Simulation Constants for Newport Cluster hardware modeling. */
constexpr double DEFAULT_FEC_ERROR_RATE =
    0.001;                                     ///< 0.1% Bit Error Rate (BER) proxy for FEC retries.
constexpr uint64_t DEFAULT_FEC_PENALTY_NS = 5; ///< Baseline logic delay for FEC decoding (5ns).
constexpr uint32_t DEFAULT_BASE_TEMP_MC =
    25000; ///< 25.0°C - Standard laboratory ambient temperature.
constexpr uint64_t DEFAULT_BASE_DELAY_NS =
    100; ///< Propagation time for light in 20m of optical fiber.
constexpr uint64_t DEFAULT_JITTER_INIT_NS = 25;  ///< Baseline clock recovery jitter.
constexpr double DEFAULT_CPNS             = 2.4; ///< Default calibration for a 2.4GHz CPU.

/** * @brief Physical thermal threshold (40.0°C).
 * Beyond this limit, the Indium Phosphide (InP) lattice drift significantly
 * impacts signal-to-noise ratio (SNR), introducing non-linear jitter.
 */
constexpr uint32_t MC_TO_C_DIVISOR = 1000; ///< Conversion factor for milli-Celsius to Celsius.

/** * @brief FEC Spike Penalty (500ns).
 * Simulates a full packet retransmission or deep-interleaving buffer flush
 * triggered by an uncorrectable error in the GaN power stage.
 */
constexpr uint64_t FEC_RETRY_SPIKE_NS = 500;

/** * @brief PAM4 Signaling Noise Floor (12ps).
 * Simulates the high-frequency jitter floor of PCIe Gen 6 PAM4 signaling.
 */
constexpr uint64_t PAM4_JITTER_FLOOR_PS = 12;

/** * @brief Signal-to-Noise Ratio (SNR) Penalty.
 * As SNR drops, logic delay increases as the PHY works harder to recover the clock.
 */
constexpr uint64_t SNR_PENALTY_NS_PER_DB = 2;

/** * @brief Standard SNR baseline for healthy hardware (30dB). */
constexpr int32_t STANDARD_SNR_DB = 3000;

/** * @brief Critical SNR threshold for increased error rates (20dB). */
constexpr int32_t SNR_CRITICAL_THRESHOLD = 2000;

/** * @brief Error rate multiplier for low SNR conditions. */
constexpr double FEC_LOW_SNR_MULTIPLIER = 10.0;

/**
 * @class PorthSimPHY
 * @brief Emulates PCIe physical layer effects for compound semiconductor interconnects.
 *
 * This class implements the Sovereign thermal/power feedback loop. It provides
 * a high-fidelity simulation of propagation delays and jitter caused by
 * physical fluctuations in the InP/GaN hardware lattice.
 */
class PorthSimPHY : public IPhysicsModel {
private:
    uint64_t m_base_delay_ns;    ///< Base floor for propagation (nanoseconds).
    uint64_t m_jitter_ns;        ///< Peak-to-peak range of random clock noise.
    uint32_t m_thermal_limit_mc; ///< Dynamic lattice drift boundary.
    uint64_t m_fec_penalty_ns;   ///< Logic delay for FEC retries.
    double m_cycles_per_ns;      ///< Calibrated CPU frequency factor.

    int32_t m_current_snr{3000};

    /** @brief Constants for thermal inertia increments. */
    static constexpr uint32_t HEATING_STEP_MC = 25;
    static constexpr uint32_t COOLING_STEP_MC = 15;

    /** @brief Probability of a Forward Error Correction (FEC) retry. */
    double m_fec_error_rate = DEFAULT_FEC_ERROR_RATE;

    /** @brief Real-time laser temperature.
     * Shared across simulation threads to model thermal-induced signal degradation.
     */
    std::atomic<uint32_t> m_current_temp_mc{DEFAULT_BASE_TEMP_MC};

    /** @brief Internal random engine and distributions.
     * Marked mutable to allow access from const-qualified physics methods.
     */
    mutable std::mt19937 m_gen;
    mutable std::uniform_int_distribution<int64_t> m_jitter_dist;
    mutable std::uniform_real_distribution<double> m_error_dist;

    /** * @brief Performance Guard: CPU Architecture Hint.
     * * Justification: Using architecture-specific relax instructions prevents
     * "Pipeline Sizzling" during busy-wait loops, reducing CPU power consumption
     * and preventing speculative execution from polluting the cache.
     */
    static void cpu_relax() noexcept {
#if defined(__i386__) || defined(__x86_64__)
        // PAUSE: Notifies the CPU that we are in a spin-loop, improving power
        // efficiency and reducing the exit-latency of the loop.
        asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
        // ISB: Flushes the pipeline on ARM64 to ensure the loop condition is
        // re-evaluated without speculative interference.
        asm volatile("isb" ::: "memory");
#endif
    }

public:
    /**
     * @brief Physics Model: Calculates jitter based on thermal lattice drift.
     * @param temp_mc Current temperature in milli-Celsius.
     * @param threshold_mc PDK thermal threshold in milli-Celsius.
     * @return uint64_t Calculated jitter in nanoseconds.
     */
    [[nodiscard]] auto calculate_thermal_jitter(uint32_t temp_mc,
                                                uint32_t threshold_mc) const noexcept
        -> uint64_t override {
        uint64_t thermal_jitter = 0;
        if (temp_mc > threshold_mc) {
            // Linear approximation of jitter increase per degree Celsius above threshold.
            thermal_jitter = (temp_mc - threshold_mc) / MC_TO_C_DIVISOR;
        }
        return thermal_jitter;
    }

    /**
     * @brief Error Model: Simulates uncorrectable FEC events.
     * @param current_snr The Signal-to-Noise Ratio (SNR) in milli-dB.
     * @param error_rate Baseline error rate for the model.
     * @return uint64_t Penalty in ns (0 if no error).
     */
    [[nodiscard]] auto get_fec_penalty(int32_t current_snr, double error_rate) const noexcept
        -> uint64_t override {
        double effective_rate = error_rate;
        if (current_snr < SNR_CRITICAL_THRESHOLD) {
            effective_rate *= FEC_LOW_SNR_MULTIPLIER; // 10x error increase at low SNR
        }

        if (m_error_dist(m_gen) < effective_rate) {
            return FEC_RETRY_SPIKE_NS;
        }
        return 0;
    }

    /** @brief Returns the model identifier for validation. */
    [[nodiscard]] auto model_name() const noexcept -> const char* override {
        return "Newport-InP-HighFi";
    }

    /**
     * @brief Construct the PHY emulator with specific performance targets.
     * @param base_ns The base floor for signal propagation.
     * @param jitter_init The peak-to-peak jitter range.
     * @param cpns Clock calibration factor (Cycles per Nanosecond).
     */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    explicit PorthSimPHY(uint64_t base_ns     = DEFAULT_BASE_DELAY_NS,
                         uint64_t jitter_init = DEFAULT_JITTER_INIT_NS,
                         double cpns          = DEFAULT_CPNS)
        : m_base_delay_ns(base_ns), m_jitter_ns(jitter_init),
          m_thermal_limit_mc(45000), // Safety default
          m_fec_penalty_ns(DEFAULT_FEC_PENALTY_NS), m_cycles_per_ns(cpns),
          m_gen(static_cast<std::mt19937::result_type>(std::random_device{}())),
          m_jitter_dist(-static_cast<int64_t>(jitter_init), static_cast<int64_t>(jitter_init)),
          m_error_dist(0.0, 1.0) {}

    /** @brief Calibrates simulation physics based on real PDK data. */
    void calibrate_from_pdk(uint32_t thermal_mc, uint64_t fec_ns) noexcept {
        m_thermal_limit_mc = thermal_mc;
        m_fec_penalty_ns   = fec_ns;
    }

    /**
     * @brief update_thermal_load: Pushes real-time temperature telemetry into the PHY model.
     * @param temp_mc Current laser temperature in milli-Celsius (mC).
     */
    auto update_thermal_load(uint32_t temp_mc) noexcept -> void {
        const uint32_t current = m_current_temp_mc.load(std::memory_order_relaxed);
        if (temp_mc > current) {
            m_current_temp_mc.store(current + HEATING_STEP_MC,
                                    std::memory_order_relaxed); // Gradual heating
        } else if (temp_mc < current) {
            m_current_temp_mc.store(current - COOLING_STEP_MC,
                                    std::memory_order_relaxed); // Gradual cooling
        }
    }

    void set_snr(int32_t snr_mdb) { m_current_snr = snr_mdb; }

    /** @brief Returns the internal inertial temperature for register syncing. */
    [[nodiscard]] auto get_current_temp() const noexcept -> uint32_t {
        return m_current_temp_mc.load(std::memory_order_relaxed);
    }

    /**
     * @brief apply_protocol_delay: Busy-waits to simulate physical propagation time.
     */
    auto apply_protocol_delay(int32_t current_snr = STANDARD_SNR_DB) noexcept -> void {
        int64_t random_jitter = 0;
        if (m_jitter_ns > 0) {
            random_jitter = m_jitter_dist(m_gen);
        }

        uint64_t total_delay_ns =
            m_base_delay_ns + m_fec_penalty_ns + static_cast<uint64_t>(std::abs(random_jitter));

        // Note: Using PDK-driven threshold synced to simulation state
        total_delay_ns += calculate_thermal_jitter(get_current_temp(), m_thermal_limit_mc);
        total_delay_ns += get_fec_penalty(current_snr, m_fec_error_rate);

        const auto target_cycles =
            static_cast<uint64_t>(static_cast<double>(total_delay_ns) * m_cycles_per_ns);

        const uint64_t start_cycles = PorthClock::now_precise();
        while (PorthClock::now_precise() - start_cycles < target_cycles) {
            cpu_relax();
        }
    }

    /**
     * @brief Hot-swappable configuration for different hardware scenarios.
     */
    auto set_config(uint64_t base, uint64_t jitter) -> void {
        m_base_delay_ns = base;
        m_jitter_ns     = jitter;
        m_jitter_dist   = std::uniform_int_distribution<int64_t>(-static_cast<int64_t>(jitter),
                                                               static_cast<int64_t>(jitter));
    }
};

} // namespace porth