/**
 * @file PorthNetworkPortal.hpp
 * @brief Production Build: Pure Sovereign Signal Interconnect.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <net/if.h>
#include <sys/mman.h>
#include <xdp/xsk.h>

#ifndef XDP_FLAGS_SKB_MODE
#define XDP_FLAGS_SKB_MODE (1U << 1)
#endif

namespace porth {

class PorthNetworkPortal {
private:
    std::string m_ifname;
    struct xsk_socket* m_xsk{nullptr};
    struct xsk_umem* m_umem{nullptr};
    void* m_umem_area{nullptr};
    struct bpf_object* m_bpf_obj{nullptr};
    struct xsk_ring_prod m_fill_ring{};
    struct xsk_ring_cons m_comp_ring{};
    struct xsk_ring_prod m_tx_ring{};
    struct xsk_ring_cons m_rx_ring{};

    static constexpr uint32_t NUM_FRAMES = 4096;
    static constexpr uint32_t FRAME_SIZE = 4096;
    static constexpr size_t UMEM_SIZE    = static_cast<size_t>(NUM_FRAMES) * FRAME_SIZE;

    // Constants for packet parsing
    static constexpr uint32_t FILL_RING_RESERVE_SIZE = 64;
    static constexpr size_t ETH_P_OFF                = 12;
    static constexpr uint8_t ETH_P_IP_H              = 0x08;
    static constexpr uint8_t ETH_P_IP_L              = 0x00;
    static constexpr size_t ETH_HLEN                 = 14;
    static constexpr size_t IP_HLEN                  = 20;
    static constexpr size_t UDP_HLEN                 = 8;
    static constexpr size_t TOTAL_HDR_LEN            = ETH_HLEN + IP_HLEN + UDP_HLEN; // 42 bytes

public:
    explicit PorthNetworkPortal(std::string ifname) : m_ifname(std::move(ifname)) {
        if (posix_memalign(&m_umem_area, static_cast<size_t>(getpagesize()), UMEM_SIZE) != 0) {
            throw std::runtime_error("UMEM allocation failed");
        }
    }

    void initialize_umem() {
        struct xsk_umem_config cfg = {.fill_size      = XSK_RING_PROD__DEFAULT_NUM_DESCS,
                                      .comp_size      = XSK_RING_CONS__DEFAULT_NUM_DESCS,
                                      .frame_size     = FRAME_SIZE,
                                      .frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM,
                                      .flags          = 0};
        int ret =
            xsk_umem__create(&m_umem, m_umem_area, UMEM_SIZE, &m_fill_ring, &m_comp_ring, &cfg);
        if (ret != 0) {
            throw std::runtime_error("UMEM registration failed");
        }
    }

    void create_socket() {
        struct xsk_socket_config cfg = {.rx_size      = XSK_RING_CONS__DEFAULT_NUM_DESCS,
                                        .tx_size      = XSK_RING_PROD__DEFAULT_NUM_DESCS,
                                        .libbpf_flags = 0,
                                        .xdp_flags    = XDP_FLAGS_SKB_MODE,
                                        .bind_flags   = XDP_USE_NEED_WAKEUP};
        int ret =
            xsk_socket__create(&m_xsk, m_ifname.c_str(), 0, m_umem, &m_rx_ring, &m_tx_ring, &cfg);
        if (ret != 0) {
            throw std::runtime_error("Socket creation failed");
        }
    }

    void bind_xsk_map() {
        if (m_bpf_obj == nullptr || m_xsk == nullptr) {
            return;
        }
        int map_fd = bpf_object__find_map_fd_by_name(m_bpf_obj, "porth_xsk_map");
        if (map_fd < 0) {
            return;
        }
        int xsk_fd   = xsk_socket__fd(m_xsk);
        uint32_t key = 0;
        bpf_map_update_elem(map_fd, &key, &xsk_fd, BPF_ANY);
    }

    void prime_fill_ring() {
        uint32_t idx = 0;
        uint32_t n   = xsk_ring_prod__reserve(&m_fill_ring, FILL_RING_RESERVE_SIZE, &idx);
        for (uint32_t i = 0; i < n; i++) {
            *xsk_ring_prod__fill_addr(&m_fill_ring, idx++) = static_cast<uint64_t>(i) * FRAME_SIZE;
        }
        xsk_ring_prod__submit(&m_fill_ring, n);
    }

    auto poll_rx() -> uint32_t {
        uint32_t idx  = 0;
        uint32_t rcvd = xsk_ring_cons__peek(&m_rx_ring, 1, &idx);
        if (rcvd == 0) {
            return 0;
        }

        for (uint32_t i = 0; i < rcvd; i++) {
            const struct xdp_desc* desc = xsk_ring_cons__rx_desc(&m_rx_ring, idx++);
            uint8_t* pkt_start          = static_cast<uint8_t*>(m_umem_area) + desc->addr;

            // Check for IPv4 EtherType (0x0800)
            if (pkt_start[ETH_P_OFF] == ETH_P_IP_H && pkt_start[ETH_P_OFF + 1] == ETH_P_IP_L) {
                // Surgical Extraction: Skip Eth(14) + IP(20) + UDP(8) = 42 bytes
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                std::string payload(reinterpret_cast<char*>(pkt_start + TOTAL_HDR_LEN),
                                    desc->len - TOTAL_HDR_LEN);
                std::cout << std::format("[Sovereign-Node] Signal: \"{}\"\n", payload);
            }

            uint32_t f_idx = 0;
            if (xsk_ring_prod__reserve(&m_fill_ring, 1, &f_idx) == 1) {
                *xsk_ring_prod__fill_addr(&m_fill_ring, f_idx) = desc->addr;
                xsk_ring_prod__submit(&m_fill_ring, 1);
            }
        }
        xsk_ring_cons__release(&m_rx_ring, rcvd);
        return rcvd;
    }

    void load_kernel_program(const std::string& path) {
        m_bpf_obj = bpf_object__open_file(path.c_str(), nullptr);
        if (m_bpf_obj != nullptr) {
            bpf_object__load(m_bpf_obj);
        }
    }

    ~PorthNetworkPortal() {
        if (m_xsk != nullptr) {
            xsk_socket__delete(m_xsk);
        }
        if (m_umem != nullptr) {
            xsk_umem__delete(m_umem);
        }
        if (m_bpf_obj != nullptr) {
            bpf_object__close(m_bpf_obj);
        }
        if (m_umem_area != nullptr) {
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
            free(m_umem_area);
        }
    }

    PorthNetworkPortal(const PorthNetworkPortal&)                        = delete;
    auto operator=(const PorthNetworkPortal&) -> PorthNetworkPortal&     = delete;
    PorthNetworkPortal(PorthNetworkPortal&&) noexcept                    = default;
    auto operator=(PorthNetworkPortal&&) noexcept -> PorthNetworkPortal& = default;

    [[nodiscard]] auto is_active() const noexcept -> bool { return m_xsk != nullptr; }
};

} // namespace porth