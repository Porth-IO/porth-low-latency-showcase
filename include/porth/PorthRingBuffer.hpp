/**
 * @file PorthRingBuffer.hpp
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) ring buffer for DMA telemetry.
 *
 * Porth-IO: The Sovereign Logic Layer
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <vector>

/**
 * @namespace gsl
 * @brief Minimal Guideline Support Library implementation to satisfy clang-tidy ownership checks.
 */
namespace gsl {
template <typename T>
using owner = T;
} // namespace gsl

namespace porth {

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
using std::hardware_destructive_interference_size;
#else
// Fallback for architectures or compilers that don't yet export the size.
// 64 is the safe industrial standard for x86 and current ARM servers.
constexpr size_t hardware_destructive_interference_size = 64;
#endif

/** * @brief Hardware-Aware cache line size.
 * Using the compiler-provided interference size ensures the Porth logic layer
 * is perfectly optimized for the specific "Sovereign" silicon it is compiled for.
 */
constexpr size_t RING_CACHE_LINE_SIZE = hardware_destructive_interference_size;

/** @brief Default ring buffer capacity. */
constexpr size_t DEFAULT_RING_CAPACITY = 1024;

/**
 * @struct PorthDescriptor
 * @brief The "Cargo" box for DMA transactions.
 *
 * This structure is the primary unit of exchange between the CPU and the
 * Newport Cluster's InP/GaN data plane.
 * * @note addr must point to a HugePage-backed region for DMA-Sovereignty.
 */
struct PorthDescriptor {
    uint64_t addr; ///< Physical or Virtual memory address for the DMA engine.
    uint32_t len;  ///< Length of the data buffer in bytes.
};

// Physical Data Audit
static_assert(
    std::is_standard_layout_v<PorthDescriptor>,
    "PorthDescriptor must be Standard Layout for binary compatibility with the PCIe TLP format.");
static_assert(std::is_trivially_copyable_v<PorthDescriptor>,
              "PorthDescriptor must be Trivially Copyable for zero-copy memory transfers.");

/**
 * @class PorthRingBuffer
 * @brief A high-performance SPSC Lock-Free Queue.
 *
 * Optimized for Zero-Copy data transfer between Hardware and Application.
 * This implementation enforces strict cache-line separation between the
 * producer (head) and consumer (tail) to maximize throughput on many-core systems.
 *
 * @tparam SIZE Number of descriptors. Must be a power of two to allow
 * the use of bitwise AND instead of the integer division (modulo) instruction.
 */
template <size_t SIZE = DEFAULT_RING_CAPACITY>
class PorthRingBuffer {
    // Bitwise Wrap-around Guard: Ensures (index & (SIZE - 1)) works as a modulo.
    static_assert((SIZE & (SIZE - 1)) == 0,
                  "SIZE must be a power of two to avoid costly division instructions.");

private:
    /** * @brief Pointer to the underlying descriptor array.
     * Marked as gsl::owner to denote that this class manages the memory
     * lifecycle if an external HugePage pointer is not provided.
     */
    gsl::owner<PorthDescriptor*> m_buffer = nullptr;
    bool m_owns_buffer                    = false; ///< RAII flag for memory lifecycle management.

    // Cache-line 1: The Producer's territory (Typically the Chip or TX side).
    // Aligned to 64 bytes to ensure the producer thread owns this cache line exclusively.
    alignas(RING_CACHE_LINE_SIZE) std::atomic<uint32_t> m_head{0};
    std::array<uint8_t, RING_CACHE_LINE_SIZE - sizeof(std::atomic<uint32_t>)> m_pad0{};

    // Cache-line 2: The Consumer's territory (Typically the Driver or RX side).
    // Isolated to prevent the consumer's 'tail' updates from invalidating the producer's L1 cache.
    alignas(RING_CACHE_LINE_SIZE) std::atomic<uint32_t> m_tail{0};
    std::array<uint8_t, RING_CACHE_LINE_SIZE - sizeof(std::atomic<uint32_t>)> m_pad1{};

    // Ensure the rest of the class doesn't bleed into the tail's cache line
    alignas(RING_CACHE_LINE_SIZE) std::array<char, 0> m_final_pad{};

public:
    /**
     * @brief Constructor: Supports wrapping existing HugePage memory for hardware-visibility.
     * * @param external_buffer Optional pointer to pre-allocated, pinned DMA memory.
     * @note If external_buffer is null, the buffer is allocated on the heap,
     * which is suitable for SIL testing but not for physical InP hardware deployment.
     */
    explicit PorthRingBuffer(PorthDescriptor* external_buffer = nullptr)
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        : m_buffer(external_buffer), m_owns_buffer(external_buffer == nullptr) {
        if (m_owns_buffer) {
            m_buffer = new PorthDescriptor[SIZE];
        }
    }

    /**
     * @brief Destructor: Ensures owned memory is released.
     * Does not touch external_buffer if m_owns_buffer is false (Sovereign Ownership).
     */
    ~PorthRingBuffer() {
        if (m_owns_buffer) {
            delete[] m_buffer;
        }
    }

    /**
     * @brief push() - Executed by the Producer thread.
     * * Adds a new descriptor to the ring.
     * * @param desc The descriptor to push.
     * @return true if successful, false if the ring is full.
     * @note Uses memory_order_release to ensure the descriptor content is
     * globally visible to the consumer BEFORE the head index update is committed.
     */
    [[nodiscard]] auto push(const PorthDescriptor& desc) noexcept -> bool {
        const uint32_t h = m_head.load(std::memory_order_relaxed);
        const uint32_t t = m_tail.load(std::memory_order_acquire);

        // Check if full: next head would hit tail.
        // The bitwise AND is used here as a zero-cost modulo.
        if (((h + 1) & (SIZE - 1)) == t) {
            return false;
        }

        // Write the data to the current head slot.
        m_buffer[h] = desc;

        // Release: Ensures all previous memory writes are visible to the consumer core.
        m_head.store((h + 1) & (SIZE - 1), std::memory_order_release);
        return true;
    }

    /**
     * @brief pop() - Executed by the Consumer thread.
     * * Retrieves a descriptor from the ring.
     * * @param out_desc Reference to store the retrieved descriptor.
     * @return true if data was retrieved, false if the ring is empty.
     * @note Uses memory_order_acquire to ensure the descriptor data is fully
     * synchronized from the producer thread before processing begins.
     */
    [[nodiscard]] auto pop(PorthDescriptor& out_desc) noexcept -> bool {
        const uint32_t t = m_tail.load(std::memory_order_relaxed);
        const uint32_t h = m_head.load(std::memory_order_acquire);

        // Check if empty: tail caught up to head.
        if (h == t) {
            return false;
        }

        // Read the data from the current tail slot.
        out_desc = m_buffer[t];

        // Release: Signals to the producer core that this slot is now logically free.
        m_tail.store((t + 1) & (SIZE - 1), std::memory_order_release);
        return true;
    }

    // Hardware-visible structures represent unique logic channels and cannot be copied.
    PorthRingBuffer(const PorthRingBuffer&)                    = delete;
    auto operator=(const PorthRingBuffer&) -> PorthRingBuffer& = delete;

    // Moving is deleted to ensure the physical memory address remains stable
    // for the duration of the hardware session.
    PorthRingBuffer(PorthRingBuffer&&)                    = delete;
    auto operator=(PorthRingBuffer&&) -> PorthRingBuffer& = delete;
};

// Layout Verification for HugePage compatibility.
static_assert(
    std::is_standard_layout_v<PorthRingBuffer<1024>>,
    "PorthRingBuffer layout must be deterministic to survive being mapped onto a HugePage.");

} // namespace porth