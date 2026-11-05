# SiliconFuse: A Cooperative Unified Memory Architecture for Windows 11

**Position paper and design manifesto**
CHZMINFINITYCORE, 2026-11
Version 0.1 - for technical review

---

## Abstract

Commodity workstations contain two pools of fast volatile memory that the
operating system treats as unrelated: system DRAM behind the CPU, and GDDR6
behind the discrete GPU. PCIe Resizable BAR makes the framebuffer visible in
the CPU physical address space for the first time, but Windows provides no
allocation model that treats the two pools as one hierarchy. Applications
therefore choose one pool at design time and copy across the bus at run time,
or they rely on vendor-specific mechanisms that work only inside a single
runtime.

SiliconFuse specifies a different arrangement. A companion kernel component
manages revocable leases over three media - DDR4, GDDR6 through the ReBAR
aperture, and NVMe as spillover - and exposes them through one protocol. The
critical design decision is that SiliconFuse does not attempt to replace WDDM
or to consume framebuffer pages that VidMm already owns. It negotiates, and
where negotiation is impossible it refuses to map.

This paper states the problem, defines the address model and the lease
contract, documents the WDDM conflict with measured data from a reference
platform, and specifies the experiments that must succeed before any kernel
mapping is shipped. It explicitly separates what has been measured from what
is proposed.

---

## 1. Problem

A modern desktop has 16 GB of DRAM and 12 GB of VRAM. Neither pool alone is
large enough for the largest working sets users actually run: model weights,
photogrammetry caches, video timelines, simulation state. The aggregate is
28 GB, but there is no supported way to allocate across the boundary.

The current options are unsatisfying:

- **Copy-based offload (CUDA, DirectML, DirectStorage).** The programmer
  partitions data manually. Copies are explicit, latency is exposed, and the
  model does not compose with ordinary CPU code.
- **Unified memory in vendor runtimes.** CUDA managed memory works inside
  CUDA. It does not provide a system-wide allocation that a Win32 process can
  use without the runtime.
- **User-mode block proxies (ImDisk and similar).** A file or a RAM disk is
  backed by a proxy that copies through user mode. Every I/O pays a context
  switch and a copy; the result is not memory, it is storage with extra steps.
- **Pagefile on a VRAM-backed volume.** The same proxy problem, plus the
  memory manager's assumptions about latency are violated.

None of these provides the property users want: one allocation primitive whose
placement is decided by the operating system's memory policy, not by the
application's choice of API.

---

## 2. Background

### 2.1 Resizable BAR

PCIe Resizable BAR lets firmware resize a device BAR beyond the legacy
256 MB window. For a 12 GB framebuffer the OS assigns a 16 GB aperture, because
BAR sizes are powers of two. The aperture is a CPU-visible MMIO window over
the framebuffer; it is not a second copy of memory and it does not increase
VRAM capacity.

Measured on the reference platform (RTX 3060 12 GB, Ryzen 7 5700X):

```
BAR1 base   0x00000078_00000000
BAR1 size   16384 MB (power-of-two aperture)
Framebuffer 12288 MB
BAR0        16 MB @ 0x00000000_F5000000
```

The 4 GB difference between aperture and framebuffer is unbacked address
space. Software that maps it will read undefined data or fault.

Direct CPU access through that aperture is measurable. A D3D12 allocation made
with a write-combined L1 custom heap was mapped into user space, written with
AVX2 streaming stores, and read back by the copy engine bit-for-bit:

```
Sequential write (WC, NT stores)   10.2-10.9 GB/s
Sequential read (uncached)            35-37 MB/s
Read (MOVNTDQA)                      250-252 MB/s
Access latency                  0.80-0.94 us per 64-bit read
Copy-engine read of same buffer    12.5-12.6 GB/s
```

The asymmetry is the central design constraint: writes are bandwidth-bound and
competitive with DMA; reads are latency-bound and unusable for bulk data. Any
policy that treats Tier 1 as symmetric memory will fail on reads.

### 2.2 WDDM and VidMm

WDDM owns the adapter. VidMm allocates framebuffer pages, evicts them under
pressure and programs GPU page tables. Its decisions are not exposed to third
parties. On the reference machine, 16355 MiB of the 16384 MiB aperture was in
use while the desktop was otherwise idle. There is, in practice, no free VRAM
to pool.

This is the central engineering constraint of the project. Any architecture
that assumes otherwise is describing a machine that does not exist.

### 2.3 Existing work

- CUDA Unified Memory implements migration inside one runtime and one vendor
  ecosystem.
- DirectStorage provides asynchronous storage-to-VRAM transfer, not a unified
  allocator.
- Linux has `HMM`, `dmabuf` and device memory cgroups; the Windows equivalent
  for discrete VRAM does not exist as a public contract.
- Research systems (e.g., heterogeneous memory managers in HPC) rely on
  kernel cooperation that Windows does not expose.

SiliconFuse does not claim novelty in tiering. Its contribution is a concrete
Windows-compatible contract and an honest boundary: what can be built with
documented interfaces, and what requires vendor or platform cooperation.

---

## 3. Design principles

1. **Measure before mapping.** No physical address is used until a read-only
   probe has validated it against an independent source.
2. **Never own what you cannot revoke.** Every Tier 1 mapping is a lease with
   a bounded lifetime.
3. **Fail closed.** If the platform cannot prove the range is safe, the range
   is not mapped.
4. **One allocation primitive.** Clients ask for bytes and access intent, not
   for a tier.
5. **No hidden copies in the hot path.** Migration is explicit, accounted and
   observable.
6. **The display workload wins.** If pool activity degrades the primary
   display path, the pool backs off.

---

## 4. Architecture

### 4.1 Tiers

| Tier | Medium | Capacity | Latency | Bandwidth |
|---|---|---|---|---|
| 0 | DDR4-3800 dual channel | 16 GB | 60-70 ns | 60.8 GB/s |
| 1 | GDDR6 via PCIe 3.0 x16 BAR1 | 12 GB | 0.6-1.5 us | 15.75 GB/s |
| 2 | NVMe | policy | 50-100 us | ~3.5 GB/s |

Tier 1 is an order of magnitude slower than Tier 0 and two orders faster than
Tier 2. Placement policy is therefore latency-driven: hot, fine-grained,
pointer-chasing structures stay in DRAM; bulk streaming and device-consumed
buffers may live in VRAM; cold data spills to NVMe only under pressure.

### 4.2 Lease protocol

A lease is a revocable grant of a contiguous byte range inside one tier. It
carries an access intent (read-only, write-back, write-combined), a priority
and a TTL. The kernel driver, not the client, resolves the range to a physical
address and installs the mapping.

State machine:

```
FREE -> RESERVED -> ACTIVE -> EVICTING -> REVOKED
                                  |
                                  +-> FAULTED (drain timeout)
```

The broker can revoke at any time. Clients are required to tolerate
revocation; a client that blocks the drain is terminated.

### 4.3 WDDM coexistence

Three modes are defined:

- **A. Exclusive compute.** The adapter is not driving a display. The pool owns
  the aperture. Safe with today's interfaces.
- **B. Reserved carve-out.** Firmware or the KMD reserves a sub-range that
  VidMm will not allocate. The pool maps only that range. Requires vendor or
  platform support.
- **C. Cooperative broker.** The pool requests pages through a VidMm-aware
  interface. This interface does not publicly exist; it is a research target,
  not a shipped feature.

Phase 2 of the project implements Mode A only. Publishing a Mode C
implementation built on undocumented behaviour would be irresponsible and is
out of scope.

---

## 5. Evaluation plan

The claims that must be tested, in order:

1. **Topology reproducibility.** The probe must produce identical BAR1 base and
   size on repeated boots and on a second platform. Cross-check against
   `nvidia-smi` BAR1 totals and SMBIOS.
2. **Aperture bounds.** Confirm that accessing `[base + 12 GB, base + 16 GB)`
   is invalid. This defines the safe limit.
3. **Mapping throughput.** Measure CPU read and write bandwidth for WC and UC
   mappings over BAR1. Theoretical 15.75 GB/s is a ceiling, not a result.
4. **Lease overhead.** Acquisition, mapping and revocation latency at p50 and
   p99.
5. **Revocation under pressure.** Force VidMm pressure and measure drain time,
   data integrity and display stability.
6. **End-to-end workload.** A working set larger than DRAM, with and without
   the pool, on a workload that also drives the display.

Negative results are publishable. A failed Mode B experiment is more valuable
than an unverified claim of 28 GB.

### 5.1 Measured results, Stage 7-8 (2026-11-05)

Reference platform: RTX 3060 12 GB (BAR1 16384 MiB, 4 x 1 GB WC L1 slices),
32 GB DDR4, Windows 11.

CryoSwap hibernates private pages of a background process into GDDR6, drops
them with `MEM_RESET` (no pagefile write) and restores them through the copy
engine plus `WriteProcessMemory` on focus. The test wipes the RAM copy with
zeros before restoring, so the only surviving source is VRAM.

| Size | WS before -> after | Avail RAM | DMA | Hash (AVX2) | WPM | Total restore | Integrity |
|---|---|---|---|---|---|---|---|
| 64 MB | 72.7 -> 24.0 | +34 MB | 8.9 ms | 1.71 ms | 18.9 ms | 39.22 ms | 100% |
| 128 MB | 136.7 -> 24.0 | +103 MB | 16.7 ms | 3.58 ms | 48.7 ms | 78.00 ms | 100% |
| 256 MB | 264.7 -> 24.0 | +220 MB | 33.1 ms | 7.28 ms | 127.3 ms | 176.75 ms | 100% |
| 512 MB | 520.7 -> 23.9 | +489 MB | 64.6 ms | 14.86 ms | 302.4 ms | 391.07 ms | 100% |

Ingress for 512 MB costs 190.1 ms in total (suspend 20.9, `ReadProcessMemory`
41.9, AVX2 store into BAR1 47.2, checksum 14.6, `MEM_RESET` + quota + trim
47.1). The store runs at 10.9 GB/s; the cross-process read is the limiter.

Control experiment (Mode B): `EmptyWorkingSet` alone drops the working set to
0 MB but produces exactly one major fault per 4 KB on the next touch
(131 072 faults for 512 MB) and keeps nothing in VRAM. CryoSwap turns that into
a 249 ms write-back of known-good bytes.

Reproduction: `CHZM_SiliconFuse.exe --torture-test`, output in
`docs/STAGE8_BENCHMARK_MATRIX.txt`.

### 5.2 Fail-safe recovery, Stage 9

Hibernating a live process into VRAM creates a single point of failure: the
moment a game or the driver needs the aperture, the slices are gone. The engine
therefore keeps a second copy.

- Every chunk streamed into a slice is also written synchronously to a
  disk-backed mirror in `StorageBridge` (`cryo_mirror.bin`, 4 GB).
- Restore tries VRAM first, verifies the AVX2 checksum, and silently rebuilds
  any region that fails from the mirror. Only proven bytes reach
  `WriteProcessMemory`.
- `NtResumeProcess` is withheld if a region cannot be proven from either
  source: the process stays suspended and the failure is logged as CRITICAL.
- The destructor and the window shutdown paths call `RestoreAllFrozen()`, so no
  orphaned suspended process can outlive the engine.

Measured on the reference platform:

| Scenario | Outcome |
|---|---|
| 512 MB, exact accounting | 492.03 ms total, phases + overhead match to 0.00 ms |
| 1024 MB, exact accounting | 930.68 ms total, phases + overhead match to 0.00 ms |
| 3 concurrent victims (64/128/256) | 3/3, +307 MB RAM, 100% after shuffled restore |
| Full VRAM eviction while frozen | 13/13 regions from the mirror, checksum MATCH, 182.61 ms, 100% |
| Exit auto-restore | 1/1 resumed, 0 orphans, 100% |

The mirror raises freeze cost from 190 ms to 492 ms at 512 MB; that is the
price of surviving a TDR, and it is paid once per freeze, not per restore.

### 5.3 Chaos engineering, Stage 10

Three failure modes were injected deliberately:

1. **Unbuffered mirror.** `cryo_mirror.bin` now uses
   `FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH` with sector-aligned
   staging. `GetProcessIoCounters` confirms 512 MB written and 512 MB read back
   from the device, and available RAM still grows by 507 MB — the Cache Manager
   no longer parks the mirror in DDR4.
2. **Simulated TDR.** With every D3D12 call refused (`device_removed`), restore
   serves 0 regions from the copy engine and 13 from the NVMe mirror, verifies
   the checksum and returns 100.0000% bit-for-bit in 256 ms.
3. **Bit flips.** One inverted byte inside a VRAM slice is caught by the AVX2
   checksum, the region is rejected and rebuilt from the mirror (100.0000%).
   Inverting a byte in *both* sources makes restore fail closed: it returns an
   error, sets `SAFE_LOCKOUT` and never resumes the process.
4. **Mid-freeze abort.** A mirror write is failed at 50%; the freeze rolls back
   before `MEM_RESET`, the victim keeps its original pages and stays 100% intact.

Session semantics follow the same asymmetry: user-initiated exit restores every
frozen process, while a real `WM_ENDSESSION` shutdown exits immediately and
leaves the victims to the OS.

---

## 6. Security and governance

A driver that maps framebuffer physical memory is a privileged primitive. The
project adopts the following constraints:

- signed builds only outside the lab, HVCI-compatible;
- IOMMU required, checked at load;
- no raw PFN interface;
- mappings scoped to leases and revoked on process exit;
- telemetry limited to metadata, never buffer contents;
- public disclosure of limitations before any binary release.

The correct comparison is not "does it work" but "what does a compromised
client gain". Under the current design, a compromised client gains access to
its own leased range and nothing else. That property must be tested, not
assumed.

---

## 7. Limitations

- 28 GB is an envelope, not allocatable capacity. The OS reserves RAM, VidMm
  owns most of the aperture, and leases are revocable.
- Mode C cannot be implemented without interfaces that are not public.
- PCIe 3.0 x16 is a platform assumption. The probe does not measure the
  negotiated link.
- The framebuffer is volatile. Tier 1 cannot hold state across power
  transitions.
- The reference platform is a single machine. Generalisation requires a second
  and third platform with different root complexes and firmware.
- CPU-mappable L1 heaps are a driver behaviour, not a D3D12 guarantee. The
  observed 10 GB/s write path depends on WDDM 3.x and the installed KMD; a
  portable client must probe and fall back.

---

## 8. Call for collaboration

The project is looking for:

- platform documentation on BAR1 sub-range reservation;
- a supported path for a non-display driver to query VidMm residency;
- review of the lease protocol from driver developers with WDDM experience;
- independent reproduction of the topology measurements.

Until those exist, SiliconFuse remains a documented experiment with a working
probe and no unsafe shortcuts.

---

## References

1. Microsoft, "Windows Display Driver Model (WDDM) Architecture", Windows
   driver documentation.
2. Microsoft, "CM_PARTIAL_RESOURCE_DESCRIPTOR", WDK reference.
3. Microsoft, "Resizable BAR", PCI configuration guidance.
4. PCI-SIG, "PCI Express Base Specification 3.0", 128b/130b encoding.
5. NVIDIA, "CUDA Unified Memory Programming Guide".
6. Microsoft, "DirectStorage API", storage acceleration documentation.
7. Linux kernel documentation, "HMM" and "dma-buf" subsystems (for contrast).
