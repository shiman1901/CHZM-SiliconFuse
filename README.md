Русская версия: [README_RU.md](README_RU.md)

# CHZM-SiliconFuse

Version 0.3.1. User-mode memory hierarchy engine for Windows 11 x64.

## 1. Overview

CHZM-SiliconFuse is a user-mode engine that manages a three-tier memory
hierarchy on a desktop PC and reclaims RAM for games:

- DDR4 system memory;
- GDDR6 video memory, exposed to the CPU through the Resizable BAR aperture;
- NVMe storage, used as a durable mirror and spillover.

On a 16 GB machine, a modern game together with Chrome, Steam, Discord and
Telegram can fill physical RAM. Windows then pages the game to disk, which
produces stutter, long loads and constant page-fault activity. SiliconFuse
reduces that pressure by moving the cold working sets of background
applications out of DDR4 while leaving the game itself untouched.

The engine works entirely in user space. It uses no kernel driver and does not
modify the system pagefile. The mechanisms are:

- Direct3D 12 custom heaps: `D3D12_HEAP_TYPE_CUSTOM` with
  `CPU_PAGE_PROPERTY_WRITE_COMBINE` and `MEMORY_POOL_L1`, which gives the CPU a
  pointer into the GPU framebuffer through BAR1;
- non-temporal AVX2 stores (`_mm256_stream_si256`) for writes into VRAM;
- the D3D12 Copy Engine (DMA) for reads from VRAM;
- standard Win32 and NT memory management APIs for working sets, page
  priorities, quotas and process control.

The result is a practical way to give a game several gigabytes of headroom on a
16 GB system without rebooting, without a driver and without risking the
contents of live applications.

## 2. How It Works

The engine has two separate operating contours. They share the VRAM pool, the
storage bridge and the hash implementation, but they have different safety
rules.

### 2.1 Contour 1: Live game mode (Safe Autopilot and Game Boost Reclaim)

This is the production path for real applications and games on 16 GB systems.

In Live mode the engine:

- never freezes threads: `NtSuspendProcess` is not used;
- never discards memory: `MEM_RESET` is not used;
- never writes an old snapshot over a live process: `WriteProcessMemory` is
  forbidden in Live mode;
- does not use EcoQoS, so CPU frequency is not reduced and video playback in a
  browser or a messenger interface does not stutter.

**Game-Aware VRAM Yield.** When a running game is detected (`Cyberpunk2077.exe`
and others), the engine does not allocate 1 GB slices in video memory. The game
keeps 100 percent of its VRAM, and the engine compresses background programs in
DDR4 instead.

**Immunity Rules.** Several process roles are protected from working set
eviction:

- the game itself (`[GAME_IMMUNE]`);
- the process family of the window that currently has focus;
- browser GPU compositors (`--type=gpu-process`);
- audio processes (`--type=utility`, plus any process with an active WASAPI
  stream detected through `IAudioMeterInformation::GetPeakValue`);
- the `Telegram.exe` interface (`[UI_PROTECTED]`).

**Compression and wake mechanics.** Before a reference snapshot is copied into
BAR1, the pages are filtered through `QueryWorkingSetEx`. Only pages with
`VirtualAttributes.Valid == 1` are copied, that is, only pages that are
physically resident. This avoids generating tens of thousands of page faults
during the snapshot.

Background renderers (`chrome.exe`, `steamwebhelper.exe`, `Discord.exe`) get a
lower page priority (`ProcessMemoryPriority = 1`) and a single flush of cold
pages (`EmptyWorkingSet`). They are not locked into a hard quota
(`HARDWS_MAX_ENABLE` is not set).

When the user switches windows (Alt+Tab, focus watcher interval 20 ms),
`WakeGroup` works from cached handles and restores `ProcessMemoryPriority = 5`
in 0.087 to 0.20 ms. Working set bounds are reset with explicit parameters
(1 MB, 4096 MB, `HARDWS_MIN_DISABLE | HARDWS_MAX_DISABLE`), which avoids the
kernel behavior in `ntoskrnl!MmAdjustWorkingSetSizeEx` where the pair `(-1, -1)`
triggers `EmptyWorkingSet` instead of clearing the quota. A batched
`PrefetchVirtualMemory` call follows.

### 2.2 Contour 2: Hard Cryo hibernation (only with `--torture-test`)

This contour performs bit-exact hibernation of a dedicated test process. It is
not applied to live user applications.

Freeze sequence:

1. The target process is suspended (`NtSuspendProcess`) with a double thread
   verification.
2. Private RW regions are read in 64 MB chunks.
3. An AVX2 `Hash64` checksum is computed.
4. The data is written into GDDR6 slices through the BAR1 window.
5. The same data is synchronously duplicated into an unbuffered NVMe mirror
   (`cryo_mirror.bin`, 4 GB, `FILE_FLAG_NO_BUFFERING |
   FILE_FLAG_WRITE_THROUGH`, 4096-byte alignment).
6. Only after the mirror is durable are the physical RAM pages reset
   (`MEM_RESET`).

A transactional journal (`cryo_journal.bin`) records every state transition:
`PREPARED -> MIRROR_DURABLE -> SOURCE_RESET -> RESTORED`.

Restore sequence:

1. Data is returned from GDDR6 through DMA.
2. The AVX2 hash is verified.
3. Corrupted blocks are automatically healed from the NVMe mirror after a bit
   flip or a GPU loss (TDR).
4. If both sources are damaged, the engine enters `SAFE_LOCKOUT` and does not
   resume the process.

Crash protection: an independent guard process
(`--recovery-guard <pid>`) watches the engine. If the engine is forcefully
terminated (`TerminateProcess`), the guard restores every affected process from
the journal, validates `creation_time` against PID reuse, and lifts all Live
mode restrictions.

## 3. Measured Benchmarks

All figures below were measured on the reference platform with the release
binary. Raw reports are in [`docs/`](docs/) in the repository and in the
`reports/` folder of the release package.

### 3.1 Test platform

| Component | Value |
|---|---|
| CPU | AMD Ryzen 7 5700X, 8 cores / 16 threads |
| RAM | 16 GB DDR4-3800, dual channel |
| GPU | NVIDIA GeForce RTX 3060 12 GB, Resizable BAR active |
| VRAM aperture | BAR1 16384 MB, usable VRAM 11964 MB |
| OS | Windows 11 x64 |
| Toolchain | MinGW-w64 GCC 16.2.0, `-O2 -mavx2` |

### 3.2 Table 1: Bus and algorithm throughput (Stages 0-8)

| Operation | Path | Result |
|---|---|---|
| Write to GDDR6 through BAR1 | AVX2 `_mm256_stream_si256` | 10,866 to 10,927 MB/s |
| Read from GDDR6 | D3D12 Copy Engine (DMA) | 8,065 to 12,580 MB/s |
| Direct uncached CPU read from BAR1 | MMIO | 37.4 MB/s (250.6 MB/s with `MOVNTDQA`), 939.5 ns per 64-bit read |
| AVX2 `Hash64` checksum | AVX2 xor/add/shuffle | 34.8 to 35.8 GB/s, 512 MB in 14.86 ms |

The read asymmetry is the reason for the split design: writes through BAR1 are
bandwidth-bound and reach PCIe line rate, while uncached MMIO reads are
latency-bound. The engine therefore writes through BAR1 and reads through the
copy engine.

### 3.3 Table 2: Real game scenario on 16 GB DDR4 (Stage 13)

Scenario: Cyberpunk 2077 plus Chrome, Steam, Discord and Telegram.

System state under peak load:

| Metric | Before | After |
|---|---|---|
| Used RAM | 15.1 / 15.9 GB (95 percent) | 12.1 / 15.9 GB (76 percent) |
| Available RAM | 820 MB | 3,800 MB |
| Cyberpunk 2077 disk swap | 3.4 MB/s | 0 MB/s |

Clean `--real-world-benchmark` pass, per process family:

| Process family | Processes (protected) | Working set before, MB | Working set after, MB | Delta, MB | Private WS before, MB | Private WS after, MB | Delta, MB |
|---|---|---|---|---|---|---|---|
| `cyberpunk2077.exe` | 1 (1) | 4055 | 4055 | 0 | 3394 | 3394 | 0 |
| `chrome.exe` | 28 (6) | 1280 | 153 | -1127 | 975 | 82 | -893 |
| `steamwebhelper.exe` | 8 (5) | 578 | 195 | -382 | 348 | 25 | -323 |
| `discord.exe` | 6 (4) | 281 | 173 | -108 | 107 | 31 | -76 |
| `telegram.exe` | 1 (0) | 77 | 77 | 0 | 37 | 37 | 0 |

Totals for the pass:

| Metric | Result |
|---|---|
| Reclaimed working set | 1637 MB |
| Reclaimed private working set | 1299 MB |
| `WakeGroup` restore time | 0.087 ms (`prefetch` 0.009 ms) |
| Crashed processes | 0 |

The game family reports a delta of 0 MB by design (`[GAME_IMMUNE]`), and the
Telegram interface reports 0 MB by design (`[UI_PROTECTED]`). Chrome peaked at
3141 MB before the pass and was reduced to 124 MB.

### 3.4 Table 3: Chrome validation bench with live JavaScript load (Stage 12.1)

Setup: 13 Chrome processes in the background, 3 tabs with active 64 MB buffers
in the V8 heap, checksum verification every 40 ms.

| Metric | Result |
|---|---|
| Trimmed working set | 727 MB |
| Trimmed private working set | 355 MB |
| Resident page snapshot copied to GDDR6 | 146 MB |
| Page faults during the snapshot, before the `QueryWorkingSetEx(Valid == 1)` filter | 122,902 |
| Page faults during the snapshot, after the filter | 1,665 (73 times fewer) |
| `restore_priority_and_quotas_ms` | 0.20 ms (about 120 times faster than 24 to 55 ms) |
| JavaScript heap integrity | `HASH:MATCH` |
| Crashed processes | 0 |
| Priority recovery after `TerminateProcess` | PASS (1 -> 5) |

### 3.5 Table 4: Hard Cryo hibernation and chaos engineering (Stages 7-11)

| Test | Result |
|---|---|
| Recovery matrix, 64 / 128 / 256 / 512 / 1024 MB | 39.22 / 78.00 / 176.75 / 391.07 / 1199.94 ms, 100.0000 percent bit match |
| Simulated GPU loss (TDR), 256 MB | 13 of 13 regions raised from the NVMe mirror in 190.92 to 256.32 ms, 100.0000 percent |
| Bit flip in VRAM, 128 MB | corrupted chunk rejected by the AVX2 hash and healed from disk, 100.0000 percent |
| Corruption of both sources (VRAM and disk) | `SAFE_LOCKOUT` triggered, process not resumed with damaged data |
| Mirror write aborted at 50 percent | clean transactional rollback before `MEM_RESET`, process intact at 100.0000 percent |
| Engine killed (`TerminateProcess`, code 99) | `--recovery-guard` restored the victim at 100.0000 percent |
| Forged journal record with a foreign `creation_time` (PID reuse) | rejected with exit code 2 |

## 4. Repository Structure, Build and CLI

### 4.1 Repository structure

```text
CHZM-SiliconFuse/
├── src/
│   ├── main.cpp              Entry point, benchmark and test drivers, guard
│   ├── engine/               VramPool, StorageBridge, CryoSwap, CommitGuard,
│   │                         PagingManager, StandbyPurge, Hash64
│   ├── gui/                  GDI interface (740x440), tray integration
│   ├── common/               Shared user/kernel pool protocol header
│   └── kernel/               Design notes only; no driver code is shipped
├── docs/                     Architecture, whitepaper and measurement reports
├── build_final.bat           Master binary build script (MinGW-w64)
├── build_probe.bat           Hardware topology probe build
├── build_bench.bat           Direct BAR1 and DMA benchmark build
├── build_broker.bat          WDDM lease broker build
├── README.md                 This document
└── README_RU.md              Russian version
```

### 4.2 Build

Requirements:

- Windows 11 x64;
- MinGW-w64 g++ with C++20 support (tested with GCC 16.2.0);
- a CPU with AVX2;
- a GPU with Resizable BAR enabled for the GDDR6 tier.

Build the master binary:

```bat
build_final.bat
```

The script compiles `src/main.cpp` and the engine and GUI units with
`-std=c++20 -O2 -mavx2 -static`, embeds the application icon through
`windres`, links `d3d12`, `dxgi`, `dwmapi`, `psapi`, `gdi32`, `msimg32`,
`shell32`, `user32`, `ole32` and `uuid`, and writes
`build\CHZM_SiliconFuse.exe`.

### 4.3 Command line modes

| Command | Purpose |
|---|---|
| `CHZM_SiliconFuse.exe` | GDI graphical interface, 740x440, live RAM/VRAM/commit telemetry |
| `CHZM_SiliconFuse.exe --real-world-benchmark` | Measurement and optimization on the live system |
| `CHZM_SiliconFuse.exe --live-combat-test` | Isolated background Chrome test with live JavaScript load |
| `CHZM_SiliconFuse.exe --torture-test` | Full Hard Cryo stress suite (Stages 7-11) |
| `CHZM_SiliconFuse.exe --release-all` | Forced reset of all memory quotas and priorities in the system |

### 4.4 Scope and limitations

- Hard Cryo is available only through `--torture-test` and is not applied to
  live applications. Live mode is the safe path for everyday use.
- The engine is user mode only. It does not create a block device, does not
  format a volume and does not attach or modify a pagefile.
- `StandbyPurge` requires an elevated token. Without elevation the call is a
  logged no-op.
- Acceptance of `D3D12_HEAP_TYPE_CUSTOM` with `WRITE_COMBINE` and
  `MEMORY_POOL_L1` depends on the GPU driver and the WDDM version. The
  benchmark probes this combination at run time.
- A single 4 GB write-combined L1 allocation is refused on the reference
  machine. Four 1 GB slices are granted; the engine uses slices.
- All figures come from the reference platform listed in section 3.1. Results
  on other hardware will differ.
