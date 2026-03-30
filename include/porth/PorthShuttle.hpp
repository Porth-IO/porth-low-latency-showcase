/**
 * @file PorthShuttle.hpp
 * @brief Zero-copy orchestrator for mapping memory structures to physical hardware.
 *
 * Porth-IO: Low Latency Showcase
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
 * Balanced to minimize cache-line walk depth while maintaining burst resilience 
 * during high-throughput workloads.
 */
constexpr size_t DEFAULT_SHUTTLE_CAPACITY = 1024;

/** * @brief Standard 2MB HugePage size for memory allocation.
 * This is the atomic unit of the Linux hugetlbfs; utilizing 2MB pages ensures 
 * that the entire data plane fits within a single TLB (Translation Lookaside 
 * Buffer) entry, eliminating Page-Walk-induced latency spikes during 
 * hardware-to-software transfers.
 */
constexpr size_t SHUTTLE_PAGE_SIZE = static_cast<size_t>(2) * 1024 * 1024;

/**
 * @class PorthShuttle
 * @brief The Zero-Copy Orchestrator for high-performance DMA memory management.
 *
 * This class manages the mapping logic required to ensure the PorthRingBuffer 
 * resides within pinned, hardware-visible HugePage memory. By utilizing C++ 
 * Placement New, it aligns the object model directly with the hardware's 
 * memory view, eliminating CPU-intensive 'memcpy' operations and ensuring 
 * sub-microsecond data availability.
 *
 * @tparam Capacity The number of descriptors in the ring. Must be a power of two.
 */
template <size_t Capacity = DEFAULT_SHUTTLE_CAPACITY>
class alignas(RING_CACHE_LINE_SIZE) PorthShuttle {
private:
    /** @brief The underlying HugePage memory allocation.
     * RAII-managed to ensure memory remains pinned and locked for the 
     * duration of the hardware session.
     */
    PorthHugePage m_memory;

    /** @brief Typed pointer to the ring buffer residing within the HugePage.
     * Managed via Placement New logic to align software structures with 
     * the hardware-mapped address space.
     */
    gsl::owner<PorthRingBuffer<Capacity>*> m_ring_ptr = nullptr;

    uint64_t m_device_iova = 0; ///< Device-visible I/O Virtual Address (IOMMU).

public:
    /**
     * @brief Constructor: Initializes the DMA memory fabric with NUMA affinity.
     * @param numa_node The target physical CPU socket for memory locality.
     * @param strict If true, throws if HugePages or memory pinning cannot be secured.
     */
    explicit PorthShuttle(int numa_node = 0, bool strict = false)
        : m_memory(SHUTTLE_PAGE_SIZE, NumaNode(numa_node), strict) {

        // Safety Check: Enforces Standard Layout to prevent compiler-specific 
        // padding from breaking the hardware's binary contract with the memory map.
        static_assert(
            std::is_standard_layout_v<PorthRingBuffer<Capacity>>,
            "Cannot map PorthRingBuffer: Type violates Standard Layout rules for MMIO/DMA.");

        // Retrieve the base address from the pinned, node-local HugePage region.
        void* base_addr = nullptr;
        base_addr       = m_memory.data();

        // Placement New: Constructs the Ring Buffer directly into hardware-visible memory.
        // This achieves true zero-copy communication; the CPU and hardware logic share 
        // the exact same physical memory address.
        if (base_addr != nullptr) {
            m_ring_ptr = new (base_addr) PorthRingBuffer<Capacity>();
        }

        std::cout << std::format("[Porth-Shuttle] Zero-Copy Placement New successful at: {}\n",
                                 base_addr);

        // Locality Audit: Verifies CPU-Memory co-location to prevent cross-socket latency.
        const int current_node = get_current_numa_node();
        if (current_node != m_memory.node()) {
            std::cerr << std::format(
                "!! [Performance-Alert] Locality Breach: Thread is on NUMA Node {}, "
                "but Memory is on Node {}. Cross-socket interconnect latency detected.\n",
                current_node,
                m_memory.node());
        } else {
            std::cout << std::format(
                "[Locality-Audit] Co-location Verified: Thread and Memory pinned to Node {}.\n",
                current_node);
        }
    }

    /**
     * @brief Destructor: Explicitly invokes the placement-mapped object's destructor.
     * Since the object was constructed via Placement New (bypassing the heap allocator), 
     * the destructor must be called manually to ensure atomic state and memory 
     * barriers are resolved before the HugePage is unmapped.
     */
    ~PorthShuttle() {
        if (m_ring_ptr != nullptr) {
            m_ring_ptr->~PorthRingBuffer();
        }
    }

    /** @brief Sets the IOVA (I/O Virtual Address) provided by the VFIO/IOMMU layer. */
    void set_device_iova(uint64_t iova) noexcept { m_device_iova = iova; }

    /**
     * @brief Returns the DMA-ready address for the hardware register handshake.
     * @return uint64_t The physical/device address of the memory region.
     * @note This address is typically written to the 'data_ptr' register in the layout.
     */
    [[nodiscard]] auto get_device_addr() const noexcept -> uint64_t {
        // Prefers the IOMMU-mapped IOVA if available (Physical Hardware Mode).
        // Falls back to the virtual address for Simulator/Digital-Twin mode.
        if (m_device_iova != 0) {
            return m_device_iova;
        }
        return std::bit_cast<uint64_t>(m_memory.data());
    }

    /** @brief Accesses the zero-copy ring buffer for data plane operations. */
    [[nodiscard]] auto ring() noexcept -> PorthRingBuffer<Capacity>* { return m_ring_ptr; }

    /** @brief Provides read-only access to the ring buffer for telemetry. */
    [[nodiscard]] auto ring() const noexcept -> const PorthRingBuffer<Capacity>* {
        return m_ring_ptr;
    }

    /** @brief Returns the base pointer for AF_XDP UMEM registration. */
    [[nodiscard]] auto get_raw_memory_ptr() const noexcept -> void* { return m_memory.data(); }

    /** @brief Returns the total aligned allocation size. */
    [[nodiscard]] auto get_raw_memory_size() const noexcept -> size_t { return m_memory.size(); }

    // Hardware-mapped orchestrators cannot be copied or moved to prevent 
    // memory aliasing and illegal DMA access to unmapped regions.
    PorthShuttle(const PorthShuttle&)                    = delete;
    auto operator=(const PorthShuttle&) -> PorthShuttle& = delete;
    PorthShuttle(PorthShuttle&&)                         = delete;
    auto operator=(PorthShuttle&&) -> PorthShuttle&      = delete;
};

} // namespace porth