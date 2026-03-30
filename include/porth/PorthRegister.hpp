/**
 * @file PorthRegister.hpp
 * @brief The atomic unit of the Porth-IO Hardware Abstraction Layer (HAL).
 *
 * Porth-IO: Low Latency Showcase
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace porth {

/** * @brief Standard Cache Line Size (64 Bytes).
 * This constant defines the physical alignment of the MMIO map. 64 bytes is 
 * chosen to match the PCIe TLP (Transaction Layer Packet) granularity and the 
 * standard L1 cache line size. This prevents "Cache Line Bouncing" between 
 * the CPU and the DMA engine, ensuring deterministic register access.
 */
constexpr size_t PORTH_CACHE_LINE_SIZE = 64;

/**
 * @class PorthRegister
 * @brief The atomic primitive for the Hardware Abstraction Layer (HAL).
 *
 * This class enforces 64-byte cache alignment to prevent "False Sharing," where 
 * adjacent hardware registers would otherwise compete for the same L1 cache line 
 * during concurrent access. It utilizes C++ atomic Acquire/Release semantics 
 * to provide a jitter-minimized synchronization bridge between the logic layer 
 * and physical hardware registers.
 *
 * @tparam T The integral type of the register. Must be trivially copyable 
 * to ensure binary compatibility with MMIO address space.
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
    /** * @brief The raw value mapped to hardware memory space.
     * Aligned to satisfy architectural requirements, ensuring that the hardware 
     * bus transaction (e.g., PCIe Read/Write) is atomic at the memory-controller level.
     */
    alignas(std::atomic_ref<T>::required_alignment) T m_value{};

    /** * @brief Hard Padding for Cache-Line Isolation.
     * This space is explicitly reserved to prevent the compiler or linker from 
     * placing unrelated objects in the same 64-byte window. This guarantees that 
     * the MESI coherency protocol only triggers for this specific register, 
     * eliminating unnecessary cache invalidation traffic.
     */
    std::array<std::byte, PORTH_CACHE_LINE_SIZE - sizeof(T)> m_padding{};

public:
    /** @brief Default constructor. Used when mapping the layout over an existing MMIO region. */
    PorthRegister() = default;

    /** @brief Destructor. No-op as lifetime is managed by the mapping layer. */
    ~PorthRegister() = default;

    // Hardware registers represent fixed physical silicon locations.
    // Copying or moving them is logically invalid for an MMIO-mapped primitive.
    PorthRegister(const PorthRegister&)                    = delete;
    auto operator=(const PorthRegister&) -> PorthRegister& = delete;
    PorthRegister(PorthRegister&&)                         = delete;
    auto operator=(PorthRegister&&) -> PorthRegister&      = delete;

    /**
     * @brief Reads the register value using Acquire semantics.
     *
     * Utilizes 'std::atomic_ref' to treat the memory as a volatile atomic.
     * * @return T The current hardware state.
     * @note memory_order_acquire ensures that subsequent software reads are 
     * ordered after this hardware read, preventing the processing of stale 
     * data in high-speed event loops.
     */
    [[nodiscard]] auto load() const noexcept -> T {
        return std::atomic_ref<const T>(m_value).load(std::memory_order_acquire);
    }

    /**
     * @brief Writes a value to hardware using Release semantics.
     *
     * Ensures the store is committed to the hardware interconnect.
     * * @param val The bitmask or value to commit to the physical register.
     * @note memory_order_release ensures that all prior software writes 
     * (e.g., preparing a DMA descriptor) are globally visible before this 
     * register toggle (e.g., "Start DMA") reaches the hardware bus.
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

/** * @brief Hardware Integrity Guard.
 * Final verification that the compiler has not introduced padding that 
 * would shift MMIO offsets. If this fails, the binary layout is no longer 
 * compatible with the hardware register map.
 */
static_assert(sizeof(PorthRegister<uint32_t>) == PORTH_CACHE_LINE_SIZE,
              "PorthRegister size mismatch: physical memory map integrity compromised.");

} // namespace porth