/*
 * CHZM SiliconFuse - unified pool protocol (shared user/kernel ABI)
 *
 * Status: PROVISIONAL (ABI version 1). No kernel component ships yet.
 * This header is the single source of truth for the layout that
 * src/kernel/ and user-mode clients will exchange. Every structure is
 * fixed-width and naturally aligned; no pointers cross the boundary.
 *
 * The protocol describes *leases*, not raw physical memory. A lease is a
 * revocable grant of a byte range inside one memory tier. The policy engine
 * owns placement, the kernel driver owns translation, clients own nothing
 * beyond the lease lifetime.
 */

#ifndef CHZM_POOL_PROTOCOL_H
#define CHZM_POOL_PROTOCOL_H

#include <stdint.h>

#define CHZM_POOL_ABI_VERSION   1u
#define CHZM_POOL_MAX_TIERS     3u
#define CHZM_POOL_MAX_LEASES    4096u

#if defined(_WIN32)
#include <winioctl.h>
#define CHZM_POOL_DEVICE_NAME   L"\\\\.\\ChzmSiliconFuse"
#define CHZM_POOL_DEVICE_PATH   L"\\Device\\ChzmSiliconFuse"
#define CHZM_POOL_SYMLINK_NAME  L"\\DosDevices\\ChzmSiliconFuse"
#endif

/* Tier identifiers, ordered by ascending access cost. */
typedef enum _CHZM_TIER {
    ChzmTierSystemRam   = 0,   /* Tier 0: DDR4, cache-coherent, ~60-70 ns   */
    ChzmTierDeviceVram  = 1,   /* Tier 1: GDDR6 over PCIe BAR1, ~1 us       */
    ChzmTierNvmeSpill   = 2    /* Tier 2: NVMe, ~50-100 us                  */
} CHZM_TIER;

/* Lease lifecycle. EVICTING and FAULTED are transient states. */
typedef enum _CHZM_LEASE_STATE {
    ChzmLeaseFree      = 0,
    ChzmLeaseReserved  = 1,
    ChzmLeaseActive    = 2,
    ChzmLeaseEvicting  = 3,
    ChzmLeaseFaulted   = 4,
    ChzmLeaseRevoked   = 5
} CHZM_LEASE_STATE;

/* Access intent. WRITE_COMBINED maps to WC memory on the CPU side. */
typedef enum _CHZM_ACCESS {
    ChzmAccessReadOnly     = 0,
    ChzmAccessWriteBack    = 1,
    ChzmAccessWriteCombined = 2
} CHZM_ACCESS;

#pragma pack(push, 8)

/* Physical topology as discovered by CHZM_HardwareProbe. */
typedef struct _CHZM_POOL_CAPS {
    uint32_t abiVersion;
    uint32_t tierCount;
    uint64_t totalBytes;          /* sum of usable tier capacities          */
    uint64_t ramBytes;            /* Tier 0 usable capacity                 */
    uint64_t vramBytes;           /* Tier 1 framebuffer capacity            */
    uint64_t bar1Base;            /* physical BAR1 aperture base            */
    uint64_t bar1Size;            /* aperture size (power-of-two, >= VRAM)  */
    uint64_t nvmeBytes;           /* Tier 2 reservation, 0 if absent        */
    uint32_t pageSize;            /* placement granule (4096 or 65536)      */
    uint32_t flags;               /* CHZM_CAPS_* bitmask                    */
} CHZM_POOL_CAPS;

#define CHZM_CAPS_REBAR_ACTIVE       0x00000001u
#define CHZM_CAPS_ABOVE_4G_APERTURE  0x00000002u
#define CHZM_CAPS_WDDM_BROKER        0x00000004u
#define CHZM_CAPS_IOMMU_PRESENT      0x00000008u
#define CHZM_CAPS_EXCLUSIVE_MODE     0x00000010u

/* A revocable byte-range grant. */
typedef struct _CHZM_LEASE {
    uint64_t leaseId;
    uint64_t offset;              /* byte offset inside the tier             */
    uint64_t length;              /* multiple of pageSize                    */
    uint64_t physicalBase;        /* resolved physical address, 0 until map  */
    uint32_t tier;                /* CHZM_TIER                               */
    uint32_t state;               /* CHZM_LEASE_STATE                        */
    uint32_t access;              /* CHZM_ACCESS                             */
    uint32_t priority;            /* 0 = lowest, 255 = pinned                */
    uint32_t ttlMs;               /* 0 = no expiry                           */
    uint32_t reserved;
} CHZM_LEASE;

/* Per-tier statistics, sampled on demand. */
typedef struct _CHZM_TIER_STATS {
    uint32_t tier;
    uint32_t leaseCount;
    uint64_t capacityBytes;
    uint64_t usedBytes;
    uint64_t faultCount;
    uint64_t evictionCount;
    uint64_t bytesMigrated;
} CHZM_TIER_STATS;

#pragma pack(pop)

#if defined(_WIN32)
/*
 * IOCTL surface (planned).
 *
 * METHOD_BUFFERED for control paths; METHOD_NEITHER is deliberately avoided
 * so the driver never dereferences user pointers directly. Bulk data movement
 * will use MDLs, not IOCTL buffers.
 */
#define CHZM_IOCTL_QUERY_CAPS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define CHZM_IOCTL_LEASE_ACQUIRE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define CHZM_IOCTL_LEASE_RELEASE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define CHZM_IOCTL_LEASE_QUERY \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)
#define CHZM_IOCTL_TIER_STATS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS)
#define CHZM_IOCTL_MAP_APERTURE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif

#endif /* CHZM_POOL_PROTOCOL_H */
