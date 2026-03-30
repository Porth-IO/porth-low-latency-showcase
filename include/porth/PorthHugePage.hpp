/**
 * @file PorthHugePage.hpp
 * @brief NUMA-aware RAII wrapper for HugePage memory pinning.
 *
 * Porth-IO: Low Latency Showcase
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
 *
 * This class ensures that memory allocated for the DMA data plane is physically 
 * contiguous and locked in RAM. By utilizing 2MB HugePages, the system minimizes 
 * TLB (Translation Lookaside Buffer) misses, which are a primary source of 
 * non-deterministic latency in high-speed hardware-software handshakes.
 */
class PorthHugePage {
private:
    gsl::owner<void*> m_ptr = nullptr; ///< Base address of the mapped memory region.
    size_t m_total_size;               ///< Total size after alignment to HugePage boundaries.
    int m_node;                        ///< The target NUMA node for this allocation.
    bool m_is_numa_managed{false};     ///< Tracks if libnuma was used for allocation.
    bool m_is_mmaped = false;          ///< Tracks if the memory is mapped via mmap.

    /** @brief Standard 2MB HugePage size. */
    static constexpr size_t HP_SIZE = static_cast<size_t>(2) * 1024 * 1024;

    /** @brief Bits per byte constant. */
    static constexpr size_t BITS_PER_BYTE = 8;

    /** * @brief Internal helper to handle the initial memory acquisition. 
     * Attempts to secure memory on the specific NUMA node where the logic 
     * execution thread is pinned to ensure maximum memory bus locality.
     */
    void allocate_initial_buffer() {
        // Verify NUMA support is available before attempting node-specific allocation.
        if (numa_available() < 0) {
            std::cerr << "[Porth-IO] Warning: NUMA support not available in kernel.\n";
            m_ptr             = std::aligned_alloc(HP_SIZE, m_total_size);
            m_is_numa_managed = false;
            return;
        }

        // Strict allocation on the target NUMA node.
        // This ensures the logic layer and hardware share the same local memory bus,
        // eliminating cross-socket interconnect latency.
        m_ptr = numa_alloc_onnode(m_total_size, m_node);

        if (m_ptr == nullptr) {
            std::cerr << std::format("[Porth-IO] Critical: Allocation failed on NUMA node {}.\n",
                                     m_node);
            m_ptr             = std::aligned_alloc(HP_SIZE, m_total_size);
            m_is_numa_managed = false;
        } else {
            m_is_numa_managed = true;
            // Explicitly bind the memory policy to this node to prevent page migration by the kernel.
            unsigned long nodemask = (1UL << m_node);
            if (set_mempolicy(MPOL_BIND, &nodemask, (sizeof(nodemask) * BITS_PER_BYTE) + 1) != 0) {
                std::cerr << "[Porth-IO] Warning: Could not set strict MPOL_BIND policy.\n";
            }
        }
    }

    /** * @brief Internal helper to attempt the HugePage upgrade. 
     * Uses mmap with MAP_HUGETLB to secure physically contiguous memory pages.
     */
    void attempt_hugepage_upgrade(bool strict) {
        // MAP_LOCKED ensures memory is pinned in RAM and never swapped to disk.
        // MAP_POPULATE pre-faults the pages to ensure the TLB is primed before measurement begins.
        int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_LOCKED | MAP_POPULATE;

        gsl::owner<void*> mapped_ptr =
            mmap(m_ptr, m_total_size, PROT_READ | PROT_WRITE, flags, -1, 0);

        if (mapped_ptr == MAP_FAILED) {
            if (strict) {
                // Fatal error for high-reliability lab environments where HugePages are required.
                throw std::runtime_error(
                    "[Fatal] Porth-IO: HugePage upgrade failed in Strict Mode. "
                    "Physical hardware requires contiguous 2MB pages for deterministic DMA.");
            }
            
            // Best-effort fallback for non-HugePage enabled hosts.
            std::cout << "[Porth-IO] Note: HugePages not supported. Falling back to standard pages.\n";
            m_is_mmaped = false;
            if (mlock(m_ptr, m_total_size) != 0) {
                if (strict) {
                    throw std::runtime_error(
                        "[Fatal] Porth-HugePage: mlock failed in Strict Mode.");
                }
                std::cerr << "[Porth-IO] Warning: Failed to lock memory. Jitter may increase.\n";
            }
        } else {
            // If successful, release the initial buffer and replace with the HugePage mapping.
            if (m_is_numa_managed) {
                numa_free(m_ptr, m_total_size);
            } else {
                std::free(m_ptr);
            }

            m_ptr       = mapped_ptr;
            m_is_mmaped = true;
            std::cout << "[Porth-IO] HugePage Upgrade Successful.\n";
        }
    }

public:
    /**
     * @brief Construct a new Porth-HugePage.
     * @param size Total requested size.
     * @param numa_node The physical CPU socket to bind the memory to.
     * @param strict If true, throws an exception if HugePages cannot be secured.
     */
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

    /** @brief Destructor: Releases the node-locked mapping and unpins memory. */
    ~PorthHugePage() {
        if (m_ptr != nullptr) {
            if (m_is_mmaped) {
                munmap(m_ptr, m_total_size);
            } else if (m_is_numa_managed) {
                numa_free(m_ptr, m_total_size);
            } else {
                std::free(m_ptr);
            }
            m_ptr = nullptr;
        }
    }

    PorthHugePage(const PorthHugePage&)                    = delete;
    auto operator=(const PorthHugePage&) -> PorthHugePage& = delete;
    PorthHugePage(PorthHugePage&&)                         = delete;
    auto operator=(PorthHugePage&&) -> PorthHugePage&      = delete;

    /** @brief Returns the base address of the pinned memory region. */
    [[nodiscard]] auto data() const noexcept -> void* {
        return m_ptr;
    }

    /** @brief Returns the total aligned size of the allocation. */
    [[nodiscard]] auto size() const noexcept -> size_t { return m_total_size; }
    
    /** @brief Returns the NUMA node index for this allocation. */
    [[nodiscard]] auto node() const noexcept -> int { return m_node; }

    /** @brief Returns the raw 64-bit address for hardware mapping. */
    [[nodiscard]] auto get_device_addr() const noexcept -> uint64_t {
        return std::bit_cast<uint64_t>(m_ptr);
    }
};

} // namespace porth