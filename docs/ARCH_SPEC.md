# CHZM SiliconFuse - Architecture Specification

Document: ARCH_SPEC
Version: 0.1 (Phase 0)
Status: Design baseline, validated only at the probe layer
Target: Windows 11 x64, WDDM 3.x, UEFI platform with Resizable BAR

---

## 1. Scope

This document specifies how SiliconFuse presents system DRAM, discrete GPU
memory exposed through PCIe BAR1, and NVMe storage as one policy-managed
memory hierarchy.

In scope:

- physical address discovery and validation;
- tier semantics, placement and migration policy;
- a WDDM coexistence contract;
- the kernel/user ABI for leases;
- safety, failure and recovery rules.

Out of scope for v0.1:

- GPU compute scheduling;
- peer-to-peer DMA between devices;
- firmware or VBIOS modification;
- support for non-WDDM display stacks;
- any claim of linear address space larger than the platform's physical
  address width.

Non-goal: replacing WDDM. SiliconFuse is a companion, not a display driver.
A second display driver on the same adapter is unsupported by Windows and is
not part of this design.

---

## 2. Reference platform (measured)

Values below were read from the reference machine by `CHZM_HardwareProbe`
v0.1. The full capture is in `docs/HARDWARE_MAP.txt`.

```
CPU                 AMD Ryzen 7 5700X (8C/16T)
RAM                 16384 MB DDR4-3800, 2 x 8 GB, dual channel
GPU                 NVIDIA RTX 3060 12 GB, GA106, VEN_10DE DEV_2504
PCI location        09:00.0
Driver              32.0.16.1742
Framebuffer         12288 MB
BAR0                16 MB @ 0x00000000_F5000000   (registers)
BAR1                16384 MB @ 0x00000078_00000000 (prefetchable, large)
Auxiliary BAR       32 MB @ 0x0000007C_00000000
Unified envelope    28672 MB
```

### 2.1 Why the aperture is 16 GB

The PCI specification requires a BAR to be a power-of-two size and naturally
aligned. A 12 GB framebuffer cannot be mapped by a 12 GB BAR. Firmware and
Windows therefore assign the next power of two, 16 GB (`0x400000000`), and the
GPU decodes only the first 12 GB. Addresses in `[base + 12 GB, base + 16 GB)`
are not backed by DRAM.

Consequences for software:

- size validation must use `aperture >= framebuffer`, not equality;
- the aperture cannot be used to infer framebuffer size;
- a bounds check on `aperture_size` alone is insufficient; the valid window is
  `min(aperture, framebuffer)`.

### 2.2 Resource descriptors

Windows reports the 16 GB window as `CmResourceTypeMemoryLarge` with the
`CM_RESOURCE_MEMORY_LARGE_40` flag. The descriptor's `Length` field stores
`length >> 8`; the probe reconstructs the real size as `Length << 8`
(0x04000000 << 8 = 0x400000000 = 16 GB). Tools that only handle
`CmResourceTypeMemory` will silently miss the aperture. This was the first
defect found during bring-up.

BAR0 (16 MB) and the auxiliary 32 MB BAR are ordinary 32-bit descriptors.
The 16 MB block is the register aperture; it must never be treated as memory.

---

## 3. Address space model

Three address spaces are involved and must not be conflated.

| Space | Owner | Purpose |
|---|---|---|
| System physical address (SPA) | Firmware + Windows | RAM frames, MMIO, BAR apertures |
| GPU physical address (GPA) | VidMm / KMD | framebuffer pages, page tables |
| Process virtual address (VA) | SiliconFuse client | mapped lease windows |

Tier 0 and Tier 1 are both reachable from the CPU through SPA. Tier 0 is
ordinary RAM. Tier 1 is MMIO: reads and writes go over PCIe and are strongly
ordered relative to normal cacheable memory unless mapped write-combined.

Tier 2 is not addressable as memory. It is a block device reached through the
storage stack; migration to and from Tier 2 is asynchronous I/O.

### 3.1 Mapping rules

1. A lease is always a contiguous range inside one tier.
2. Tier 1 leases must be aligned to the placement granule and must lie fully
   inside `min(aperture, framebuffer)`.
3. The CPU mapping of a Tier 1 lease uses `MmMapLockedPagesSpecifyCache` with
   `MmWriteCombined` for write-mostly buffers. Read-heavy use of WC memory is
   permitted but must be documented as slow: uncached reads do not combine.
4. No mapping may alias a page owned by another lease. The driver keeps a
   range tree keyed by physical address; overlap is a bug, not a policy.
5. User mappings are created only for leased ranges, never for raw PFNs.

A user-mode client can map its *own* D3D12 L1 allocation through the runtime
(measured, see 5.3) without a kernel driver. That path is limited to memory the
client already owns; it does not provide access to another client's pages.

### 3.2 DMA

Data movement between tiers uses DMA, not CPU copies, wherever the platform
allows it:

- RAM -> VRAM: the GPU's copy engine (when the WDDM KMD exposes a shared
  allocation) or CPU write-combined stores as fallback;
- VRAM -> RAM: CPU reads are expensive; prefer a copy engine or batched
  128-byte reads with prefetch;
- RAM <-> NVMe: standard block I/O with MDLs.

Peer-to-peer between the GPU and NVMe is out of scope until the IOMMU and
root-complex topology are validated on the target platform.

---

## 4. Tier semantics and QoS

| Property | Tier 0 (DDR4) | Tier 1 (GDDR6/ReBAR) | Tier 2 (NVMe) |
|---|---|---|---|
| Capacity (reference) | 16 GB | 12 GB | policy |
| Read bandwidth | 60.8 GB/s theoretical | 15.75 GB/s line rate | ~3.5 GB/s |
| Write bandwidth | 60.8 GB/s | 15.75 GB/s (WC stores) | ~3.0 GB/s |
| Load-to-use latency | 60-70 ns | 0.6-1.5 us | 50-100 us |
| Granularity | 4 KB pages | 4 KB / 64 KB | 4 KB logical blocks |
| Coherence | hardware | none | none |
| Volatility | volatile | volatile | persistent |

The latency jump from Tier 0 to Tier 1 is roughly one order of magnitude, and
from Tier 1 to Tier 2 another two orders. Placement policy must therefore be
latency-driven, not capacity-driven: a buffer that is touched every frame must
not be placed in Tier 1 merely because Tier 0 is full.

### 4.1 Placement policy (target)

1. Pin working set in Tier 0 while free frames exist above a low-water mark.
2. Place write-mostly, streaming or device-consumed buffers in Tier 1 when a
   lease is available.
3. Demote cold pages to Tier 2 only under memory pressure, in LRU order, with
   writeback batching.
4. Never migrate a page that is mapped and dirty without quiescing the lease.

### 4.2 Fault path

A Tier 1 lease that is revoked while mapped raises a driver-owned fault:

1. The driver marks the lease `EVICTING` and blocks new mappings.
2. Dirty ranges are flushed to Tier 0 (or Tier 2 if Tier 0 is exhausted).
3. Outstanding user mappings are invalidated; subsequent accesses take a
   page fault handled by the driver.
4. The lease is released to the broker, state `REVOKED`.
5. The client is notified through the completion port associated with the
   lease handle.

A client that ignores revocation is terminated by handle closure. The driver
never waits indefinitely for a client to drain.

---

## 5. WDDM coexistence

### 5.1 The conflict

VidMm owns the framebuffer. It allocates, evicts and remaps pages through GPU
page tables without informing any third party. BAR1 is a CPU-visible window
over the same physical DRAM. Writing to a page that VidMm has assigned to a
live allocation corrupts that allocation; reading it leaks another process's
data.

There is no public WDDM interface that partitions BAR1. The aperture is
exposed to the CPU as one resource for the adapter; the OS does not arbitrate
sub-ranges between clients.

Measured state on the reference machine: 16355 MiB of 16384 MiB BAR1 in use
under an idle desktop. The premise that 12 GB of VRAM is "free capacity" is
false on a live system.

### 5.2 Arbitration contract

SiliconFuse requires a broker that understands WDDM budget pressure. Three
deployment modes are defined, in order of increasing capability and risk.

**Mode A - Exclusive compute (safe today).**
The adapter is not bound to the display stack (no monitor attached, driver
disabled or the device is in a compute-only configuration). SiliconFuse owns
the aperture. No coexistence problem exists. Throughput is bounded by PCIe.

**Mode B - Reserved carve-out (requires firmware or vendor support).**
Firmware reserves a fixed sub-range of the framebuffer for non-WDDM use, or
the KMD exposes a shared allocation that VidMm will not evict. SiliconFuse
maps only that range. Leases are backed by the carve-out, not by arbitrary
VidMm pages.

**Mode C - Cooperative broker (research target).**
A privileged broker queries VidMm budget, requests eviction of specific
allocations, and receives a lease only after VidMm confirms the pages are
free. This requires an interface that Microsoft and the KMD vendor must
provide. Without that interface, Mode C is not implementable and must not be
simulated by guessing at VidMm state.

The v0.1 specification commits to Mode A for the first kernel build. Mode B is
the first production-viable target. Mode C is a research item for discussion
with Microsoft and NVIDIA, not a shipped feature.

### 5.3 Direct CPU mapping of an own allocation (measured)

Phase 0 produced a result that narrows the problem. A D3D12 committed resource
created with `D3D12_HEAP_TYPE_CUSTOM`, `CPU_PAGE_PROPERTY_WRITE_COMBINE` and
`MEMORY_POOL_L1` was accepted by the runtime on the reference platform, and
`ID3D12Resource::Map` returned a CPU pointer into the allocation. The copy
engine read the same 64 MB back and matched the CPU-written pattern bit for
bit, which proves the pointer addresses VRAM and not a shadow buffer.

Measured with `tools/ReBarBench` (64 MB allocation, AVX2 kernels):

```
Sequential write (NT stores)     10.2-10.9 GB/s
Sequential read (uncached)          35-37 MB/s
Sequential read (MOVNTDQA)        250-252 MB/s
Access latency                  0.80-0.94 us per 64-bit read
Random 4K write                2.0-2.8 M IOPS (0.4-0.5 us)
Random 4K read                 8.9-9.6 K IOPS (105-113 us)
Random 64K read                    565-595 IOPS (1.68-1.77 ms)
Copy-engine read of same data  12.5-12.6 GB/s
```

Implications:

1. A client does not need a kernel driver to reach its *own* VRAM; D3D12 is
   sufficient. This is the basis for a user-mode Tier 1 client library.
2. Write path: write-combined stores are bandwidth-bound and reach roughly
   10 GB/s, within 20 percent of the copy engine.
3. Read path: uncached MMIO reads are latency-bound (~0.8 us each) and are
   roughly two orders of magnitude slower than DMA. Bulk reads must go through
   the copy engine; MMIO reads are for control data only.
4. The behaviour is driver-specific. It is not guaranteed by the D3D12
   contract, so the client must probe it at startup and fall back to DMA.
5. Mapping another client's pages is still impossible without a kernel broker.
   Modes A/B/C above remain the only paths for shared or reserved memory.

### 5.4 Lease state machine

```
        acquire                 map
FREE ----------> RESERVED ----------> ACTIVE
  ^                 |                   |
  |                 | release           | revoke (WDDM pressure / TTL)
  |                 v                   v
  +------------- REVOKED <---------- EVICTING
                                      |
                                      | flush failure
                                      v
                                   FAULTED
```

Rules:

- `ACTIVE -> EVICTING` is initiated only by the broker, never by the client.
- `EVICTING` has a bounded drain budget (default 2 s). Exceeding it forces
  `FAULTED` and invalidates mappings; data loss is reported to the client.
- `FAULTED` is terminal for the lease. The client must re-acquire.
- A lease never returns to `FREE` with dirty data still in Tier 1.

### 5.5 Ordering and visibility

Writes to Tier 1 are posted PCIe transactions. Before a lease is handed to the
GPU (or released), the driver must issue a write-combining flush
(`KeMemoryBarrier`, `MmFlushForWriteCombinedMemory` semantics) and, where the
device consumes the data, a doorbell or explicit fence through the owning
driver. SiliconFuse does not program GPU fences itself.

### 5.6 Chunked pool and granular eviction (Phase 2.1, measured)

A monolithic 4 GB `WRITE_COMBINE` / L1 resource is refused by WDDM on the
reference machine. Four independent 1 GB slices are granted, so the pool is
built as a `PoolSlice[]` array, each slice a separate resource with its own
CPU mapping, dirty range and spill offset.

Eviction is deficit-driven:

```
deficit = max(usage - budget, (baseline_free - slice_bytes) - free_now)
```

Slices are released FIFO until the freed bytes cover the deficit. The
`slice_bytes` tolerance prevents baseline chasing, which would always drain the
whole pool.

| Case | 6 GB load | 8 GB load |
|---|---|---|
| Pool | 4096 MB (4 x 1024) | 4096 MB (4 x 1024) |
| Dirty set | 2048 MB (512 per slice) | 2048 MB |
| Fill throughput | 5.26 GB/s | 5.09 GB/s |
| Deficit | 1039 MB | 2053 MB, then 1012 MB |
| Slices released | 2 (2 GB) | 3 + 1 (4 GB) |
| Resident after yield | 2048 MB | 0 MB |
| Eviction | 353 ms (copy 231 ms) | 1056 ms (first release 372 ms) |
| Restore | 224 ms | 472 ms |
| Pressure commit | 6144 MB, 0.3 s | 8192 MB, 0.77 s |
| Outcome | no TDR, final ACTIVE | no TDR, final ACTIVE |

Findings:

1. `QueryVideoMemoryInfo` is a **per-process** budget. In several runs it
   shrank for the broker only sometimes; in others WDDM paged silently and the
   broker was never notified. A cooperative broker cannot rely on it alone.
   Global free VRAM (`nvml.dll`, loaded dynamically) with a 1 GB tolerance is
   the second signal that makes arbitration deterministic.
2. Eviction cost is dominated by page release for the first slice (372 ms of
   the 926 ms first round). Later slices release in 13-16 ms. Copy itself is
   91-202 ms per 512 MB slice under an 11 GB/s competing load.
3. Partial eviction is real: a 6 GB load releases 2 of 4 slices and keeps
   2 GB resident; an 8 GB load releases all 4. This is the behaviour the pool
   is for, and it is not achievable with one monolithic resource.

Safety: a run started while a game was active exhausted system resources and
crashed the game (`ERROR_NO_SYSTEM_RESOURCES`). Both tools now abort above
40 percent median GPU utilization unless `--allow-busy` is passed.

Consequence for Phase 3: the kernel driver must treat budget notifications as
a hint, maintain its own residency telemetry, and evict in slices. A
reservation (Mode B) remains the only way to bound the handoff deterministically.

---

### 5.7 Phase 3 user-mode engine (implemented)

`CHZM_SiliconFuse.exe` packages the proven pool into a control application:

- `src/engine/VramPool.cpp` - 4 x 1 GB slices, 64 MB dirty window per slice,
  two-signal arbitration (WDDM budget event + NVML free VRAM), manual
  evict/restore, live statistics.
- `src/engine/StorageBridge.cpp` - host staging buffer plus asynchronous spill
  file. A read served from staging is a memcpy; the disk write is a background
  thread and never blocks the read path.
- `src/gui/MainWindow.cpp` - dark control surface with live RAM/VRAM bars,
  system commit, last DMA throughput, power button and autostart toggle.

Measured: manual eviction of four slices 89.97 ms, restore 158.91 ms, window
920x540 with a dark DWM caption. The binary is verified by running it and
capturing the window (`docs/ui.png`).

**Not implemented, and why.** The brief asks for an NTFS volume, a page file
and `NtCreatePagingFile(..., flags = 0x1C000000)` with `HybridPriority = 7`.
None of that exists in this build:

1. A user-mode process cannot create a block device. There is no SiliconFuse
   volume, so there is nothing to format. Calling `FormatVolume` on a real
   volume would destroy data and is never attempted.
2. `NtCreatePagingFile` is declared as
   `NTSTATUS NtCreatePagingFile(PUNICODE_STRING PageFileName,
   PLARGE_INTEGER MinimumSize, PLARGE_INTEGER MaximumSize, ULONG Priority)`.
   It has no flags parameter and no `HybridPriority` value; `0x1C000000` is
   not a valid Priority.
3. A page file on a pool that the engine evicts on demand is a direct path to
   `MEMORY_MANAGEMENT (0x1A)` or `0xC2`, which is the failure class the project
   exists to avoid.

`PagingManager` therefore reports `not attempted` with the reason and leaves
the existing C: pagefile untouched. The block device and pagefile belong to
Phase 4, after a signed WDF driver exists.

## 6. Kernel architecture (planned)

```
+-------------------------------------------------------------+
| user mode                                                   |
|   client library  ->  \\.\ChzmSiliconFuse  (IOCTL)          |
+-------------------------------------------------------------+
| kernel mode                                                 |
|   ChzmSiliconFuse.sys  (KMDF, companion, not a display KMD) |
|     - lease manager + range tree                            |
|     - MDL mapper (WC for Tier 1)                            |
|     - policy engine, eviction worker                        |
|     - telemetry ring buffer                                 |
|   broker interface (Mode B/C)  <->  WDDM KMD / firmware     |
+-------------------------------------------------------------+
| hardware                                                    |
|   DDR4 (Tier 0)   GDDR6 via BAR1 (Tier 1)   NVMe (Tier 2)   |
+-------------------------------------------------------------+
```

Design decisions:

- KMDF, not a legacy WDM driver: power and PnP handling are mandatory for a
  device that can be surprise-removed.
- The driver is a *filter-independent companion*, enumerated as a software
  device. It does not attach to the PCI device stack of the GPU, so it cannot
  interfere with `nvlddmkm` binding.
- Physical ranges are obtained from the broker, never by walking PCI config
  space directly. Config-space access from a companion driver is not supported
  and would race with the owning driver.
- `METHOD_BUFFERED` for all control IOCTLs. No `METHOD_NEITHER`; no direct
  dereference of user pointers.
- MDLs are built with `MmProbeAndLockPages`; Tier 1 mappings use
  `MmMapLockedPagesSpecifyCache(..., MmWriteCombined, ...)`.
- The driver refuses to load if IOMMU is disabled on a platform where the
  aperture is above 4 GB and the firmware does not describe the resource
  (fail closed, not open).

### 6.1 Power and removal

- On `D0Exit`, all Tier 1 leases are flushed and released. Tier 1 is volatile;
  the driver cannot survive a GPU power transition with live mappings.
- On surprise removal, leases enter `FAULTED`, clients receive
  `STATUS_DEVICE_REMOVED`, and the range tree is torn down without touching
  the aperture.
- On resume, the probe sequence is re-run; the aperture base may change, so
  cached physical addresses are invalidated.

---

## 7. Security and safety

Mapping framebuffer physical memory into user space is equivalent to giving
the process a window into every other client's GPU allocations. The threat
model assumes hostile local users and compromised user-mode processes.

Mandatory controls:

1. The driver is signed (test signing is not acceptable outside a lab) and
   compatible with HVCI and WDAC.
2. Leases are per-handle and per-process; no global mapping IOCTL exists.
3. The range tree rejects any mapping that overlaps a non-leased range or
   crosses the framebuffer boundary.
4. IOMMU (VT-d / AMD-Vi) must be enabled and the driver checks it at load.
5. No raw PFN interface, no `\Device\PhysicalMemory` dependency, no arbitrary
   physical read primitive.
6. Tier 1 mappings are revoked on process termination, session lock policy
   change and adapter reset.
7. Telemetry never records buffer contents, only addresses, sizes and states.

Residual risk: a signed kernel driver with the ability to map arbitrary
physical ranges is a rootkit primitive. The design constrains the range to the
leased aperture, but the constraint is only as strong as the implementation
and the signing process. This is why Phase 2 is gated on code review and a
dedicated test machine.

---

## 8. Failure modes

| Failure | Detection | Response |
|---|---|---|
| Aperture smaller than framebuffer | Probe validation | Refuse Tier 1; log at boot |
| ReBAR disabled | Aperture < 1 GB | Tier 1 unavailable; Tier 0/2 only |
| BAR base changes after resume | Probe re-run | Invalidate all cached physical addresses |
| WDDM reclaims a lease | Broker revoke | Flush, invalidate, `REVOKED` |
| Drain timeout | Watchdog (2 s) | `FAULTED`, client notification |
| GPU reset / TDR | Device state query | Flush and release all Tier 1 leases |
| PCIe correctable errors | WHEA counters | Log, throttle migration |
| NVMe removal during spill | Block stack completion | `FAULTED`, promote back to RAM |

---

## 9. Bring-up plan

**Phase 0 - Cartography (complete).**
Read-only probe. Reproduces BAR0/BAR1/base/size, RAM size, DRAM type and
framebuffer size. Cross-checked against `nvidia-smi` and SMBIOS.

**Phase 1 - User-mode broker.**
Lease protocol implemented against a synthetic reserved buffer. Measures
protocol overhead. No physical mapping.

**Phase 2 - Read-only kernel mapping.**
Mode A only. Map a reserved range, read it, verify against a known pattern,
unmap. Requires test signing, IOMMU on, and a machine where the display driver
is not bound to the target GPU.

**Phase 3 - Arbitration and eviction.**
Mode B with a vendor or firmware carve-out. 72-hour soak with WDDM pressure,
no display corruption, bounded drain latency.

**Phase 4 - Tier 2 and telemetry.**
NVMe spillover with writeback batching, per-tier statistics, fault injection.

Each phase has a hard gate: no phase starts until the previous phase has a
reproducible test and a documented failure log.

---

## 10. Metrics

The architecture is evaluated on:

- effective capacity per tier under a mixed workload;
- migration throughput (GB/s) and CPU cost per GB;
- p50/p99 lease acquisition latency;
- fault rate and recovery time after revocation;
- display corruption incidents (target: zero);
- performance delta of the WDDM workload with and without the pool active.

A pool that improves a synthetic benchmark but degrades the primary display
workload is a regression, not a feature.

---

## 11. Open questions

1. Does any Windows interface expose VidMm allocation residency to a
   non-display kernel driver? If not, Mode C cannot exist.
2. Can firmware reserve a BAR1 sub-range without breaking the KMD's own
   aperture mapping?
3. What is the real sustained CPU read bandwidth from BAR1 on this chipset
   with WC vs UC mappings? Needs measurement, not assumption.
4. Does the platform support PASID/ATS for the GPU, and would that change the
   coherence story?
5. What is the correct behaviour when the aperture base moves across a
   driver update?

---

## 12. Risks and unverified claims

- The 60.8 GB/s and 15.75 GB/s theoretical figures are line rates. Phase 0
  measured 11.6-12.5 GB/s through the copy engine and 10.2 GB/s through direct
  WC stores; the theoretical values remain ceilings.
- PCIe link generation and width were not read. The platform may negotiate
  below 3.0 x16; the probe does not verify it.
- The 28 GB envelope assumes 16 GB RAM and 12 GB VRAM. It is not achievable
  as allocatable memory on a live system.
- Mode C depends on interfaces that may not be publicly available. Treat it
  as a research direction.
- The probe falls back to the firmware boot configuration when the live
  Configuration Manager query returns `CR_FAILURE`, which is normal for display
  adapters. After a driver-initiated BAR resize the two can differ; the report
  states which source was used.
- Accepting `CUSTOM + WRITE_COMBINE + L1` is driver and WDDM-version specific.
  It was observed on WDDM 3.x with driver 32.0.16.1742 and must be probed at
  run time, never assumed.
- Direct MMIO read numbers depend on CPU memory-ordering behaviour under
  uncached loads; the 4K/64K random read results are per-access latencies, not
  batched throughput.
- No kernel code from this project has been built, signed or run.
