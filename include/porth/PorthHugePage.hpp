/**
 * @file PorthHugePage.hpp
 * @brief NUMA-aware RAII wrapper for HugePage memory sovereignty.
 *
 * Porth-IO: The Sovereign Logic Layer
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <numa.h>
#include <numaif.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

// Minimal GSL-like owner tag to satisfy Clang-Tidy ownership rules.
namespace gsl {
template <typename T>
using owner = T;
}

namespace porth {

/** @brief Strongly typed NUMA node to prevent swappable parameter errors. */
struct NumaNode {
    int value;
    explicit NumaNode(int v) : value(v) {}
};

/**
 * @class PorthHugePage
 * @brief RAII wrapper for pinning memory to 2MB HugePage boundaries with NUMA affinity.
 */
class PorthHugePage {
private:
    gsl::owner<void*> m_ptr = nullptr; ///< Base address of the mapped memory region.
    size_t m_total_size;               ///< Total size after alignment to HugePage boundaries.
    int m_node;                        ///< The target NUMA node for this allocation.
    bool m_is_numa_managed{false};     ///< Tracks if libnuma was used for allocation.
    bool m_is_mmaped = false;

    static constexpr size_t HP_SIZE = static_cast<size_t>(2) * 1024 * 1024;

    /** @brief Bits per byte constant to eliminate magic numbers. */
    static constexpr size_t BITS_PER_BYTE = 8;

    /** @brief Internal helper to handle the initial memory acquisition. */
    void allocate_initial_buffer() {
        // Verify NUMA node is available before allocation
        if (numa_available() < 0) {
            std::cerr << "[Porth-IO] Warning: NUMA support not available in kernel.\n";
            m_ptr             = std::aligned_alloc(HP_SIZE, m_total_size);
            m_is_numa_managed = false;
            return;
        }

        // Strict allocation on the target node.
        // This ensures the logic layer and Newport hardware share the same local memory bus.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        m_ptr = numa_alloc_onnode(m_total_size, m_node);

        if (m_ptr == nullptr) {
            std::cerr << std::format("[Porth-IO] Critical: Allocation failed on NUMA node {}.\n",
                                     m_node);
            m_ptr             = std::aligned_alloc(HP_SIZE, m_total_size);
            m_is_numa_managed = false;
        } else {
            m_is_numa_managed = true;
            // Explicitly bind the memory policy to this node to prevent pages from migrating.
            unsigned long nodemask = (1UL << m_node);
            if (set_mempolicy(MPOL_BIND, &nodemask, (sizeof(nodemask) * BITS_PER_BYTE) + 1) != 0) {
                std::cerr << "[Porth-IO] Warning: Could not set strict MPOL_BIND policy.\n";
            }
        }
    }

    /** @brief Internal helper to attempt the HugePage upgrade. */
    void attempt_hugepage_upgrade(bool strict) {
        // MAP_LOCKED ensures the memory is pinned in RAM and never swapped to disk.
        // MAP_POPULATE pre-faults the pages to ensure the TLB is primed before use.
        int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_LOCKED | MAP_POPULATE;

        // Declare as owner immediately so the assignment to m_ptr is valid.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        gsl::owner<void*> mapped_ptr =
            mmap(m_ptr, m_total_size, PROT_READ | PROT_WRITE, flags, -1, 0);

        if (mapped_ptr == MAP_FAILED) {
            if (strict) {
                // SOVEREIGN REQUIREMENT: Fatal error for Newport Lab environments
                throw std::runtime_error(
                    "[Fatal] Porth-IO: HugePage upgrade failed in Strict Mode. "
                    "Newport hardware requires physically contiguous 2MB pages.");
            }
            // Contributor/Home Fallback: Standard memory with best-effort pinning
            std::cout << "[Porth-IO] Note: HugePages not supported.\n";
            m_is_mmaped = false;
            if (mlock(m_ptr, m_total_size) != 0) {
                if (strict) {
                    // FATAL in the Lab: We MUST lock memory for Newport performance
                    throw std::runtime_error(
                        "[Fatal] Porth-HugePage: mlock failed in Strict Mode.");
                }
                // WARNING at Home: Allow OrbStack to keep running with a warning
                std::cerr << "[Porth-IO] Warning: Failed to lock memory. Jitter may increase on "
                             "this host.\n";
            }
        } else {
            if (m_is_numa_managed) {
                numa_free(m_ptr, m_total_size);
            } else {
                // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
                std::free(m_ptr);
            }

            m_ptr       = mapped_ptr;
            m_is_mmaped = true;
            std::cout << "[Porth-IO] Sovereign HugePage Upgrade Successful.\n";
        }
    }

public:
    explicit PorthHugePage(size_t size, NumaNode numa_node = NumaNode(0), bool strict = false)
        : m_total_size(((size + HP_SIZE - 1) / HP_SIZE) * HP_SIZE), m_node(numa_node.value) {

        allocate_initial_buffer();

        if (m_ptr == nullptr) {
            throw std::runtime_error("[Porth-IO] Fatal: Total memory allocation failure.");
        }

        attempt_hugepage_upgrade(strict);

        std::cout << std::format(
            "[Porth-IO] Memory Initialized: {} bytes at {}\n", m_total_size, m_ptr);
    }

    /** @brief Destructor: Releases the node-locked mapping. */
    ~PorthHugePage() {
        if (m_ptr != nullptr) {
            if (m_is_mmaped) {
                munmap(m_ptr, m_total_size);
            } else if (m_is_numa_managed) {
                numa_free(m_ptr, m_total_size);
            } else {
                // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
                std::free(m_ptr);
            }
            m_ptr = nullptr;
        }
    }

    PorthHugePage(const PorthHugePage&)                    = delete;
    auto operator=(const PorthHugePage&) -> PorthHugePage& = delete;
    PorthHugePage(PorthHugePage&&)                         = delete;
    auto operator=(PorthHugePage&&) -> PorthHugePage&      = delete;

    [[nodiscard]] auto data() const noexcept -> void* {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return m_ptr;
    }

    [[nodiscard]] auto size() const noexcept -> size_t { return m_total_size; }
    [[nodiscard]] auto node() const noexcept -> int { return m_node; }

    [[nodiscard]] auto get_device_addr() const noexcept -> uint64_t {
        return std::bit_cast<uint64_t>(m_ptr);
    }
};

} // namespace porth