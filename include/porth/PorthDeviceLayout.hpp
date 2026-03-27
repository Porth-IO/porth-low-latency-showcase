/**
 * @file PorthDeviceLayout.hpp
 * @brief Physical memory map definition for Porth-IO hardware.
 *
 * Porth-IO: The Sovereign Logic Layer
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "PorthRegister.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace porth {

/** * @brief Standard x86_64/ARM64 cache line size.
 * Aligned to 64 bytes to prevent "False Sharing" where the CPU's L1 cache
 * coherency protocol (MESI/MOESI) would otherwise force unnecessary
 * synchronization between registers in the same line.
 */
constexpr size_t cache_line_alignment = 64;

/** * @brief Total footprint of the MMIO region.
 * Must remain exactly 512 bytes to match the FPGA/ASIC register file RTL.
 */
constexpr size_t expected_layout_size = 512;

// --- MMIO Layout Offsets ---
// These offsets are strictly defined by the Physical Design Kit (PDK).
// Each register is spaced by 64 bytes (one cache line) to ensure that
// frequent writes to 'status' do not invalidate the 'control' register in the CPU cache.

constexpr size_t offset_control     = 0x00;
constexpr size_t offset_status      = 0x40;
constexpr size_t offset_data_ptr    = 0x80;
constexpr size_t offset_counter     = 0xC0;
constexpr size_t offset_laser_temp  = 0x100;
constexpr size_t offset_gan_voltage = 0x140;
constexpr size_t offset_rf_snr      = 0x180;
constexpr size_t offset_safety_trip = 0x1C0;

/**
 * @struct PorthDeviceLayout
 * @brief The physical memory-mapped I/O (MMIO) layout for Porth-compatible hardware.
 *
 * This structure defines the precise register map for interaction with
 * Newport Cluster compound semiconductor chips (InP/GaN).
 * * The layout utilizes "Hardware Isolation Padding" (spacing registers by 64 bytes).
 * This ensures that a PCIe Write TLP (Transaction Layer Packet) targeting one
 * register does not inadvertently affect the cache-line-granular prefetch
 * logic of the CPU for adjacent registers.
 *
 * @note This struct MUST be mapped via 'mmap' with the MAP_SHARED flag over a
 * UIO or VFIO file descriptor to bypass the kernel stack.
 */
struct alignas(cache_line_alignment) PorthDeviceLayout {
    /** * @brief Offset 0x00: Master control register.
     * Manages Start/Stop/Reset state machines. Writing 0x1 triggers
     * the hardware-level sequencer for GaN power-on.
     */
    PorthRegister<uint32_t> control;

    /** * @brief Offset 0x40: Device status register.
     * Indicates Ready/Busy/Error. Poll this register after a reset
     * to ensure the InP lattice has stabilized.
     */
    PorthRegister<uint32_t> status;

    /** * @brief Offset 0x80: Data Plane pointer.
     * Stores the 64-bit physical DMA base address. The hardware uses this
     * to fetch payloads directly from HugePage memory via the Shuttle interconnect.
     */
    PorthRegister<uint64_t> data_ptr;

    /** * @brief Offset 0xC0: Telemetry counter.
     * Tracks packet/work-unit throughput. Designed for 64-bit hardware
     * wrap-around; the software layer should handle monotonic increments.
     */
    PorthRegister<uint64_t> counter;

    /** * @brief Offset 0x100: Photonics Laser Temperature.
     * Unit: milli-Celsius (mC).
     * @note Operational Limit: 45,000mC. Beyond this, Indium Phosphide
     * lattice drift renders the signal-to-noise ratio (SNR) unusable.
     */
    PorthRegister<uint32_t> laser_temp;

    /** * @brief Offset 0x140: GaN Power Stage Voltage.
     * Unit: milli-Volts (mV).
     * Tracks the drain-source voltage of the Gallium Nitride power FETs.
     */
    PorthRegister<uint32_t> gan_voltage;

    /** * @brief Offset 0x180: RF Signal-to-Noise Ratio.
     * Unit: scaled dB (Value = dB * 100).
     * Represents the integrity of the high-speed RF mode coupling.
     */
    PorthRegister<int32_t> rf_snr;

    /** @brief Offset 0x1C0: Hardware Emergency Trip.
     * Write 0xDEADBEEF to trigger an immediate hardware-level shutdown.
     * Designed to protect InP/GaN substrates during thermal/voltage excursions.
     */
    PorthRegister<uint32_t> safety_trip;
};

// --- PHYSICAL MEMORY AUDIT ---
// These assertions act as the "Sovereign Guard." They prevent the C++ compiler
// from optimizing, reordering, or padding the struct in a way that breaks
// the binary contract with the Newport hardware.

static_assert(std::is_standard_layout_v<PorthDeviceLayout>,
              "PorthDeviceLayout must be Standard Layout to guarantee C-style binary compatibility "
              "with MMIO.");

static_assert(
    sizeof(PorthDeviceLayout) == expected_layout_size,
    "PorthDeviceLayout size mismatch. Check for unexpected compiler padding between registers.");

static_assert(offsetof(PorthDeviceLayout, control) == offset_control,
              "Physical Mismatch: 'control' register moved from 0x00.");
static_assert(offsetof(PorthDeviceLayout, status) == offset_status,
              "Physical Mismatch: 'status' register moved from 0x40.");
static_assert(offsetof(PorthDeviceLayout, data_ptr) == offset_data_ptr,
              "Physical Mismatch: 'data_ptr' register moved from 0x80.");
static_assert(offsetof(PorthDeviceLayout, counter) == offset_counter,
              "Physical Mismatch: 'counter' register moved from 0xC0.");
static_assert(offsetof(PorthDeviceLayout, laser_temp) == offset_laser_temp,
              "Physical Mismatch: 'laser_temp' register moved from 0x100.");
static_assert(offsetof(PorthDeviceLayout, gan_voltage) == offset_gan_voltage,
              "Physical Mismatch: 'gan_voltage' register moved from 0x140.");
static_assert(offsetof(PorthDeviceLayout, rf_snr) == offset_rf_snr,
              "Physical Mismatch: 'rf_snr' register moved from 0x180.");
static_assert(offsetof(PorthDeviceLayout, safety_trip) == offset_safety_trip,
              "Physical Mismatch: 'safety_trip' register moved from 0x1C0.");

} // namespace porth