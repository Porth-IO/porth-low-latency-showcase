/**
 * @file PorthXDPPortal.hpp
 * @brief Zero-copy network bridge using AF_XDP for the Sovereign Logic Layer.
 *
 * Porth-IO: The Sovereign Logic Layer
 *
 * Copyright (c) 2026 Porth-IO Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "PorthShuttle.hpp"
#include "PorthTelemetry.hpp"
#include <bit>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <format>
#include <iostream>
#include <linux/if_link.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <xdp/xsk.h>

namespace porth {

class PorthXDPPortal {
private:
    std::string m_ifname;
    uint32_t m_queue_id;

    // AF_XDP UMEM (User Memory) Structures
    struct xsk_umem* m_umem{nullptr};
    struct xsk_ring_prod m_fill_ring{};
    struct xsk_ring_cons m_comp_ring{};

    // AF_XDP Socket Structures
    struct xsk_socket* m_xsk{nullptr};
    struct xsk_ring_cons m_rx_ring{};
    struct xsk_ring_prod m_tx_ring{};

    int m_xsk_fd = -1;
    void* m_umem_buffer{nullptr}; ///< Pointer to the underlying Shuttle memory

    uint64_t m_umem_start{0}; ///< Start address of the Sovereign HugePage
    uint64_t m_umem_end{0};   ///< End address of the Sovereign HugePage

    /** @brief Recycles a frame back to the NIC for future packet ingestion. */
    void recycle_frame(uint64_t addr) noexcept {
        uint32_t idx_fill = 0;
        if (xsk_ring_prod__reserve(&m_fill_ring, 1, &idx_fill) == 1) {
            *xsk_ring_prod__fill_addr(&m_fill_ring, idx_fill) = addr;
            xsk_ring_prod__submit(&m_fill_ring, 1);
        }
    }

    /** * @brief Internal validation helper to lower cognitive complexity.
     * Functions defined inside the class body are implicitly inline.
     */
    [[nodiscard]] __attribute__((always_inline)) auto is_addr_valid(uint64_t addr,
                                                                    uint32_t len) const noexcept
        -> bool {
        return (addr >= m_umem_start) && ((addr + len) <= m_umem_end);
    }

public:
    explicit PorthXDPPortal(std::string ifname, uint32_t queue_id = 0)
        : m_ifname(std::move(ifname)), m_queue_id(queue_id) {
        std::cout << std::format("[Porth-XDP] Initializing Sovereign Portal on {} (Queue {})...\n",
                                 m_ifname,
                                 m_queue_id)
                  << std::flush;
    }

    /** @brief Destructor: Releases AF_XDP resources. */
    ~PorthXDPPortal() {
        std::cout << std::format("[Porth-XDP] Closing portal on {}. Releasing XDP hooks.\n",
                                 m_ifname)
                  << std::flush;

        if (m_xsk != nullptr) {
            xsk_socket__delete(m_xsk);
        }
        if (m_umem != nullptr) {
            xsk_umem__delete(m_umem);
        }
    }

    /**
     * @brief Binds the XDP socket directly to the PorthShuttle's HugePage memory.
     * @tparam Cap The capacity of the Shuttle ring.
     * @param shuttle The active DMA shuttle.
     */
    template <size_t Cap>
    void bind_shuttle_memory(PorthShuttle<Cap>& shuttle) {
        m_umem_buffer          = shuttle.get_raw_memory_ptr();
        const size_t umem_size = shuttle.get_raw_memory_size();

        m_umem_start = std::bit_cast<uint64_t>(m_umem_buffer);
        m_umem_end   = m_umem_start + shuttle.get_raw_memory_size();

        struct xsk_umem_config umem_cfg = {.fill_size      = XSK_RING_PROD__DEFAULT_NUM_DESCS,
                                           .comp_size      = XSK_RING_CONS__DEFAULT_NUM_DESCS,
                                           .frame_size     = XSK_UMEM__DEFAULT_FRAME_SIZE,
                                           .frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM,
                                           .flags          = 0};

        int ret = xsk_umem__create(
            &m_umem, m_umem_buffer, umem_size, &m_fill_ring, &m_comp_ring, &umem_cfg);

        if (ret != 0) {
            throw std::runtime_error("Failed to map PorthShuttle as XDP UMEM.");
        }

        std::cout << "[Porth-XDP] Shuttle memory successfully mapped as Zero-Copy UMEM.\n"
                  << std::flush;
    }

    /**
     * @brief Hijacks the NIC queue and binds the AF_XDP socket to our UMEM.
     */
    void open_portal() {
        if (m_umem == nullptr) {
            throw std::runtime_error(
                "Porth-XDP: UMEM not initialized. Call bind_shuttle_memory first.");
        }

        struct xsk_socket_config xsk_cfg = {.rx_size      = XSK_RING_CONS__DEFAULT_NUM_DESCS,
                                            .tx_size      = XSK_RING_PROD__DEFAULT_NUM_DESCS,
                                            .libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,
                                            .xdp_flags    = XDP_FLAGS_UPDATE_IF_NOEXIST,
                                            .bind_flags   = XDP_USE_NEED_WAKEUP};

        int ret = xsk_socket__create(
            &m_xsk, m_ifname.c_str(), m_queue_id, m_umem, &m_rx_ring, &m_tx_ring, &xsk_cfg);

        m_xsk_fd = xsk_socket__fd(m_xsk);

        if (m_xsk_fd < 0) {
            throw std::runtime_error("Porth-XDP: Failed to retrieve XDP socket file descriptor.");
        }

        if (ret != 0) {
            throw std::runtime_error(std::format(
                "Porth-XDP: Failed to create AF_XDP socket on {} (Queue {}). Error code: {}",
                m_ifname,
                m_queue_id,
                ret));
        }

        std::cout << std::format("[Porth-XDP] Portal Opened. Socket bound to {} (Queue {}).\n",
                                 m_ifname,
                                 m_queue_id)
                  << std::flush;
    }

    /**
     * @brief bridge_to_shuttle: Polls the NIC and pushes work to the hardware.
     */
    template <size_t Cap>
    void bridge_to_shuttle(PorthShuttle<Cap>& shuttle, PorthStats* stats = nullptr) noexcept {
        uint32_t idx_rx     = 0;
        const uint32_t rcvd = xsk_ring_cons__peek(&m_rx_ring, 1, &idx_rx);

        if (rcvd == 0) {
            if (xsk_ring_prod__needs_wakeup(&m_fill_ring)) {
                (void)recvfrom(m_xsk_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
            }
            return;
        }

        const struct xdp_desc* desc = xsk_ring_cons__rx_desc(&m_rx_ring, idx_rx);
        const uint64_t packet_addr  = xsk_umem__add_offset_to_addr(desc->addr);
        const uint32_t packet_len   = desc->len;

        if (!is_addr_valid(packet_addr, packet_len)) {
            if (stats != nullptr) {
                stats->dropped_packets.fetch_add(1, std::memory_order_relaxed);
            }
            recycle_frame(desc->addr);
            xsk_ring_cons__release(&m_rx_ring, 1);
            return;
        }

        if (shuttle.ring()->push({.addr = packet_addr, .len = packet_len})) {
            xsk_ring_cons__release(&m_rx_ring, 1);
            if (stats != nullptr) {
                stats->total_packets.fetch_add(1, std::memory_order_relaxed);
                stats->total_bytes.fetch_add(packet_len, std::memory_order_relaxed);
            }
            recycle_frame(desc->addr);
        } else {
            if (stats != nullptr) {
                stats->dropped_packets.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Prohibit copying and moving to maintain address stability for AF_XDP
    PorthXDPPortal(const PorthXDPPortal&)                    = delete;
    auto operator=(const PorthXDPPortal&) -> PorthXDPPortal& = delete;
    PorthXDPPortal(PorthXDPPortal&&)                         = delete;
    auto operator=(PorthXDPPortal&&) -> PorthXDPPortal&      = delete;
};

} // namespace porth