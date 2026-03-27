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

// Named constant to avoid magic number errors
static const u16 PORTH_SOVEREIGN_PORT = 12345;

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, int);
    __type(value, int);
} porth_xsk_map SEC(".maps");

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

    // 1. SURGICAL FILTER: Only redirect IPv4.
    if (eth->h_proto != __constant_htons(ETH_P_IP)) {
        return XDP_PASS;
    }

    // Explicit cast to unsigned char* to avoid void* arithmetic GNU extension
    struct iphdr* iph = (void*)((unsigned char*)data + sizeof(*eth));
    if ((void*)(iph + 1) > data_end) {
        return XDP_PASS;
    }

    // 2. Only look at UDP
    if (iph->protocol != IPPROTO_UDP) {
        return XDP_PASS;
    }

    struct udphdr* udp = (void*)((unsigned char*)iph + ((unsigned long)iph->ihl * 4));
    if ((void*)(udp + 1) > data_end) {
        return XDP_PASS;
    }

    // 3. ONLY redirect our Sovereign Port
    u16 dport = __constant_ntohs(udp->dest);
    if (dport == PORTH_SOVEREIGN_PORT) {
        // Explicitly cast the flags to __u64 to resolve sign conversion warnings
        return (int)bpf_redirect_map(
            &porth_xsk_map, ctx->rx_queue_index, (unsigned long long)XDP_PASS);
    }

    return XDP_PASS;
}

char license[] SEC("_license") = "GPL";
