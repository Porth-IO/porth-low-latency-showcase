/**
 * @file porth_xdp_kern.c
 * @brief eBPF kernel program for high-speed packet redirection via AF_XDP.
 *
 * Porth-IO: Low Latency Showcase
 */

#include <linux/types.h>

/* * Sovereign Header Guard:
 * This comment breaks the #include block to prevent clang-format from
 * re-sorting linux/types.h alphabetically. It must remain at the top
 * to define __u64 and __u32 before the BPF helpers are processed.
 */
#include <bpf/bpf_helpers.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>

// Using standard BPF types to satisfy tidy without breaking verifier
typedef __u16 u16;
typedef __u32 u32;

/** @brief Target UDP port for low-latency telemetry redirection. */
static const u16 PORTH_SOVEREIGN_PORT = 12345;

/** * @brief BPF map for AF_XDP socket redirection.
 *
 * This map stores file descriptors for AF_XDP sockets. When a packet matches 
 * the filtering criteria, it is redirected to the socket index stored here, 
 * bypassing the standard Linux networking stack.
 */
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, int);
    __type(value, int);
} porth_xsk_map SEC(".maps");

/**
 * @brief Main XDP program entry point.
 *
 * Performs surgical packet inspection at the earliest possible stage in the 
 * Linux kernel (the NIC driver). Packets matching the specific UDP port are 
 * moved directly to userspace UMEM, eliminating the overhead of kernel-to-user 
 * copies and context switches.
 *
 * @param ctx The XDP metadata context containing packet boundaries.
 * @return int XDP_PASS to continue to stack, or result of bpf_redirect_map.
 */
SEC("xdp")
int porth_xdp_prog(struct xdp_md* ctx) {
    // BPF Context access requires explicit integer-to-pointer conversion for the verifier.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    void* data_end = (void*)(unsigned long)ctx->data_end;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    void* data = (void*)(unsigned long)ctx->data;

    struct ethhdr* eth = data;
    if ((void*)(eth + 1) > data_end) {
        return XDP_PASS;
    }

    // 1. SURGICAL FILTER: Only redirect IPv4 traffic.
    if (eth->h_proto != __constant_htons(ETH_P_IP)) {
        return XDP_PASS;
    }

    // Explicit cast to unsigned char* to avoid void* arithmetic GNU extension
    struct iphdr* iph = (void*)((unsigned char*)data + sizeof(*eth));
    if ((void*)(iph + 1) > data_end) {
        return XDP_PASS;
    }

    // 2. Protocol Filter: Narrow focus to UDP segments.
    if (iph->protocol != IPPROTO_UDP) {
        return XDP_PASS;
    }

    struct udphdr* udp = (void*)((unsigned char*)iph + ((unsigned long)iph->ihl * 4));
    if ((void*)(udp + 1) > data_end) {
        return XDP_PASS;
    }

    // 3. Port Hijacking: Redirect packets targeting the calibrated port.
    u16 dport = __constant_ntohs(udp->dest);
    if (dport == PORTH_SOVEREIGN_PORT) {
        // Explicitly cast the flags to __u64 to resolve sign conversion warnings.
        // The redirection uses the RX queue index to ensure NUMA locality.
        return (int)bpf_redirect_map(
            &porth_xsk_map, ctx->rx_queue_index, (unsigned long long)XDP_PASS);
    }

    return XDP_PASS;
}

/** @brief BPF program license required for loading into the Linux kernel. */
char license[] SEC("_license") = "GPL";