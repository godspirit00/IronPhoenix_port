/* QEMU networking API subset for the standalone nvnet port: a NIC with no backend. */
#pragma once
#include "nv2a_compat.h"
#include <sys/types.h>

struct iovec {
    void *iov_base;
    size_t iov_len;
};
static inline size_t iov_size(const struct iovec *iov, unsigned cnt)
{
    size_t n = 0;
    for (unsigned i = 0; i < cnt; i++) n += iov[i].iov_len;
    return n;
}
static inline size_t iov_to_buf(const struct iovec *iov, unsigned cnt, size_t offset, void *buf, size_t bytes)
{
    size_t done = 0;
    for (unsigned i = 0; i < cnt && done < bytes; i++) {
        size_t n = iov[i].iov_len < bytes - done ? iov[i].iov_len : bytes - done;
        memcpy((uint8_t *)buf + done, iov[i].iov_base, n);
        done += n;
    }
    return done;
}

typedef struct NetClientState NetClientState;
typedef struct NetClientInfo {
    int type;
    size_t size;
    bool (*can_receive)(NetClientState *);
    ssize_t (*receive)(NetClientState *, const uint8_t *, size_t);
    ssize_t (*receive_iov)(NetClientState *, const struct iovec *, int);
    void (*link_status_changed)(NetClientState *);
} NetClientInfo;
struct NetClientState {
    bool link_down;
    void *opaque;
    NetClientInfo *info;
};
typedef struct NICState {
    NetClientState nc;
} NICState;
typedef struct MACAddr {
    uint8_t a[6];
} MACAddr;
typedef struct NICConf {
    MACAddr macaddr;
} NICConf;
#define NET_CLIENT_DRIVER_NIC 1

static inline NetClientState *qemu_get_queue(NICState *nic) { return &nic->nc; }
static inline void *qemu_get_nic_opaque(NetClientState *nc) { return nc->opaque; }
static inline ssize_t qemu_send_packet(NetClientState *nc, const uint8_t *buf, int size) { return size; /* dropped */ }

typedef uint64_t dma_addr_t;
#define NVNET(obj) ((NvNetState *)(obj))
static inline uint32_t le32_to_cpu(uint32_t v) { return v; }
static inline uint16_t le16_to_cpu(uint16_t v) { return v; }
static inline uint32_t cpu_to_le32(uint32_t v) { return v; }
static inline uint16_t cpu_to_le16(uint16_t v) { return v; }
static inline bool is_broadcast_ether_addr(const uint8_t *a)
{
    return (a[0] & a[1] & a[2] & a[3] & a[4] & a[5]) == 0xff;
}
static inline int nvnet_dma_read(dma_addr_t addr, void *buf, size_t len)
{
    memcpy(buf, XBOX_PHYS(addr), len);
    return 0;
}
static inline int nvnet_dma_write(dma_addr_t addr, const void *buf, size_t len)
{
    memcpy(XBOX_PHYS(addr), buf, len);
    return 0;
}
void nvnet_host_set_irq(bool level);

#define MAC_FMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC_ARG(x) ((uint8_t *)(x))[0], ((uint8_t *)(x))[1], ((uint8_t *)(x))[2], \
                   ((uint8_t *)(x))[3], ((uint8_t *)(x))[4], ((uint8_t *)(x))[5]

#define trace_nvnet_update_irq(...) ((void)0)
#define trace_nvnet_packet_tx(...) ((void)0)
#define trace_nvnet_desc_store(...) ((void)0)
#define trace_nvnet_cant_rx(...) ((void)0)
#define trace_nvnet_rx_dma(...) ((void)0)
#define trace_nvnet_cant_tx(...) ((void)0)
#define trace_nvnet_tx_dma(...) ((void)0)
#define trace_nvnet_rx_filter_bcast_match(...) ((void)0)
#define trace_nvnet_rx_filter_mcast_match(...) ((void)0)
#define trace_nvnet_rx_filter_mcast_mismatch(...) ((void)0)
#define trace_nvnet_rx_filter_ucast_match(...) ((void)0)
#define trace_nvnet_rx_filter_ucast_mismatch(...) ((void)0)
#define trace_nvnet_rx_oversized(...) ((void)0)
#define trace_nvnet_rx_filter_dropped(...) ((void)0)
#define trace_nvnet_link_negotiation_start(...) ((void)0)
#define trace_nvnet_link_negotiation_done(...) ((void)0)
#define trace_nvnet_link_status_changed(...) ((void)0)
#define trace_nvnet_phy_reg_read(...) ((void)0)
#define trace_nvnet_phy_reg_write(...) ((void)0)
#define trace_nvnet_reg_read(...) ((void)0)
#define trace_nvnet_reg_write(...) ((void)0)
#define trace_nvnet_io_read(...) ((void)0)
#define trace_nvnet_io_write(...) ((void)0)
#define trace_nvnet_mdio_read(...) ((void)0)
#define trace_nvnet_mdio_write(...) ((void)0)
#define trace_nvnet_dump_ring_descriptors(...) ((void)0)
#define trace_nvnet_dump_ring_descriptor(...) ((void)0)
