/**
 * @file PorthShuttle.hpp
 * @brief Zero-copy orchestrator for mapping memory structures to physical hardware.
 *
 * Porth-IO: The Sovereign Logic Layer
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "PorthHugePage.hpp"
#include "PorthRingBuffer.hpp"
#include "PorthUtil.hpp"
#include <bit>
#include <cstdint>
#include <format>
#include <iostream>
#include <new>

/**
 * @namespace gsl
 * @brief Minimal Guideline Support Library implementation to satisfy clang-tidy ownership checks.
 */
namespace gsl {
template <typename T>
using owner = T;
} // namespace gsl

namespace porth {

/** * @brief Default capacity for the Shuttle ring buffer.
 * Balanced to minimize cache-line walk depth while maintaining burst resilience.
 */
constexpr size_t DEFAULT_SHUTTLE_CAPACITY = 1024;

/** * @brief Standard 2MB HugePage size for memory allocation.
 * This is the atomic unit of the Linux hugetlbfs; using 2MB pages ensures that
 * the entire data plane fits within a single TLB entry, eliminating
 * Page-Walk-induced latency during high-speed Newport Cluster transfers.
 */
constexpr size_t SHUTTLE_PAGE_SIZE = static_cast<size_t>(2) * 1024 * 1024;

/**
 * @class PorthShuttle
 * @brief The Zero-Copy Orchestrator for high-performance DMA.
 *
 * This class handles the "Sovereign Mapping" logic, ensuring the PorthRingBuffer
 * resides within pinned, hardware-visible HugePage memory. By utilizing
 * Placement New, we align the C++ object model directly with the DMA engine's
 * memory view, eliminating CPU-intensive 'memcpy' operations.
 *
 * @tparam Capacity The number of descriptors in the ring. Must be a power of two.
 */
template <size_t Capacity = DEFAULT_SHUTTLE_CAPACITY>
class alignas(RING_CACHE_LINE_SIZE) PorthShuttle {
private:
    /** @brief The underlying HugePage memory allocation.
     * RAII-managed to ensure memory is pinned for the duration of the hardware session.
     */
    PorthHugePage m_memory;

    /** @brief Typed pointer to the ring buffer within the HugePage.
     * Managed via 'Placement New' logic.
     */
    gsl::owner<PorthRingBuffer<Capacity>*> m_ring_ptr = nullptr;

    uint64_t m_device_iova = 0; ///< Device-visible I/O Virtual Address.

public:
    /**
     * @brief Constructor: Initializes the DMA fabric with optional strictness.
     * @param numa_node The target NUMA node for memory affinity.
     * @param strict If true, fails fast if 2MB HugePages or mlock cannot be secured (Lab Mode).
     */
    explicit PorthShuttle(int numa_node = 0, bool strict = false)
        : m_memory(SHUTTLE_PAGE_SIZE, NumaNode(numa_node), strict) {

        // Safety Check: We cannot map non-Standard Layout types because
        // compiler-specific padding would break the Newport hardware's view of memory.
        static_assert(
            std::is_standard_layout_v<PorthRingBuffer<Capacity>>,
            "Cannot map PorthRingBuffer: Type violates Standard Layout rules for MMIO/DMA.");

        // Retrieve the raw base address from the pinned HugePage region.
        void* base_addr = nullptr;
        base_addr       = m_memory.data();

        // Placement New: We construct the C++ object directly onto the hardware-visible memory.
        // This is a zero-copy operation; the CPU and InP/GaN chip now share this exact memory
        // address.
        if (base_addr != nullptr) {
            m_ring_ptr = new (base_addr) PorthRingBuffer<Capacity>();
        }

        // Telemetry logging for the initialization phase.
        std::cout << std::format("[Porth-Shuttle] Zero-Copy Placement New successful at: {}\n",
                                 base_addr);

        // Sovereign Audit: Verify CPU-Memory Co-location
        const int current_node = get_current_numa_node();
        // NOLINTNEXTLINE(bugprone-branch-clone)
        if (current_node != m_memory.node()) {
            std::cerr << std::format(
                "!! [Sovereign-Alert] Performance Hazard: Thread is on NUMA Node {}, "
                "but Memory is locked on Node {}. Cross-socket latency detected.\n",
                current_node,
                m_memory.node());
        } else {
            std::cout << std::format(
                "[Sovereign-Audit] Locality Verified: Thread and Memory co-located on Node {}.\n",
                current_node);
        }
    }

    /**
     * @brief Destructor: Manually releases the placement-mapped object.
     * * Since the object was constructed via Placement New (bypassing the heap allocator),
     * we must explicitly invoke the destructor to ensure atomic indices and internal
     * pointers are cleaned up before the HugePage is unmapped.
     */
    ~PorthShuttle() {
        if (m_ring_ptr != nullptr) {
            // Explicitly call the destructor for the object living in the HugePage.
            m_ring_ptr->~PorthRingBuffer();
        }
    }

    /** @brief set_device_iova: Sets the IOVA provided by the VFIO/IOMMU layer. */
    void set_device_iova(uint64_t iova) noexcept { m_device_iova = iova; }

    /**
     * @brief Returns the DMA-ready physical address for the hardware handshake.
     * @return uint64_t The physical/device address of the memory region.
     * @note This address is written to the 'data_ptr' register in the PorthDeviceLayout.
     */
    [[nodiscard]] auto get_device_addr() const noexcept -> uint64_t {
        // If an IOVA was mapped via VFIO/IOMMU, hardware MUST use that address.
        // Otherwise, we fallback to the standard virtual address (Simulator mode).
        if (m_device_iova != 0) {
            return m_device_iova;
        }
        return std::bit_cast<uint64_t>(m_memory.data());
    }

    /** @brief Access the zero-copy ring buffer for data transmission. */
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    [[nodiscard]] auto ring() noexcept -> PorthRingBuffer<Capacity>* { return m_ring_ptr; }

    /** @brief Const access to the zero-copy ring buffer for telemetry. */
    [[nodiscard]] auto ring() const noexcept -> const PorthRingBuffer<Capacity>* {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return m_ring_ptr;
    }

    /** @brief Exposes raw virtual address for AF_XDP UMEM registration. */
    [[nodiscard]] auto get_raw_memory_ptr() const noexcept -> void* { return m_memory.data(); }

    /** @brief Exposes total allocation size for AF_XDP UMEM registration. */
    [[nodiscard]] auto get_raw_memory_size() const noexcept -> size_t { return m_memory.size(); }

    // Hardware-mapped orchestrators cannot be copied or moved to prevent
    // memory aliasing and illegal DMA access to unmapped regions.
    PorthShuttle(const PorthShuttle&)                    = delete;
    auto operator=(const PorthShuttle&) -> PorthShuttle& = delete;
    PorthShuttle(PorthShuttle&&)                         = delete;
    auto operator=(PorthShuttle&&) -> PorthShuttle&      = delete;
};

} // namespace porth