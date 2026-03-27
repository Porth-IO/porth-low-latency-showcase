/**
 * @file PorthRegister.hpp
 * @brief The atomic unit of the Porth-IO Hardware Abstraction Layer (HAL).
 *
 * Porth-IO: The Sovereign Logic Layer
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace porth {

/** * @brief The Cardiff Standard Cache Line (64 Bytes).
 * This constant is the physical foundation of our MMIO map. We hardcode 64 bytes
 * to align with the Newport Cluster's PCIe TLP (Transaction Layer Packet)
 * granularity. This prevents "Cache Line Bouncing" between the CPU and the
 * InP/GaN DMA engine, ensuring deterministic register access.
 */
constexpr size_t PORTH_CACHE_LINE_SIZE = 64;

/**
 * @class PorthRegister
 * @brief The atomic "brick" of the Porth-IO Hardware Abstraction Layer.
 *
 * Enforces 64-byte cache alignment to prevent "False Sharing," where adjacent
 * hardware registers would otherwise compete for the same L1 cache line.
 * It utilizes Acquire/Release semantics to provide a jitter-free synchronization
 * bridge between the Sovereign Logic Layer and physical hardware.
 *
 * @tparam T The integral type of the register. Must be trivially copyable
 * to ensure raw bit-level compatibility with MMIO space.
 */
template <typename T>
class alignas(PORTH_CACHE_LINE_SIZE) PorthRegister {
    static_assert(std::is_integral_v<T>,
                  "PorthRegister only accepts integer types to match hardware bit-widths.");
    static_assert(std::is_trivially_copyable_v<T>,
                  "PorthRegister types must be trivially copyable for direct MMIO mapping.");
    static_assert(
        std::atomic<T>::is_always_lock_free,
        "PorthRegister must be lock-free; hardware registers cannot wait on software mutexes.");

private:
    /** * @brief The raw value mapped to hardware.
     * Aligned to std::atomic_ref requirements to ensure that the hardware
     * bus transaction (e.g., PCIe Read/Write) is atomic at the architectural level.
     */
    alignas(std::atomic_ref<T>::required_alignment) T m_value{};

    /** * @brief Hard Padding for Cache-Line Isolation.
     * This space is explicitly reserved to prevent the compiler from placing
     * unrelated objects in the same 64-byte window. This guarantees that
     * the MESI coherency protocol only triggers for this specific register.
     * We use std::array to satisfy modern C++ safety guidelines without latency impact.
     */
    std::array<std::byte, PORTH_CACHE_LINE_SIZE - sizeof(T)> m_padding{};

public:
    /** @brief Default constructor. Used when mapping the PorthDeviceLayout over an existing memory
     * region. */
    PorthRegister() = default;

    /** @brief Destructor. No-op as memory is typically managed by the PorthEmulatedDevice or
     * PorthHugePage. */
    ~PorthRegister() = default;

    // Hardware registers represent fixed physical silicon locations.
    // Copying or moving them is logically impossible and would violate hardware sovereignty.
    PorthRegister(const PorthRegister&)                    = delete;
    auto operator=(const PorthRegister&) -> PorthRegister& = delete;
    PorthRegister(PorthRegister&&)                         = delete;
    auto operator=(PorthRegister&&) -> PorthRegister&      = delete;

    /**
     * @brief Reads the register value using Acquire semantics.
     * * Utilizes 'std::atomic_ref' to treat the MMIO memory as a volatile atomic.
     * @return T The current hardware state.
     * @note memory_order_acquire ensures that all subsequent reads in the
     * software are ordered after this hardware read, preventing stale data
     * processing in high-speed event loops.
     */
    [[nodiscard]] auto load() const noexcept -> T {
        return std::atomic_ref<const T>(m_value).load(std::memory_order_acquire);
    }

    /**
     * @brief Writes a value to hardware using Release semantics.
     * * Ensures the store is visible to the Newport Cluster interconnect.
     * @param val The bitmask or value to commit to the register.
     * @note memory_order_release ensures that all prior software writes
     * (e.g., preparing a DMA descriptor) are globally visible before this
     * register toggle (e.g., "Start DMA") reaches the hardware.
     */
    auto write(T val) noexcept -> void {
        std::atomic_ref<T>(m_value).store(val, std::memory_order_release);
    }

    /** @brief Overload for assignment. Syntactic sugar for 'write()'. */
    auto operator=(T val) noexcept -> PorthRegister& {
        write(val);
        return *this;
    }

    /** @brief Overload for conversion. Syntactic sugar for 'load()'. */
    operator T() const noexcept { return load(); }
};

/** * @brief The Sovereign Guard.
 * Final verification that the compiler has not introduced padding that
 * would shift our MMIO offsets. If this fails, the logic layer is no
 * longer compatible with the Newport Cluster PDK.
 */
static_assert(sizeof(PorthRegister<uint32_t>) == PORTH_CACHE_LINE_SIZE,
              "PorthRegister size mismatch: physical memory map integrity compromised.");

} // namespace porth