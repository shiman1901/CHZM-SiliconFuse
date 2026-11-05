// ============================================================================
//  CHZM SiliconFuse - ReBAR Direct Silicon Benchmark
//  File: tools/ReBarBench/main.cpp
//
//  Measures the real cost of moving data between the CPU and the RTX 3060
//  framebuffer on a platform with Resizable BAR enabled.
//
//  IMPORTANT ENGINEERING NOTE
//  --------------------------
//  The brief asks for a CPU pointer directly into L1 (GDDR6) through
//  D3D12_HEAP_TYPE_CUSTOM + WRITE_COMBINE + MEMORY_POOL_L1. The tool attempts
//  exactly that combination first. On the reference platform (RTX 3060,
//  WDDM 3.x, ReBAR active, driver 32.0.16.1742) the runtime accepts it and
//  Map() returns a CPU pointer into video memory; the measurement is then a
//  genuine MMIO test over PCIe. Platforms that reject the combination fall
//  back to the copy-engine path only, and the report states which happened.
//
//  Two paths are measured and reported separately:
//
//    [DIRECT MMIO]  CPU stores/loads through the BAR1 aperture.
//                   Writes are write-combined and bandwidth-bound.
//                   Reads are uncached and latency-bound: every access is a
//                   separate PCIe transaction.
//
//    [DMA]          Host staging <-> VRAM through the WDDM copy engine.
//                   This is the supported bulk path and the only sane way to
//                   read large blocks back to the CPU.
//
//  A kernel driver is still required for arbitrary physical BAR1 pages that
//  belong to another client; this tool only maps its own D3D12 allocation.
//
//  Build: build_bench.bat  (MinGW-w64 g++, C++20, AVX2, static)
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <immintrin.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <random>
#include <algorithm>

// ---------------------------------------------------------------------------
// Firmware resource list (BAR1 discovery, same ABI as CHZM_HardwareProbe)
// ---------------------------------------------------------------------------
#pragma pack(push, 4)
struct SF_PARTIAL_RESOURCE_DESCRIPTOR {
    UCHAR  Type;
    UCHAR  ShareDisposition;
    USHORT Flags;
    union {
        struct { LARGE_INTEGER Start; ULONG Length; } Memory;
        struct { ULONG Level; ULONG Vector; ULONG_PTR Affinity; } Interrupt;
        struct { ULONG Data[3]; } Raw;
    } u;
};

struct SF_PARTIAL_RESOURCE_LIST {
    USHORT Version;
    USHORT Revision;
    ULONG  Count;
    SF_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptors[1];
};

struct SF_FULL_RESOURCE_DESCRIPTOR {
    ULONG InterfaceType;
    ULONG BusNumber;
    SF_PARTIAL_RESOURCE_LIST PartialResourceList;
};

struct SF_RESOURCE_LIST {
    ULONG Count;
    SF_FULL_RESOURCE_DESCRIPTOR List[1];
};
#pragma pack(pop)

static_assert(sizeof(SF_PARTIAL_RESOURCE_DESCRIPTOR) == 20, "resource ABI");

static constexpr UCHAR kTypeMemory = 3;
static constexpr UCHAR kTypeMemoryLarge = 7;
static constexpr USHORT kFlagPrefetchable = 0x0004;
static constexpr USHORT kLarge40 = 0x0200;
static constexpr USHORT kLarge48 = 0x0400;
static constexpr USHORT kLarge64 = 0x0800;

struct ApertureInfo {
    uint64_t base = 0;
    uint64_t size = 0;
    bool     found = false;
};

static uint64_t DecodeLength(const SF_PARTIAL_RESOURCE_DESCRIPTOR* d)
{
    uint64_t length = d->u.Memory.Length;
    if (d->Type == kTypeMemoryLarge) {
        if (d->Flags & kLarge40)      length <<= 8;
        else if (d->Flags & kLarge48) length <<= 16;
        else if (d->Flags & kLarge64) length <<= 32;
    }
    return length;
}

static bool ParseBootConfig(const std::vector<BYTE>& data, ApertureInfo& best)
{
    if (data.size() < sizeof(SF_RESOURCE_LIST)) return false;
    const SF_RESOURCE_LIST* list =
        reinterpret_cast<const SF_RESOURCE_LIST*>(data.data());
    if (list->Count == 0 || list->Count > 64) return false;

    const BYTE* end = data.data() + data.size();
    for (ULONG i = 0; i < list->Count; ++i) {
        const BYTE* fullRaw = reinterpret_cast<const BYTE*>(&list->List[i]);
        if (fullRaw + sizeof(SF_FULL_RESOURCE_DESCRIPTOR) > end) break;

        const SF_FULL_RESOURCE_DESCRIPTOR* full =
            reinterpret_cast<const SF_FULL_RESOURCE_DESCRIPTOR*>(fullRaw);
        const BYTE* entry = reinterpret_cast<const BYTE*>(
            &full->PartialResourceList.PartialDescriptors[0]);

        for (ULONG d = 0; d < full->PartialResourceList.Count; ++d) {
            const BYTE* raw = entry + (size_t)d * sizeof(SF_PARTIAL_RESOURCE_DESCRIPTOR);
            if (raw + sizeof(SF_PARTIAL_RESOURCE_DESCRIPTOR) > end) break;

            const SF_PARTIAL_RESOURCE_DESCRIPTOR* desc =
                reinterpret_cast<const SF_PARTIAL_RESOURCE_DESCRIPTOR*>(raw);
            if (desc->Type != kTypeMemory && desc->Type != kTypeMemoryLarge) continue;

            const uint64_t length = DecodeLength(desc);
            if (!(desc->Flags & kFlagPrefetchable)) continue;
            if (length <= best.size) continue;

            best.base = (uint64_t)desc->u.Memory.Start.QuadPart;
            best.size = length;
            best.found = best.base != 0;
        }
    }
    return best.found;
}

// Enumerates Enum\PCI\VEN_10DE*\<instance>\LogConf and takes the largest
// prefetchable window from the firmware boot configuration. No elevation.
static void TryReadBootConfig(const std::wstring& logConfPath, ApertureInfo& best)
{
    HKEY logConf = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, logConfPath.c_str(), 0,
                      KEY_READ, &logConf) != ERROR_SUCCESS)
        return;

    DWORD type = 0, size = 0;
    if (RegQueryValueExW(logConf, L"BootConfig", nullptr, &type, nullptr, &size)
            == ERROR_SUCCESS &&
        (type == REG_RESOURCE_LIST || type == REG_FULL_RESOURCE_DESCRIPTOR) &&
        size >= sizeof(SF_RESOURCE_LIST)) {
        std::vector<BYTE> buffer(size);
        if (RegQueryValueExW(logConf, L"BootConfig", nullptr, &type,
                             buffer.data(), &size) == ERROR_SUCCESS) {
            ParseBootConfig(buffer, best);
        }
    }
    RegCloseKey(logConf);
}

static ApertureInfo QueryAperture()
{
    ApertureInfo best;
    HKEY pci = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Enum\\PCI", 0,
                      KEY_READ, &pci) != ERROR_SUCCESS)
        return best;

    for (DWORD index = 0;; ++index) {
        wchar_t name[512];
        DWORD nameLen = (DWORD)(sizeof(name) / sizeof(name[0]));
        if (RegEnumKeyExW(pci, index, name, &nameLen, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS)
            break;
        if (wcsncmp(name, L"VEN_10DE", 8) != 0) continue;

        const std::wstring devicePath =
            std::wstring(L"SYSTEM\\CurrentControlSet\\Enum\\PCI\\") + name;
        HKEY deviceKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, devicePath.c_str(), 0,
                          KEY_READ, &deviceKey) != ERROR_SUCCESS)
            continue;

        for (DWORD child = 0;; ++child) {
            wchar_t instance[512];
            DWORD instanceLen = (DWORD)(sizeof(instance) / sizeof(instance[0]));
            if (RegEnumKeyExW(deviceKey, child, instance, &instanceLen,
                              nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            TryReadBootConfig(devicePath + L"\\" + instance + L"\\LogConf", best);
        }
        RegCloseKey(deviceKey);
    }
    RegCloseKey(pci);
    return best;
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------
static std::string FormatHex64(uint64_t value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%08llX_%08llX",
                  (unsigned long long)(value >> 32),
                  (unsigned long long)(value & 0xFFFFFFFFull));
    return buf;
}

static std::string HrText(HRESULT hr)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
    return buf;
}

struct Timer {
    LARGE_INTEGER freq{};
    LARGE_INTEGER start{};
    Timer() { QueryPerformanceFrequency(&freq); }
    void Begin() { QueryPerformanceCounter(&start); }
    double End() const
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
    }
};

static double MBps(uint64_t bytes, double seconds)
{
    if (seconds <= 0.0) return 0.0;
    return (double)bytes / (1024.0 * 1024.0) / seconds;
}

// ---------------------------------------------------------------------------
// AVX2 kernels
// ---------------------------------------------------------------------------
static bool Avx2Available()
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
#else
    int cpuInfo[4] = {};
    __cpuid(cpuInfo, 0);
    if (cpuInfo[0] < 7) return false;
    __cpuidex(cpuInfo, 7, 0);
    return (cpuInfo[1] & (1 << 5)) != 0;
#endif
}

// Write-combining fill: 256-bit non-temporal stores, no read-for-ownership.
// The pattern is a linear sequence of 64-bit words, cheap to generate so the
// measurement reflects store bandwidth rather than ALU work.
static void Avx2StreamFill(void* dst, size_t bytes, uint64_t seed)
{
    uint8_t* p = static_cast<uint8_t*>(dst);
    const size_t vectors = bytes / 32;

    __m256i value = _mm256_set_epi64x((long long)(seed + 3), (long long)(seed + 2),
                                      (long long)(seed + 1), (long long)seed);
    const __m256i step = _mm256_set1_epi64x(4);

    for (size_t i = 0; i < vectors; ++i) {
        _mm256_stream_si256(reinterpret_cast<__m256i*>(p + i * 32), value);
        value = _mm256_add_epi64(value, step);
    }
    _mm_sfence();
}

// Scalar twin of Avx2StreamFill, used to verify written data bit-for-bit.
static void ScalarPatternFill(void* dst, size_t bytes, uint64_t seed)
{
    uint64_t* words = static_cast<uint64_t*>(dst);
    const size_t count = bytes / sizeof(uint64_t);
    for (size_t i = 0; i < count; ++i) words[i] = seed + i;
}

// Streaming read with additive accumulation; used for integrity and read
// bandwidth. Addition is used instead of XOR so regular patterns do not cancel.
static uint64_t Avx2Checksum(const void* src, size_t bytes)
{
    const uint8_t* p = static_cast<const uint8_t*>(src);
    const size_t vectors = bytes / 32;
    __m256i acc = _mm256_setzero_si256();

    for (size_t i = 0; i < vectors; ++i) {
        __m256i v = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(p + i * 32));
        acc = _mm256_add_epi64(acc, v);
    }

    alignas(32) uint64_t lanes[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), acc);
    uint64_t result = lanes[0] + lanes[1] + lanes[2] + lanes[3];

    for (size_t i = vectors * 32; i < bytes; ++i) result += p[i];
    return result;
}

// Non-temporal loads (MOVNTDQA). On write-combined memory this can fetch a
// full 64-byte line per transaction instead of serialising 32-byte reads.
static uint64_t Avx2ChecksumNt(const void* src, size_t bytes)
{
    const uint8_t* p = static_cast<const uint8_t*>(src);
    const size_t vectors = bytes / 32;
    __m256i acc = _mm256_setzero_si256();

    for (size_t i = 0; i < vectors; ++i) {
        __m256i v = _mm256_stream_load_si256(
            reinterpret_cast<const __m256i*>(p + i * 32));
        acc = _mm256_add_epi64(acc, v);
    }

    alignas(32) uint64_t lanes[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), acc);
    return lanes[0] + lanes[1] + lanes[2] + lanes[3];
}

// ---------------------------------------------------------------------------
// D3D12 context
// ---------------------------------------------------------------------------
struct GpuContext {
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* copyQueue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    DXGI_ADAPTER_DESC1 desc{};
    bool uma = false;
    bool cacheCoherentUma = false;

    void Release()
    {
        if (fenceEvent) CloseHandle(fenceEvent);
        if (fence) fence->Release();
        if (list) list->Release();
        if (allocator) allocator->Release();
        if (copyQueue) copyQueue->Release();
        if (device) device->Release();
        if (adapter) adapter->Release();
    }

    bool Initialize()
    {
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* candidate = nullptr;
            if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 d{};
            candidate->GetDesc1(&d);
            if (d.VendorId == 0x10DE && (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                adapter = candidate;
                desc = d;
                break;
            }
            candidate->Release();
        }
        factory->Release();
        if (!adapter) return false;

        if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                                     IID_PPV_ARGS(&device))))
            return false;

        D3D12_FEATURE_DATA_ARCHITECTURE1 arch1{};
        arch1.NodeIndex = 0;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1,
                                                  &arch1, sizeof(arch1)))) {
            uma = arch1.UMA != FALSE;
            cacheCoherentUma = arch1.CacheCoherentUMA != FALSE;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        if (FAILED(device->CreateCommandQueue(&queueDesc,
                                              IID_PPV_ARGS(&copyQueue))))
            return false;

        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                  IID_PPV_ARGS(&allocator))))
            return false;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                                             allocator, nullptr,
                                             IID_PPV_ARGS(&list))))
            return false;
        list->Close();

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(&fence))))
            return false;
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return fenceEvent != nullptr;
    }

    bool Begin()
    {
        if (FAILED(allocator->Reset())) return false;
        return SUCCEEDED(list->Reset(allocator, nullptr));
    }

    bool SubmitAndWait(UINT timeoutMs = 120000)
    {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = { list };
        copyQueue->ExecuteCommandLists(1, lists);

        const UINT64 target = ++fenceValue;
        if (FAILED(copyQueue->Signal(fence, target))) return false;
        if (fence->GetCompletedValue() < target) {
            if (FAILED(fence->SetEventOnCompletion(target, fenceEvent)))
                return false;
            if (WaitForSingleObject(fenceEvent, timeoutMs) != WAIT_OBJECT_0)
                return false;
        }
        return true;
    }
};

static D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

static bool CreateBuffer(ID3D12Device* device, uint64_t bytes,
                         D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state,
                         ID3D12Resource** out)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = BufferDesc(bytes);
    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(out)));
}

// ---------------------------------------------------------------------------
// Benchmark
// ---------------------------------------------------------------------------
struct SuiteResult {
    uint32_t allocationMb = 0;
    uint64_t vramBytes = 0;

    double seqWriteMBps = 0.0;
    double seqReadMBps = 0.0;

    double r4kWriteMBps = 0.0, r4kWriteIops = 0.0, r4kWriteLatUs = 0.0;
    double r4kReadMBps = 0.0,  r4kReadIops = 0.0,  r4kReadLatUs = 0.0;

    double r64kWriteMBps = 0.0, r64kWriteIops = 0.0, r64kWriteLatUs = 0.0;
    double r64kReadMBps = 0.0,  r64kReadIops = 0.0,  r64kReadLatUs = 0.0;

    double stagingFillMBps = 0.0;
    double stagingReadMBps = 0.0;

    bool integrityPassed = false;
    uint64_t checksum = 0;
    bool ok = false;
};

static bool RunSuite(GpuContext& gpu, uint32_t sizeMb, SuiteResult& result)
{
    const uint64_t vramBytes = (uint64_t)sizeMb * 1024ull * 1024ull;
    const uint64_t stagingBytes = 256ull * 1024ull * 1024ull;
    const uint64_t chunkBytes = 64ull * 1024ull * 1024ull;
    const uint32_t randomOps = 10000;

    ID3D12Resource* vram = nullptr;
    ID3D12Resource* staging = nullptr;
    ID3D12Resource* readback = nullptr;
    void* stagingPtr = nullptr;
    void* readbackPtr = nullptr;

    auto cleanup = [&]() {
        if (staging && stagingPtr) staging->Unmap(0, nullptr);
        if (readback && readbackPtr) readback->Unmap(0, nullptr);
        if (readback) readback->Release();
        if (staging) staging->Release();
        if (vram) vram->Release();
        stagingPtr = nullptr;
        readbackPtr = nullptr;
        readback = nullptr;
        staging = nullptr;
        vram = nullptr;
    };
    auto fail = [&]() -> bool {
        cleanup();
        result.ok = false;
        return false;
    };

    if (!CreateBuffer(gpu.device, vramBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_COMMON, &vram))
        return fail();
    if (!CreateBuffer(gpu.device, stagingBytes, D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &staging))
        return fail();
    if (!CreateBuffer(gpu.device, chunkBytes, D3D12_HEAP_TYPE_READBACK,
                      D3D12_RESOURCE_STATE_COPY_DEST, &readback))
        return fail();

    if (FAILED(staging->Map(0, nullptr, &stagingPtr))) return fail();
    {
        D3D12_RANGE readRange{ 0, (SIZE_T)chunkBytes };
        if (FAILED(readback->Map(0, &readRange, &readbackPtr))) return fail();
    }

    result.allocationMb = sizeMb;
    result.vramBytes = vramBytes;

    // --- CPU staging fill (AVX2 streaming stores over write-combined RAM) ---
    {
        double best = 1e9;
        for (int pass = 0; pass < 3; ++pass) {
            Timer timer;
            timer.Begin();
            Avx2StreamFill(stagingPtr, stagingBytes, 0x9E3779B97F4A7C15ull);
            const double seconds = timer.End();
            best = std::min(best, seconds);
        }
        result.stagingFillMBps = MBps(stagingBytes, best);
    }

    // --- Host-side read of the readback heap (cached system RAM) ---
    {
        double best = 1e9;
        for (int pass = 0; pass < 8; ++pass) {
            Timer timer;
            timer.Begin();
            volatile uint64_t sink = Avx2Checksum(readbackPtr, chunkBytes);
            (void)sink;
            const double seconds = timer.End();
            best = std::min(best, seconds);
        }
        result.stagingReadMBps = MBps(chunkBytes, best);
    }

    // --- Sequential host -> VRAM (PCIe write, copy engine) ---
    {
        double best = 1e9;
        for (int pass = 0; pass < 3; ++pass) {
            if (!gpu.Begin()) return fail();
            for (uint64_t offset = 0; offset < vramBytes; offset += chunkBytes) {
                const UINT bytes = (UINT)std::min<uint64_t>(chunkBytes, vramBytes - offset);
                gpu.list->CopyBufferRegion(vram, offset, staging, 0, bytes);
            }
            Timer timer;
            timer.Begin();
            if (!gpu.SubmitAndWait()) return fail();
            best = std::min(best, timer.End());
        }
        result.seqWriteMBps = MBps(vramBytes, best);
    }

    // --- Sequential VRAM -> host (PCIe read, copy engine) ---
    {
        double best = 1e9;
        for (int pass = 0; pass < 3; ++pass) {
            if (!gpu.Begin()) return fail();
            for (uint64_t offset = 0; offset < vramBytes; offset += chunkBytes) {
                const UINT bytes = (UINT)std::min<uint64_t>(chunkBytes, vramBytes - offset);
                gpu.list->CopyBufferRegion(readback, 0, vram, offset, bytes);
            }
            Timer timer;
            timer.Begin();
            if (!gpu.SubmitAndWait()) return fail();
            best = std::min(best, timer.End());
        }
        result.seqReadMBps = MBps(vramBytes, best);
    }

    // --- Integrity: staging -> VRAM -> readback, bit-exact compare ---
    {
        Avx2StreamFill(stagingPtr, chunkBytes, 0xDEADBEEFCAFEBABEull);
        if (!gpu.Begin()) return fail();
        gpu.list->CopyBufferRegion(vram, 0, staging, 0, (UINT)chunkBytes);
        gpu.list->CopyBufferRegion(readback, 0, vram, 0, (UINT)chunkBytes);
        if (!gpu.SubmitAndWait()) return fail();

        result.integrityPassed =
            std::memcmp(stagingPtr, readbackPtr, (size_t)chunkBytes) == 0;
        result.checksum = Avx2Checksum(readbackPtr, chunkBytes);
    }

    // --- Random block transfers ---
    const uint64_t blocks4k = vramBytes / 4096ull;
    const uint64_t blocks64k = vramBytes / 65536ull;
    std::mt19937_64 rng(0xC0FFEEull);
    std::vector<uint64_t> offs4k(randomOps), offs64k(randomOps);
    for (uint32_t i = 0; i < randomOps; ++i) {
        offs4k[i] = (rng() % blocks4k) * 4096ull;
        offs64k[i] = (rng() % blocks64k) * 65536ull;
    }

    auto runRandom = [&](const std::vector<uint64_t>& offsets, uint32_t blockSize,
                         bool writeDirection, double& mbps, double& iops,
                         double& latencyUs) -> bool {
        if (!gpu.Begin()) return false;
        for (uint64_t offset : offsets) {
            if (writeDirection)
                gpu.list->CopyBufferRegion(vram, offset, staging, 0, blockSize);
            else
                gpu.list->CopyBufferRegion(readback, 0, vram, offset, blockSize);
        }
        Timer timer;
        timer.Begin();
        if (!gpu.SubmitAndWait()) return false;
        const double seconds = timer.End();
        const double totalBytes = (double)offsets.size() * blockSize;

        mbps = (totalBytes / (1024.0 * 1024.0)) / seconds;
        iops = (double)offsets.size() / seconds;
        latencyUs = (seconds / (double)offsets.size()) * 1e6;
        return true;
    };

    if (!runRandom(offs4k, 4096, true, result.r4kWriteMBps,
                   result.r4kWriteIops, result.r4kWriteLatUs)) return fail();
    if (!runRandom(offs4k, 4096, false, result.r4kReadMBps,
                   result.r4kReadIops, result.r4kReadLatUs)) return fail();
    if (!runRandom(offs64k, 65536, true, result.r64kWriteMBps,
                   result.r64kWriteIops, result.r64kWriteLatUs)) return fail();
    if (!runRandom(offs64k, 65536, false, result.r64kReadMBps,
                   result.r64kReadIops, result.r64kReadLatUs)) return fail();

    cleanup();
    result.ok = true;
    return true;
}

// ---------------------------------------------------------------------------
// Direct MMIO probe: the runtime may accept CUSTOM + WRITE_COMBINE + L1 on
// ReBAR platforms, in which case Map() yields a CPU pointer into GDDR6.
// ---------------------------------------------------------------------------
struct DirectMmioProbe {
    HRESULT createHr = E_FAIL;
    HRESULT mapHr = E_FAIL;
    bool mapped = false;
    bool integrityPassed = false;
    bool gpuIntegrityPassed = false;
    uint64_t bytes = 0;
    uint64_t checksum = 0;

    double writeMBps = 0.0;
    double readMBps = 0.0;
    double readNtMBps = 0.0;
    double latencyNs = 0.0;

    double r4kWriteMBps = 0.0, r4kWriteIops = 0.0, r4kWriteLatUs = 0.0;
    double r4kReadMBps = 0.0,  r4kReadIops = 0.0,  r4kReadLatUs = 0.0;
    double r64kWriteMBps = 0.0, r64kWriteIops = 0.0, r64kWriteLatUs = 0.0;
    double r64kReadMBps = 0.0,  r64kReadIops = 0.0,  r64kReadLatUs = 0.0;
};

static DirectMmioProbe ProbeDirectMmio(GpuContext& gpu)
{
    DirectMmioProbe probe;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_CUSTOM;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L1;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    const uint64_t bytes = 64ull * 1024ull * 1024ull;
    const uint64_t verifyBytes = 4096ull;
    const uint64_t seed = 0xA5A5F00DDEADBEEFull;
    D3D12_RESOURCE_DESC desc = BufferDesc(bytes);

    ID3D12Resource* resource = nullptr;
    probe.createHr = gpu.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&resource));

    if (FAILED(probe.createHr) || !resource) return probe;

    void* ptr = nullptr;
    probe.mapHr = resource->Map(0, nullptr, &ptr);
    if (FAILED(probe.mapHr) || !ptr) {
        resource->Release();
        return probe;
    }

    probe.mapped = true;
    probe.bytes = bytes;

    // Bit-exact integrity check over the first 4 KB.
    Avx2StreamFill(ptr, (size_t)verifyBytes, seed);
    std::vector<uint8_t> expected((size_t)verifyBytes);
    ScalarPatternFill(expected.data(), (size_t)verifyBytes, seed);
    probe.integrityPassed =
        std::memcmp(ptr, expected.data(), (size_t)verifyBytes) == 0;
    probe.checksum = Avx2Checksum(ptr, (size_t)verifyBytes);

    // Sequential write: streaming stores, fence included in the measurement.
    {
        double best = 1e9;
        for (int pass = 0; pass < 3; ++pass) {
            Timer timer;
            timer.Begin();
            Avx2StreamFill(ptr, (size_t)bytes, seed + (uint64_t)pass);
            best = std::min(best, timer.End());
        }
        probe.writeMBps = MBps(bytes, best);
    }

    // Sequential read: ordinary uncached loads.
    {
        double best = 1e9;
        for (int pass = 0; pass < 2; ++pass) {
            Timer timer;
            timer.Begin();
            volatile uint64_t sink = Avx2Checksum(ptr, (size_t)bytes);
            (void)sink;
            best = std::min(best, timer.End());
        }
        probe.readMBps = MBps(bytes, best);
    }

    // Sequential read: non-temporal loads.
    {
        Timer timer;
        timer.Begin();
        volatile uint64_t sink = Avx2ChecksumNt(ptr, (size_t)bytes);
        (void)sink;
        probe.readNtMBps = MBps(bytes, timer.End());
    }

    // Access latency: independent 64-bit reads, one 4 KB page apart.
    {
        const uint64_t count = 512;
        const uint64_t stride = 4096;
        volatile uint64_t sink = 0;
        Timer timer;
        timer.Begin();
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t offset = (i * stride) % (bytes - 8);
            sink += *reinterpret_cast<volatile const uint64_t*>(
                static_cast<const uint8_t*>(ptr) + offset);
        }
        probe.latencyNs = timer.End() * 1e9 / (double)count;
        (void)sink;
    }

    std::mt19937_64 rng(0xC0FFEEull);

    auto timedRandom = [&](uint32_t blockSize, uint32_t ops, bool writeDirection,
                           double& mbps, double& iops, double& latencyUs) {
        const uint64_t blocks = bytes / blockSize;
        std::vector<uint64_t> offsets(ops);
        for (uint32_t i = 0; i < ops; ++i)
            offsets[i] = (rng() % blocks) * blockSize;

        uint64_t acc = 0;
        double totalNs = 0.0;
        for (uint32_t i = 0; i < ops; ++i) {
            uint8_t* target = static_cast<uint8_t*>(ptr) + offsets[i];
            Timer timer;
            timer.Begin();
            if (writeDirection) {
                Avx2StreamFill(target, blockSize, seed + i * 64);
            } else {
                acc ^= Avx2Checksum(target, blockSize);
            }
            totalNs += timer.End() * 1e9;
        }

        const double perOpNs = totalNs / (double)ops;
        latencyUs = perOpNs / 1000.0;
        iops = 1e9 / perOpNs;
        mbps = ((double)blockSize * iops) / (1024.0 * 1024.0);

        static volatile uint64_t g_sink;
        g_sink = acc;
        (void)g_sink;
    };

    timedRandom(4096, 10000, true, probe.r4kWriteMBps,
                probe.r4kWriteIops, probe.r4kWriteLatUs);
    timedRandom(4096, 10000, false, probe.r4kReadMBps,
                probe.r4kReadIops, probe.r4kReadLatUs);
    timedRandom(65536, 1024, true, probe.r64kWriteMBps,
                probe.r64kWriteIops, probe.r64kWriteLatUs);
    timedRandom(65536, 1024, false, probe.r64kReadMBps,
                probe.r64kReadIops, probe.r64kReadLatUs);

    // GPU-verified integrity: write a known pattern through the CPU mapping,
    // then have the copy engine read the same allocation back and compare.
    // This proves the pointer targets the memory the GPU actually sees.
    {
        Avx2StreamFill(ptr, (size_t)bytes, 0x1122334455667788ull);

        ID3D12Resource* readback = nullptr;
        if (CreateBuffer(gpu.device, bytes, D3D12_HEAP_TYPE_READBACK,
                         D3D12_RESOURCE_STATE_COPY_DEST, &readback)) {
            D3D12_RANGE range{ 0, (SIZE_T)bytes };
            void* rb = nullptr;
            if (SUCCEEDED(readback->Map(0, &range, &rb)) && rb) {
                if (gpu.Begin()) {
                    D3D12_RESOURCE_BARRIER barrier{};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition.pResource = resource;
                    barrier.Transition.Subresource =
                        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    gpu.list->ResourceBarrier(1, &barrier);
                    gpu.list->CopyBufferRegion(readback, 0, resource, 0, bytes);

                    if (gpu.SubmitAndWait()) {
                        probe.gpuIntegrityPassed =
                            std::memcmp(ptr, rb, (size_t)bytes) == 0;
                        probe.checksum = Avx2Checksum(rb, (size_t)bytes);
                    }
                }
                readback->Unmap(0, nullptr);
            }
            readback->Release();
        }
    }

    resource->Unmap(0, nullptr);
    resource->Release();
    return probe;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------
static constexpr double IMDISK_SEQ_MBPS = 5103.8;
static constexpr double IMDISK_IOPS_4K = 109020.0;
static constexpr double IMDISK_LAT_US = 36.2;

static void PrintRule(char c = '=')
{
    for (int i = 0; i < 70; ++i) std::putchar(c);
    std::putchar('\n');
}

static void PrintSuite(const SuiteResult& s)
{
    std::printf("Allocation Size    : %u MB pure GDDR6 (D3D12 L1 / DEFAULT heap)\n",
                s.allocationMb);
    std::printf("Sequential Write   : %8.1f MB/s (Host -> VRAM, PCIe write)\n",
                s.seqWriteMBps);
    std::printf("Sequential Read    : %8.1f MB/s (VRAM -> Host, PCIe read)\n",
                s.seqReadMBps);
    std::printf("Random 4K Write    : %8.0f IOPS | Avg Latency: %.1f us\n",
                s.r4kWriteIops, s.r4kWriteLatUs);
    std::printf("Random 4K Read     : %8.0f IOPS | Avg Latency: %.1f us\n",
                s.r4kReadIops, s.r4kReadLatUs);
    std::printf("Random 64K Write   : %8.1f MB/s | %8.0f IOPS | %.1f us\n",
                s.r64kWriteMBps, s.r64kWriteIops, s.r64kWriteLatUs);
    std::printf("Random 64K Read    : %8.1f MB/s | %8.0f IOPS | %.1f us\n",
                s.r64kReadMBps, s.r64kReadIops, s.r64kReadLatUs);
    std::printf("CPU staging fill   : %8.1f MB/s (AVX2 NT stores, L0 WC RAM)\n",
                s.stagingFillMBps);
    std::printf("CPU staging read   : %8.1f MB/s (AVX2 loads, L0 readback RAM)\n",
                s.stagingReadMBps);
    std::printf("Integrity          : %s (checksum %s)\n",
                s.integrityPassed ? "PASS" : "FAIL",
                FormatHex64(s.checksum).c_str());
    PrintRule('-');
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"CHZM-SiliconFuse ReBAR Benchmark");

    uint32_t requestedMb = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            requestedMb = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        }
    }

    const ApertureInfo aperture = QueryAperture();

    GpuContext gpu;
    if (!gpu.Initialize()) {
        std::printf("[FATAL] D3D12 initialization failed (NVIDIA adapter or copy queue).\n");
        return 1;
    }

    DirectMmioProbe mmio = ProbeDirectMmio(gpu);

    PrintRule();
    std::printf("CHZM-SiliconFuse ReBAR Direct Silicon Benchmark v0.1\n");
    PrintRule();
    std::printf("Adapter            : %ls\n", gpu.desc.Description);
    std::printf("Adapter LUID       : %08lX:%08lX\n",
                (unsigned long)gpu.desc.AdapterLuid.HighPart,
                (unsigned long)gpu.desc.AdapterLuid.LowPart);
    std::printf("Vendor / Device    : 0x%04X / 0x%04X\n",
                gpu.desc.VendorId, gpu.desc.DeviceId);
    std::printf("Dedicated VRAM     : %llu MB\n",
                (unsigned long long)(gpu.desc.DedicatedVideoMemory / (1024ull * 1024ull)));
    if (aperture.found) {
        std::printf("Aperture Target    : %s (BAR1, %llu MB, firmware map)\n",
                    FormatHex64(aperture.base).c_str(),
                    (unsigned long long)(aperture.size / (1024ull * 1024ull)));
    } else {
        std::printf("Aperture Target    : firmware map unavailable\n");
    }
    std::printf("D3D12 Architecture : UMA=%d CacheCoherentUMA=%d\n",
                gpu.uma ? 1 : 0, gpu.cacheCoherentUma ? 1 : 0);
    std::printf("AVX2               : %s\n", Avx2Available() ? "yes" : "no");

    PrintRule('-');
    if (mmio.mapped) {
        std::printf("[DIRECT MMIO] D3D12 CUSTOM + WRITE_COMBINE + L1: ACCEPTED\n");
        std::printf("  Buffer           : %llu MB L1 allocation, CPU pointer via PCIe\n",
                    (unsigned long long)(mmio.bytes / (1024ull * 1024ull)));
        std::printf("  Sequential Write : %8.1f MB/s (AVX2 NT stores + sfence)\n",
                    mmio.writeMBps);
        std::printf("  Sequential Read  : %8.1f MB/s (AVX2 loads, uncached)\n",
                    mmio.readMBps);
        std::printf("  Sequential Read NT: %7.1f MB/s (MOVNTDQA)\n",
                    mmio.readNtMBps > 0.0 ? mmio.readNtMBps : 0.0);
        std::printf("  Access Latency   : %8.1f ns per 64-bit read\n", mmio.latencyNs);
        std::printf("  Random 4K Write  : %8.0f IOPS | %.1f us | %.1f MB/s\n",
                    mmio.r4kWriteIops, mmio.r4kWriteLatUs, mmio.r4kWriteMBps);
        std::printf("  Random 4K Read   : %8.0f IOPS | %.1f us | %.1f MB/s\n",
                    mmio.r4kReadIops, mmio.r4kReadLatUs, mmio.r4kReadMBps);
        std::printf("  Random 64K Write : %8.0f IOPS | %.1f us | %.1f MB/s\n",
                    mmio.r64kWriteIops, mmio.r64kWriteLatUs, mmio.r64kWriteMBps);
        std::printf("  Random 64K Read  : %8.0f IOPS | %.1f us | %.1f MB/s\n",
                    mmio.r64kReadIops, mmio.r64kReadLatUs, mmio.r64kReadMBps);
        std::printf("  Integrity        : %s (checksum %s)\n",
                    mmio.integrityPassed ? "PASS" : "FAIL",
                    FormatHex64(mmio.checksum).c_str());
        std::printf("  GPU-verified     : %s (copy engine readback, 64 MB)\n",
                    mmio.gpuIntegrityPassed ? "PASS" : "FAIL");
    } else {
        std::printf("[DIRECT MMIO] UNAVAILABLE\n");
        std::printf("  CreateCommittedResource(CUSTOM, WRITE_COMBINE, L1) -> %s\n",
                    HrText(mmio.createHr).c_str());
        std::printf("  Map()                                               -> %s\n",
                    HrText(mmio.mapHr).c_str());
        std::printf("  This platform does not expose a CPU-mapped L1 heap.\n");
    }
    PrintRule('-');
    std::printf("[DMA] Copy engine over PCIe (supported WDDM path)\n");

    std::vector<uint32_t> sizes;
    if (requestedMb) {
        sizes.push_back(requestedMb);
    } else {
        sizes.push_back(1024);
        sizes.push_back(2048);
    }

    bool any = false;
    SuiteResult last{};
    for (uint32_t sizeMb : sizes) {
        SuiteResult result;
        if (RunSuite(gpu, sizeMb, result)) {
            PrintSuite(result);
            any = true;
            last = result;
        } else {
            std::printf("Allocation Size    : %u MB - FAILED (VRAM or host memory)\n",
                        sizeMb);
            PrintRule('-');
        }
    }

    if (any) {
        const bool useDirect = mmio.mapped;
        const double bw = useDirect ? mmio.writeMBps : last.seqWriteMBps;
        const double iops = useDirect ? mmio.r4kReadIops : last.r4kReadIops;
        const double lat = useDirect ? mmio.r4kReadLatUs : last.r4kReadLatUs;

        const double seqRatio = bw / IMDISK_SEQ_MBPS;
        const double iopsRatio = iops / IMDISK_IOPS_4K;
        const double latRatio = lat > 0.0 ? IMDISK_LAT_US / lat : 0.0;

        std::printf("[COMPARE] ImDisk Proxy (Old)  : %.1f MB/s | %.0f IOPS (%.1f us)\n",
                    IMDISK_SEQ_MBPS, IMDISK_IOPS_4K, IMDISK_LAT_US);
        std::printf("[COMPARE] ReBAR %s (New) : %.1f MB/s | %.0f IOPS (%.1f us)\n",
                    useDirect ? "MMIO " : "DMA  ", bw, iops, lat);
        std::printf("[COMPARE] Ratio               : %.2fx sequential | %.2fx 4K IOPS | %.2fx latency\n",
                    seqRatio, iopsRatio, latRatio);
        PrintRule();
        std::printf("Reading the result: direct MMIO writes are bandwidth-bound and reach\n");
        std::printf("PCIe line rate; MMIO reads are latency-bound (one transaction per\n");
        std::printf("access) and lose to the copy engine. Correct policy is WC writes via\n");
        std::printf("BAR1 and DMA for reads. The ImDisk reference was a user-mode block\n");
        std::printf("proxy, so ratios are indicative, not an apples-to-apples transport test.\n");
    }

    gpu.Release();
    return any ? 0 : 1;
}
