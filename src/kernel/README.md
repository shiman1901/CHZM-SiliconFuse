# src/kernel - planned Ring-0 component

This directory is reserved for the SiliconFuse companion driver. It contains
no source code at v0.1 by design: a driver that maps framebuffer physical
memory is a security boundary, and it will not be published before the
arbitration contract in `../../docs/ARCH_SPEC.md` is validated.

Planned layout:

```text
src/kernel/
├── ChzmSiliconFuse.sys       KMDF companion (software device)
├── driver.c / driver.h       DriverEntry, EvtDeviceAdd, PnP, power
├── lease.c / lease.h         Lease manager, range tree, state machine
├── map.c / map.h             MDL construction, WC mapping, unmap
├── policy.c / policy.h       Placement, promotion, eviction worker
├── telemetry.c               Ring buffer, ETW provider
├── broker.c / broker.h       Mode B/C interface (vendor or firmware)
└── ChzmSiliconFuse.inf       Installation, service, security descriptor
```

Hard requirements before any code lands here:

- KMDF, not legacy WDM. Surprise removal and D-state transitions must be
  handled explicitly.
- The driver enumerates as a software device and does not attach to the GPU
  PCI stack. Interfering with `nvlddmkm` binding is not acceptable.
- Physical ranges come from the broker, never from direct PCI config access.
- All control IOCTLs use `METHOD_BUFFERED`. No `METHOD_NEITHER`, no raw user
  pointers.
- Tier 1 mappings use `MmMapLockedPagesSpecifyCache(..., MmWriteCombined, ...)`
  and are revoked on process exit, adapter reset and power transition.
- IOMMU must be enabled; the driver fails closed if it cannot confirm it.
- Test signing is acceptable only on the lab machine. Public builds must be
  signed and HVCI-compatible.

Build toolchain (when code exists): Visual Studio 2022 + Windows Driver Kit
10.0.26100 or newer, x64 target, `/W4 /WX`.

The user/kernel ABI is already fixed in `../common/chzm_pool_protocol.h`
(ABI version 1). Do not change it without bumping `CHZM_POOL_ABI_VERSION`.
