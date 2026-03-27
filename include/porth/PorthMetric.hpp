/**
 * @file PorthMetric.hpp
 * @brief Statistical analysis engine for ultra-low latency timing telemetry.
 *
 * Porth-IO: The Sovereign Logic Layer
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace porth {

/**
 * @brief Default capacity for the metric buffer.
 * 1,000,000 samples provide a statistically significant window for 10Gbps
 * line-rate packet analysis without excessive resident set size (RSS) pressure.
 */
constexpr size_t DEFAULT_METRIC_CAPACITY = 1000000;

/** @brief Percentile constants for statistical analysis. */
constexpr double PERCENTILE_Q1     = 25.0;
constexpr double PERCENTILE_Q3     = 75.0;
constexpr double PERCENTILE_P50    = 50.0;
constexpr double PERCENTILE_P99_9  = 99.9;
constexpr double PERCENTILE_P99_99 = 99.99;

/**
 * @class PorthMetric
 * @brief Statistical analysis engine for ultra-low latency timing.
 *
 * Optimized for the Newport Cluster, this class uses a pre-allocated buffer
 * to ensure that recording samples never triggers heap allocations or
 * context-switch-inducing Page Faults.
 */
class PorthMetric {
private:
    /**
     * @brief Pre-allocated sample buffer.
     * Reserving this space upfront protects the "Hot Path" from 'malloc' jitter.
     */
    std::vector<uint64_t> m_samples;
    size_t m_capacity;
    size_t m_count = 0;

    /**
     * @brief Prepares a sorted copy of the recorded samples for analysis.
     * @note This is an O(N log N) operation performed outside the hot path.
     * We create a copy to preserve the original chronological sequence of events.
     */
    [[nodiscard]] auto get_sorted_samples() const -> std::vector<uint64_t> {
        std::vector<uint64_t> sorted(m_samples.begin(),
                                     m_samples.begin() + static_cast<std::ptrdiff_t>(m_count));
        std::ranges::sort(sorted);
        return sorted;
    }

    /**
     * @brief Calculates the arithmetic mean.
     * @param samples Vector of sorted/unsorted timing samples.
     * @return double Average cycle count.
     */
    [[nodiscard]] auto calculate_mean(const std::vector<uint64_t>& samples) const noexcept
        -> double {
        const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
        return sum / static_cast<double>(m_count);
    }

    /**
     * @brief Calculates the standard deviation using the inner product for numerical stability.
     * @param samples Timing samples.
     * @param mean Pre-calculated mean.
     * @return double Standard deviation in cycles.
     */
    [[nodiscard]] auto calculate_stdev(const std::vector<uint64_t>& samples,
                                       double mean) const noexcept -> double {
        const double sq_sum =
            std::inner_product(samples.begin(), samples.end(), samples.begin(), 0.0);
        return std::sqrt(std::abs((sq_sum / static_cast<double>(m_count)) - (mean * mean)));
    }

    /**
     * @brief Retrieves a specific percentile and converts it to physical time (ns).
     * @param samples Sorted timing samples.
     * @param percentile Target percentile (0.0 to 100.0).
     * @param cpns Cycles Per Nanosecond (Hardware Frequency).
     * @return double Latency in nanoseconds.
     */
    [[nodiscard]] auto
    get_percentile_ns(const std::vector<uint64_t>& samples,
                      double percentile // NOLINT(bugprone-easily-swappable-parameters)
                      ,
                      double cpns) const noexcept -> double {
        auto idx = static_cast<size_t>(percentile * static_cast<double>(m_count) / 100.0);
        if (idx >= m_count) {
            idx = m_count - 1;
        }
        return static_cast<double>(samples[idx]) / cpns;
    }

    /**
     * @brief Internal helper to write the formatted markdown table to a stream.
     * Used for automated CI/CD performance regression tracking.
     */
    static void write_markdown_table(std::ostream& out,
                                     const std::string& label,
                                     double min_ns,
                                     double median_ns,
                                     double p999_ns,
                                     double max_ns) {
        out << "### Benchmark: " << label << "\n";
        out << "| Metric | Latency (ns) |\n";
        out << "| :--- | :--- |\n";
        out << std::format("| Minimum | {:.2f} |\n", min_ns);
        out << std::format("| Median (P50) | {:.2f} |\n", median_ns);
        out << std::format("| P99.9 | {:.2f} |\n", p999_ns);
        out << std::format("| Maximum | {:.2f} |\n\n", max_ns);
    }

public:
    /**
     * @brief Construct a new Metric engine with a fixed capacity.
     * @param max_samples Total samples to pre-allocate.
     * @note Memory is resized in the constructor to trigger all Page Faults
     * during the initialization phase, rather than the measurement phase.
     */
    explicit PorthMetric(size_t max_samples = DEFAULT_METRIC_CAPACITY) : m_capacity(max_samples) {
        m_samples.resize(m_capacity, 0);
    }

    /**
     * @brief record(): Stores a latency sample in the zero-jitter buffer.
     * * Hot-path telemetry: Performs a bounds check and a direct array write.
     * * @param latency The raw cycle count delta (from PorthClock).
     * @note This function is 'noexcept' and allocation-free to ensure it
     * does not interfere with the high-speed code it is measuring.
     */
    void record(uint64_t latency) noexcept {
        if (m_count < m_capacity) {
            m_samples[m_count++] = latency;
        }
    }

    /**
     * @brief save_to_file(): Exports raw telemetry for external visualization (e.g.,
     * Python/Gnuplot).
     * @param filename Path to the output file.
     */
    void save_to_file(const std::string& filename) {
        std::ofstream out(filename);
        if (!out.is_open()) {
            return;
        }

        for (size_t i = 0; i < m_count; ++i) {
            out << i << " " << m_samples[i] << "\n";
        }
    }

    /**
     * @brief print_stats(): Performs jitter analysis and outputs to console.
     * * Converts raw hardware cycles into nanoseconds based on the system clock.
     * * @param cycles_per_ns Frequency constant from calibration.
     * @note Focuses on IQR and P99.99 to identify outlier noise caused by
     * OS interrupts or thermal throttling on the InP/GaN lattice.
     */
    void print_stats(double cycles_per_ns) {
        if (m_count == 0) {
            return;
        }

        const auto sorted_samples = get_sorted_samples();

        const double mean_cycles  = calculate_mean(sorted_samples);
        const double stdev_cycles = calculate_stdev(sorted_samples, mean_cycles);

        const double q1_ns  = get_percentile_ns(sorted_samples, PERCENTILE_Q1, cycles_per_ns);
        const double q3_ns  = get_percentile_ns(sorted_samples, PERCENTILE_Q3, cycles_per_ns);
        const double p99_ns = get_percentile_ns(sorted_samples, PERCENTILE_P99_99, cycles_per_ns);

        std::cout << "\n--- Porth-IO Jitter Analysis (ns) ---\n";
        std::cout << std::format("Mean:    {:.2f} ns\n", mean_cycles / cycles_per_ns);
        std::cout << std::format("StDev:   {:.2f} ns\n", stdev_cycles / cycles_per_ns);
        std::cout << std::format("IQR:     {:.2f} ns\n", q3_ns - q1_ns); // Measures consistency
        std::cout << std::format("P99.99:  {:.2f} ns\n", p99_ns); // Measures worst-case jitter
    }

    /**
     * @brief save_markdown_report(): Generates an automated summary table for CI/CD documentation.
     * * @param filename File path to append to.
     * @param label The name of the benchmark run (e.g., "Shuttle-Throughput").
     * @param cycles_per_ns Calibration constant for conversion.
     */
    void
    save_markdown_report(const std::string& filename // NOLINT(bugprone-easily-swappable-parameters)
                         ,
                         const std::string& label,
                         double cycles_per_ns) {
        std::ofstream out(filename, std::ios::app);
        if (!out.is_open() || m_count == 0) {
            return;
        }

        const auto sorted_samples = get_sorted_samples();

        const double min_ns    = get_percentile_ns(sorted_samples, 0.0, cycles_per_ns);
        const double median_ns = get_percentile_ns(sorted_samples, PERCENTILE_P50, cycles_per_ns);
        const double p999_ns   = get_percentile_ns(sorted_samples, PERCENTILE_P99_9, cycles_per_ns);
        const double max_ns    = get_percentile_ns(sorted_samples, 100.0, cycles_per_ns);

        write_markdown_table(out, label, min_ns, median_ns, p999_ns, max_ns);
    }

    /** @brief Resets the sample count for a new run.
     * @note Does not deallocate memory; zeroing the counter is an O(1) operation.
     */
    void reset() noexcept { m_count = 0; }

    // Boilerplate compliance for HFT resource management
    PorthMetric(const PorthMetric&)                        = default;
    auto operator=(const PorthMetric&) -> PorthMetric&     = default;
    PorthMetric(PorthMetric&&) noexcept                    = default;
    auto operator=(PorthMetric&&) noexcept -> PorthMetric& = default;
    ~PorthMetric()                                         = default;
};

} // namespace porth