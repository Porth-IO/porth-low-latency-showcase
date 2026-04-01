/**
 * @file PorthMetric.hpp
 * @brief Statistical analysis engine for ultra-low latency timing telemetry.
 *
 * Porth-IO: Low Latency Showcase
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
 * 1,000,000 samples provide a statistically significant window for high-speed
 * packet analysis without excessive resident set size (RSS) pressure.
 */
constexpr size_t DEFAULT_METRIC_CAPACITY = 1000000;

/** @brief Percentile constants for statistical jitter and tail-latency analysis. */
constexpr double PERCENTILE_Q1     = 25.0;
constexpr double PERCENTILE_Q3     = 75.0;
constexpr double PERCENTILE_P50    = 50.0;
constexpr double PERCENTILE_P99_9  = 99.9;
constexpr double PERCENTILE_P99_99 = 99.99;

/**
 * @class PorthMetric
 * @brief Statistical analysis engine for high-precision timing telemetry.
 *
 * Optimized for deterministic performance, this class utilizes a pre-allocated 
 * sample buffer to ensure that recording timing data never triggers heap 
 * allocations or context-switch-inducing Page Faults during the "Hot Path."
 */
class PorthMetric {
private:
    /**
     * @brief Pre-allocated sample buffer.
     * Reserving this space upfront protects the measurement cycle from 
     * 'malloc' related jitter and non-deterministic behavior.
     */
    std::vector<uint64_t> m_samples;
    size_t m_capacity;
    size_t m_count = 0;

    /**
     * @brief Prepares a sorted copy of the recorded samples for percentile analysis.
     * @note This is an O(N log N) operation performed outside the measurement loop.
     * We create a copy to preserve the original chronological sequence of events.
     */
    [[nodiscard]] auto get_sorted_samples() const -> std::vector<uint64_t> {
        std::vector<uint64_t> sorted(m_samples.begin(),
                                     m_samples.begin() + static_cast<std::ptrdiff_t>(m_count));
        std::ranges::sort(sorted);
        return sorted;
    }

    /**
     * @brief Calculates the arithmetic mean of the recorded timing samples.
     * @param samples Vector of timing samples (sorted or unsorted).
     * @return double Average cycle count.
     */
    [[nodiscard]] auto calculate_mean(const std::vector<uint64_t>& samples) const noexcept
        -> double {
        const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
        return sum / static_cast<double>(m_count);
    }

    /**
     * @brief Calculates the standard deviation for numerical stability analysis.
     * @param samples Timing samples.
     * @param mean Pre-calculated mean of the sample set.
     * @return double Standard deviation in hardware cycles.
     */
    [[nodiscard]] auto calculate_stdev(const std::vector<uint64_t>& samples,
                                       double mean) const noexcept -> double {
        const double sq_sum =
            std::inner_product(samples.begin(), samples.end(), samples.begin(), 0.0);
        return std::sqrt(std::abs((sq_sum / static_cast<double>(m_count)) - (mean * mean)));
    }

    /**
     * @brief Retrieves a specific percentile and converts hardware cycles to nanoseconds.
     * @param samples Sorted timing samples.
     * @param percentile Target percentile (0.0 to 100.0).
     * @param cpns Cycles Per Nanosecond calibration constant.
     * @return double Latency value in nanoseconds.
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
     * @brief Internal helper to write a formatted markdown table for CI/CD metrics.
     */
    static void write_markdown_table(std::ostream& out,
                                     const std::string& label,
                                     double mean_ns,
                                     double iqr_ns,
                                     double p9999_ns,
                                     double stdev_ns) {
        out << "### Benchmark: " << label << "\n";
        out << "| Metric | Latency (ns) |\n";
        out << "| :--- | :--- |\n";
        out << std::format("| Mean | {:.2f} |\n", mean_ns);
        out << std::format("| Jitter (IQR) | {:.2f} |\n", iqr_ns);
        out << std::format("| Tail (P99.99) | {:.2f} |\n", p9999_ns);
        out << std::format("| Standard Deviation | {:.2f} |\n\n", stdev_ns);
    }

public:
    /**
     * @brief Construct a new Metric engine with a fixed capacity.
     * @param max_samples Total samples to pre-allocate.
     * @note Memory is initialized and paged in the constructor to prevent
     * mid-run Page Faults during high-speed data collection.
     */
    explicit PorthMetric(size_t max_samples = DEFAULT_METRIC_CAPACITY) : m_capacity(max_samples) {
        m_samples.resize(m_capacity, 0);
    }

    /**
     * @brief record(): Stores a hardware cycle sample in the zero-jitter buffer.
     *
     * This function is 'noexcept' and allocation-free, designed to be used in
     * time-critical code paths without introducing measurement bias.
     *
     * @param latency The raw hardware cycle count delta.
     */
    void record(uint64_t latency) noexcept {
        if (m_count < m_capacity) {
            m_samples[m_count++] = latency;
        }
    }

    /**
     * @brief save_to_file(): Exports raw telemetry for secondary analysis (CSV/Dat).
     * @param filename Path to the target output file.
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
     * @brief print_stats(): Performs jitter analysis and outputs to standard console.
     * * Focuses on Interquartile Range (IQR) and tail percentiles (P99.99) to identify
     * outliers caused by OS noise or thermal-induced lattice drift.
     *
     * @param cycles_per_ns System-specific clock calibration constant.
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
        std::cout << std::format("IQR:     {:.2f} ns\n", q3_ns - q1_ns); // Measures jitter consistency
        std::cout << std::format("P99.99:  {:.2f} ns\n", p99_ns); // Measures worst-case jitter outliers
    }

    /**
     * @brief save_markdown_report(): Generates a summary for automated documentation.
     * @param filename File path to append the report to.
     * @param label The descriptor for this specific benchmark run.
     * @param cycles_per_ns System-specific clock calibration constant.
     */
    void
    save_markdown_report(const std::string& filename, const std::string& label, double cycles_per_ns) {
        std::ofstream out(filename, std::ios::app);
        if (!out.is_open() || m_count == 0) {
            return;
        }

        const auto sorted_samples = get_sorted_samples();

        const double mean_ns   = calculate_mean(sorted_samples) / cycles_per_ns;
        const double stdev_ns  = calculate_stdev(sorted_samples, calculate_mean(sorted_samples)) / cycles_per_ns;
        const double q1_ns     = get_percentile_ns(sorted_samples, PERCENTILE_Q1, cycles_per_ns);
        const double q3_ns     = get_percentile_ns(sorted_samples, PERCENTILE_Q3, cycles_per_ns);
        const double p9999_ns  = get_percentile_ns(sorted_samples, PERCENTILE_P99_99, cycles_per_ns);

        write_markdown_table(out, label, mean_ns, q3_ns - q1_ns, p9999_ns, stdev_ns);
    }

    /** @brief Resets the sample count for a new measurement run without reallocating. */
    void reset() noexcept { m_count = 0; }

    PorthMetric(const PorthMetric&)                        = default;
    auto operator=(const PorthMetric&) -> PorthMetric&     = default;
    PorthMetric(PorthMetric&&) noexcept                    = default;
    auto operator=(PorthMetric&&) noexcept -> PorthMetric& = default;
    ~PorthMetric()                                         = default;
};

} // namespace porth