// ============================================================================
//  CHZM SiliconFuse - Phase 2.1 Chunked Lease Broker
//  File: tools/LeaseBroker/main.cpp
//
//  Scales the Tier-1 pool to 4 GB through independent 1 GB slices instead of a
//  single monolithic allocation, and evicts only the slices needed to cover
//  the measured deficit. A 4 GB request was rejected by WDDM on the reference
//  platform; four 1 GB slices allocate where one 4 GB block did not.
//
//  Properties:
//    * each slice is its own D3D12 L1 custom / WRITE_COMBINE resource;
//    * dirty data is tracked per slice, so eviction copies only live bytes;
//    * eviction is incremental: deficit / slice_size slices are released, the
//      rest stay resident in GDDR6;
//    * restore brings back exactly the slices that were evicted.
//
//  Build: build_broker.bat (MinGW-w64 g++, C++20, AVX2, static)
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
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <algorithm>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
struct Timer {
    LARGE_INTEGER freq{};
    LARGE_INTEGER start{};
    Timer() { QueryPerformanceFrequency(&freq); }
    void Begin() { QueryPerformanceCounter(&start); }
    double Ms() const
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
    }
};

static std::string WallClock()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

static void Avx2Fill(void* dst, size_t bytes, uint64_t seed)
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

static D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

static bool CreateCustomL1(ID3D12Device* device, uint64_t bytes,
                           ID3D12Resource** out)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_CUSTOM;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L1;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = BufferDesc(bytes);
    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(out)));
}

static bool CreateHeapBuffer(ID3D12Device* device, uint64_t bytes,
                             D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                             ID3D12Resource** out)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = BufferDesc(bytes);
    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(out)));
}

struct BudgetInfo {
    uint64_t budget = 0;
    uint64_t currentUsage = 0;
};

static bool QueryBudget(IDXGIAdapter3* adapter, DXGI_MEMORY_SEGMENT_GROUP group,
                        BudgetInfo& info)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO raw{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, group, &raw)))
        return false;
    info.budget = raw.Budget;
    info.currentUsage = raw.CurrentUsage;
    return true;
}

// NVIDIA driver telemetry, loaded dynamically; used only as a secondary
// pressure signal because per-process WDDM budgets are advisory.
struct NvmlMemory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

struct NvmlUtilization {
    unsigned int gpu;
    unsigned int memory;
};

typedef int (*PfnNvmlInit)(void);
typedef int (*PfnNvmlHandle)(unsigned int, void**);
typedef int (*PfnNvmlMemory)(void*, NvmlMemory*);
typedef int (*PfnNvmlUtil)(void*, NvmlUtilization*);

// ---------------------------------------------------------------------------
// Pool model
// ---------------------------------------------------------------------------
struct PoolSlice {
    ID3D12Resource* resource = nullptr;
    void* cpuPtr = nullptr;
    uint64_t sizeBytes = 0;
    uint64_t dirtyBytes = 0;
    uint64_t spillOffset = 0;
    bool inVram = false;
    bool mapped = false;
};

enum class LeaseState { Active, Evicting, Restoring, AllEvicted };

struct Broker {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;

    IDXGIAdapter3* adapter3 = nullptr;
    HANDLE budgetEvent = nullptr;
    DWORD cookie = 0;

    HMODULE nvmlModule = nullptr;
    PfnNvmlMemory nvmlGetMemory = nullptr;
    PfnNvmlUtil nvmlGetUtil = nullptr;
    void* nvmlDevice = nullptr;

    std::vector<PoolSlice> slices;
    uint64_t sliceBytes = 0;
    uint64_t totalBytes = 0;
    uint64_t dirtyTotal = 0;
    uint64_t dirtyPerSlice = 0;
    std::vector<uint8_t> spill;

    ID3D12Resource* readStage = nullptr;
    void* readPtr = nullptr;
    ID3D12Resource* writeStage = nullptr;
    void* writePtr = nullptr;
    uint64_t stageBytes = 0;

    LeaseState state = LeaseState::Active;
    std::atomic<bool> stop{ false };
    uint64_t initialBudget = 0;
    int evictionRounds = 0;
    int slicesEvicted = 0;
    int slicesRestored = 0;

    ~Broker() { ReleaseAll(); }

    // -- lifecycle ----------------------------------------------------------
    bool Init()
    {
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            std::printf("[FAIL] CreateDXGIFactory1\n");
            return false;
        }

        IDXGIAdapter1* adapter1 = nullptr;
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* candidate = nullptr;
            if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc{};
            candidate->GetDesc1(&desc);
            if (desc.VendorId == 0x10DE &&
                (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                adapter1 = candidate;
                break;
            }
            candidate->Release();
        }
        factory->Release();
        if (!adapter1) {
            std::printf("[FAIL] NVIDIA adapter not found\n");
            return false;
        }
        if (FAILED(adapter1->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
            std::printf("[FAIL] IDXGIAdapter3 unavailable\n");
            adapter1->Release();
            return false;
        }
        if (FAILED(D3D12CreateDevice(adapter1, D3D_FEATURE_LEVEL_11_0,
                                     IID_PPV_ARGS(&device)))) {
            std::printf("[FAIL] D3D12CreateDevice\n");
            adapter1->Release();
            return false;
        }
        adapter1->Release();

        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        if (FAILED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                  IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                                             allocator, nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(&fence)))) {
            std::printf("[FAIL] GPU copy context\n");
            return false;
        }
        list->Close();

        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        budgetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent || !budgetEvent) {
            std::printf("[FAIL] CreateEvent\n");
            return false;
        }

        nvmlModule = LoadLibraryW(L"nvml.dll");
        if (nvmlModule) {
            auto init = reinterpret_cast<PfnNvmlInit>(
                reinterpret_cast<void*>(GetProcAddress(nvmlModule, "nvmlInit_v2")));
            auto getHandle = reinterpret_cast<PfnNvmlHandle>(
                reinterpret_cast<void*>(GetProcAddress(nvmlModule, "nvmlDeviceGetHandleByIndex_v2")));
            nvmlGetMemory = reinterpret_cast<PfnNvmlMemory>(
                reinterpret_cast<void*>(GetProcAddress(nvmlModule, "nvmlDeviceGetMemoryInfo")));
            nvmlGetUtil = reinterpret_cast<PfnNvmlUtil>(
                reinterpret_cast<void*>(GetProcAddress(nvmlModule, "nvmlDeviceGetUtilizationRates")));
            if (init && getHandle && nvmlGetMemory &&
                init() == 0 && getHandle(0, &nvmlDevice) == 0) {
                std::printf("NVML               : available (secondary pressure signal)\n");
            } else {
                nvmlGetMemory = nullptr;
                nvmlDevice = nullptr;
                std::printf("NVML               : unavailable\n");
            }
        } else {
            std::printf("NVML               : unavailable\n");
        }
        return true;
    }

    bool QueryNvmlFree(uint64_t& freeBytes, uint64_t& totalBytes) const
    {
        if (!nvmlGetMemory || !nvmlDevice) return false;
        NvmlMemory memory{};
        if (nvmlGetMemory(nvmlDevice, &memory) != 0) return false;
        freeBytes = memory.free;
        totalBytes = memory.total;
        return true;
    }

    bool QueryNvmlUtil(unsigned& gpuPercent) const
    {
        if (!nvmlGetUtil || !nvmlDevice) return false;
        NvmlUtilization util{};
        if (nvmlGetUtil(nvmlDevice, &util) != 0) return false;
        gpuPercent = util.gpu;
        return true;
    }

    // -- GPU plumbing -------------------------------------------------------
    bool BeginList()
    {
        if (FAILED(allocator->Reset())) return false;
        return SUCCEEDED(list->Reset(allocator, nullptr));
    }

    bool SubmitAndWait()
    {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        const UINT64 target = ++fenceValue;
        if (FAILED(queue->Signal(fence, target))) return false;
        if (fence->GetCompletedValue() < target) {
            if (FAILED(fence->SetEventOnCompletion(target, fenceEvent)))
                return false;
            if (WaitForSingleObject(fenceEvent, 60000) != WAIT_OBJECT_0)
                return false;
        }
        return true;
    }

    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list->ResourceBarrier(1, &barrier);
    }

    // -- pool ---------------------------------------------------------------
    bool AddSlice(uint64_t bytes)
    {
        ID3D12Resource* resource = nullptr;
        if (!CreateCustomL1(device, bytes, &resource)) return false;

        void* mapped = nullptr;
        if (FAILED(resource->Map(0, nullptr, &mapped))) {
            resource->Release();
            return false;
        }

        PoolSlice slice;
        slice.resource = resource;
        slice.cpuPtr = mapped;
        slice.sizeBytes = bytes;
        slice.mapped = true;
        slice.inVram = true;
        slices.push_back(slice);
        totalBytes += bytes;
        return true;
    }

    bool AllocatePool(uint64_t targetBytes, uint64_t preferredSlice)
    {
        sliceBytes = preferredSlice;
        while (totalBytes + sliceBytes <= targetBytes) {
            if (!AddSlice(sliceBytes)) {
                std::printf("[WARN] slice %llu MB refused by WDDM, stopping at %llu MB\n",
                            (unsigned long long)(sliceBytes / (1024ull * 1024ull)),
                            (unsigned long long)(totalBytes / (1024ull * 1024ull)));
                break;
            }
        }
        // If 1 GB slices were refused, retry the whole pool with 2 GB slices.
        if (totalBytes < 2048ull * 1024ull * 1024ull &&
            sliceBytes < 2048ull * 1024ull * 1024ull) {
            for (auto& s : slices) {
                if (s.mapped) s.resource->Unmap(0, nullptr);
                s.resource->Release();
            }
            slices.clear();
            totalBytes = 0;
            sliceBytes = 2048ull * 1024ull * 1024ull;
            while (totalBytes + sliceBytes <= targetBytes) {
                if (!AddSlice(sliceBytes)) break;
            }
        }
        return !slices.empty();
    }

    // -- staging ------------------------------------------------------------
    bool PrepareStaging(uint64_t bytes)
    {
        stageBytes = bytes;
        if (!CreateHeapBuffer(device, bytes, D3D12_HEAP_TYPE_READBACK,
                              D3D12_RESOURCE_STATE_COPY_DEST, &readStage))
            return false;
        if (FAILED(readStage->Map(0, nullptr, &readPtr))) {
            FreeStaging();
            return false;
        }
        if (!CreateHeapBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD,
                              D3D12_RESOURCE_STATE_GENERIC_READ, &writeStage)) {
            FreeStaging();
            return false;
        }
        if (FAILED(writeStage->Map(0, nullptr, &writePtr))) {
            FreeStaging();
            return false;
        }
        return true;
    }

    void FreeStaging()
    {
        if (readStage) {
            if (readPtr) { readStage->Unmap(0, nullptr); readPtr = nullptr; }
            readStage->Release();
            readStage = nullptr;
        }
        if (writeStage) {
            if (writePtr) { writeStage->Unmap(0, nullptr); writePtr = nullptr; }
            writeStage->Release();
            writeStage = nullptr;
        }
        stageBytes = 0;
    }

    // -- slicing operations -------------------------------------------------
    void FillSlices(uint64_t dirtyPerSliceBytes)
    {
        dirtyPerSlice = dirtyPerSliceBytes;
        spill.clear();
        spill.reserve(slices.size() * (size_t)dirtyPerSliceBytes);
        Timer timer;
        timer.Begin();
        uint64_t totalFilled = 0;

        for (size_t i = 0; i < slices.size(); ++i) {
            PoolSlice& slice = slices[i];
            const uint64_t bytes = std::min<uint64_t>(dirtyPerSliceBytes,
                                                      slice.sizeBytes);
            slice.dirtyBytes = bytes;
            slice.spillOffset = spill.size();
            spill.resize(spill.size() + bytes);
            Avx2Fill(slice.cpuPtr, (size_t)bytes, 0x5EEDF00Dull + i * 0x1000000ull);
            totalFilled += bytes;
        }

        const double seconds = timer.Ms() / 1000.0;
        dirtyTotal = totalFilled;
        const double gbps = seconds > 0.0
            ? ((double)totalFilled / (1024.0 * 1024.0 * 1024.0)) / seconds
            : 0.0;
        std::printf("[FILL] %llu slices, %llu MB dirty, %.2f GB/s aggregate "
                    "(AVX2 NT stores)\n",
                    (unsigned long long)slices.size(),
                    (unsigned long long)(totalFilled / (1024ull * 1024ull)),
                    gbps);

        // The CPU mapping is not needed while the pool is idle. Releasing it
        // here removes the 1 GB unmap cost from the eviction critical path.
        for (auto& slice : slices) {
            if (slice.mapped) {
                slice.resource->Unmap(0, nullptr);
                slice.mapped = false;
                slice.cpuPtr = nullptr;
            }
        }
    }

    bool EvictSlice(PoolSlice& slice, int index, double& totalMs,
                    uint64_t& freedBytes)
    {
        Timer timer;
        timer.Begin();

        if (slice.mapped) {
            slice.resource->Unmap(0, nullptr);
            slice.mapped = false;
            slice.cpuPtr = nullptr;
        }

        double copyMs = 0.0;
        if (slice.dirtyBytes) {
            if (!BeginList()) return false;
            Transition(slice.resource, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            uint64_t copied = 0;
            Timer copyTimer;
            copyTimer.Begin();
            while (copied < slice.dirtyBytes) {
                const uint64_t bytes = std::min<uint64_t>(stageBytes,
                                                          slice.dirtyBytes - copied);
                list->CopyBufferRegion(readStage, 0, slice.resource, copied, bytes);
                if (!SubmitAndWait()) return false;
                std::memcpy(spill.data() + slice.spillOffset + copied,
                            readPtr, (size_t)bytes);
                copied += bytes;
                if (copied < slice.dirtyBytes && !BeginList()) return false;
            }
            copyMs = copyTimer.Ms();
        }

        ID3D12Pageable* pageable = slice.resource;
        Timer evictTimer;
        evictTimer.Begin();
        device->Evict(1, &pageable);
        const double evictMs = evictTimer.Ms();

        Timer releaseTimer;
        releaseTimer.Begin();
        slice.resource->Release();
        slice.resource = nullptr;
        const double releaseMs = releaseTimer.Ms();

        slice.inVram = false;
        freedBytes += slice.sizeBytes;
        totalMs = timer.Ms();
        slicesEvicted++;

        std::printf("[EVICT] slice=%d copy=%.2f ms evict=%.2f ms release=%.2f ms "
                    "total=%.2f ms freed=%llu MB remaining=%llu MB\n",
                    index, copyMs, evictMs, releaseMs, totalMs,
                    (unsigned long long)(slice.sizeBytes / (1024ull * 1024ull)),
                    (unsigned long long)(ResidentBytes() / (1024ull * 1024ull)));
        std::fflush(stdout);
        return true;
    }

    bool RestoreSlice(PoolSlice& slice, int index, double& totalMs)
    {
        Timer timer;
        timer.Begin();

        if (!AddSliceAt(slice, slice.sizeBytes)) return false;

        if (slice.dirtyBytes) {
            if (!BeginList()) return false;
            Transition(slice.resource, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            uint64_t copied = 0;
            while (copied < slice.dirtyBytes) {
                const uint64_t bytes = std::min<uint64_t>(stageBytes,
                                                          slice.dirtyBytes - copied);
                std::memcpy(writePtr, spill.data() + slice.spillOffset + copied,
                            (size_t)bytes);
                list->CopyBufferRegion(slice.resource, copied, writeStage, 0, bytes);
                if (!SubmitAndWait()) return false;
                copied += bytes;
                if (copied < slice.dirtyBytes && !BeginList()) return false;
            }
            if (!BeginList()) return false;
            Transition(slice.resource, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COMMON);
            if (!SubmitAndWait()) return false;
        }

        // Restored slices stay unmapped while idle; the mapping is recreated
        // on the next CPU write, not on restore.
        slice.mapped = false;
        slice.cpuPtr = nullptr;
        slice.inVram = true;
        slicesRestored++;

        totalMs = timer.Ms();
        std::printf("[RESTORE] slice=%d total=%.2f ms resident=%llu MB\n",
                    index, totalMs,
                    (unsigned long long)(ResidentBytes() / (1024ull * 1024ull)));
        std::fflush(stdout);
        return true;
    }

    // Re-creates a released slice in place, keeping its metadata.
    bool AddSliceAt(PoolSlice& slice, uint64_t bytes)
    {
        ID3D12Resource* resource = nullptr;
        if (!CreateCustomL1(device, bytes, &resource)) return false;
        slice.resource = resource;
        slice.sizeBytes = bytes;
        return true;
    }

    uint64_t ResidentBytes() const
    {
        uint64_t total = 0;
        for (const auto& s : slices)
            if (s.inVram) total += s.sizeBytes;
        return total;
    }

    // -- eviction policy ----------------------------------------------------
    void EvictForDeficit(uint64_t deficit, const char* source)
    {
        if (deficit == 0) deficit = 1;
        std::printf("[%s] [STATE] EVICTING deficit=%llu MB (%s)\n",
                    WallClock().c_str(),
                    (unsigned long long)(deficit / (1024ull * 1024ull)), source);
        std::fflush(stdout);

        state = LeaseState::Evicting;
        uint64_t freed = 0;
        double totalMs = 0.0;
        int count = 0;

        for (size_t i = 0; i < slices.size(); ++i) {
            if (!slices[i].inVram) continue;
            double oneMs = 0.0;
            if (!EvictSlice(slices[i], (int)i, oneMs, freed)) {
                std::printf("[%s] [FAIL] eviction of slice %zu\n",
                            WallClock().c_str(), i);
                break;
            }
            totalMs += oneMs;
            ++count;
            if (freed >= deficit) break;
        }

        evictionRounds++;
        state = ResidentBytes() == 0 ? LeaseState::AllEvicted : LeaseState::Active;

        std::printf("[%s] [EVICT-DONE] slices=%d freed=%llu MB total=%.2f ms "
                    "resident=%llu MB\n",
                    WallClock().c_str(), count,
                    (unsigned long long)(freed / (1024ull * 1024ull)), totalMs,
                    (unsigned long long)(ResidentBytes() / (1024ull * 1024ull)));
        std::fflush(stdout);
    }

    void RestoreAll()
    {
        state = LeaseState::Restoring;
        std::printf("[%s] [STATE] RESTORING %d slice(s)\n",
                    WallClock().c_str(), slicesEvicted - slicesRestored);
        std::fflush(stdout);

        double totalMs = 0.0;
        for (size_t i = 0; i < slices.size(); ++i) {
            if (slices[i].inVram) continue;
            double oneMs = 0.0;
            if (!RestoreSlice(slices[i], (int)i, oneMs)) {
                std::printf("[%s] [FAIL] restore of slice %zu\n",
                            WallClock().c_str(), i);
                break;
            }
            totalMs += oneMs;
        }

        state = LeaseState::Active;
        std::printf("[%s] [RESTORE-DONE] total=%.2f ms resident=%llu MB\n",
                    WallClock().c_str(), totalMs,
                    (unsigned long long)(ResidentBytes() / (1024ull * 1024ull)));
        std::fflush(stdout);
    }

    // -- monitor ------------------------------------------------------------
    void Monitor()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        BudgetInfo info{};
        QueryBudget(adapter3, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, info);
        initialBudget = info.budget;

        uint64_t nvmlFree = 0, nvmlTotal = 0;
        const bool nvmlOk = QueryNvmlFree(nvmlFree, nvmlTotal);
        std::printf("[%s] [MONITOR] budget=%llu MB usage=%llu MB resident=%llu MB\n",
                    WallClock().c_str(),
                    (unsigned long long)(info.budget / (1024ull * 1024ull)),
                    (unsigned long long)(info.currentUsage / (1024ull * 1024ull)),
                    (unsigned long long)(ResidentBytes() / (1024ull * 1024ull)));
        if (nvmlOk) {
            std::printf("[%s] [MONITOR] nvml free=%llu MB total=%llu MB\n",
                        WallClock().c_str(),
                        (unsigned long long)(nvmlFree / (1024ull * 1024ull)),
                        (unsigned long long)(nvmlTotal / (1024ull * 1024ull)));
        }
        std::fflush(stdout);

        bool recoveryLogged = false;
        int recoveryStreak = 0;
        DWORD lastAction = 0;
        const uint64_t baselineFree = nvmlOk ? nvmlFree : 0;

        while (!stop.load(std::memory_order_relaxed)) {
            Timer reaction;
            reaction.Begin();
            WaitForSingleObjectEx(budgetEvent, 100, FALSE);

            BudgetInfo now{};
            if (!QueryBudget(adapter3, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, now))
                continue;

            uint64_t freeNow = 0, totalNow = 0;
            const bool nvmlNow = QueryNvmlFree(freeNow, totalNow);

            // Deficit is how much the newcomer took away from the baseline,
            // not a fixed threshold: this machine already runs with ~1.5 GB
            // free after the 4 GB pool is resident.
            uint64_t deficit = 0;
            const char* source = "wddm";
            if (now.currentUsage > now.budget)
                deficit = now.currentUsage - now.budget;

            if (nvmlNow) {
                // Keep half a slice of tolerance: evicting 2 of 4 slices is
                // enough to restore the free-VRAM baseline for a 6 GB load,
                // while an 8 GB load still takes the whole pool. Chasing the
                // exact baseline would always drain all slices.
                const uint64_t tolerance = sliceBytes;
                const uint64_t target =
                    baselineFree > tolerance ? baselineFree - tolerance : 0;
                uint64_t d = 0;
                if (freeNow < target) d = target - freeNow;
                if (freeNow < (768ull * 1024ull * 1024ull))
                    d = std::max(d, (768ull * 1024ull * 1024ull) - freeNow);
                if (d > deficit) {
                    deficit = d;
                    source = "nvml";
                }
            }

            const bool cooldownOver = (GetTickCount() - lastAction) > 2000;

            if (deficit > 0 && ResidentBytes() > 0 && cooldownOver) {
                const double reactionMs = reaction.Ms();
                std::printf("[%s] [SIGNAL] usage=%llu MB budget=%llu MB "
                            "nvml_free=%llu MB baseline=%llu MB deficit=%llu MB "
                            "reaction=%.2f ms [%s]\n",
                            WallClock().c_str(),
                            (unsigned long long)(now.currentUsage / (1024ull * 1024ull)),
                            (unsigned long long)(now.budget / (1024ull * 1024ull)),
                            (unsigned long long)(nvmlNow ? freeNow / (1024ull * 1024ull) : 0),
                            (unsigned long long)(baselineFree / (1024ull * 1024ull)),
                            (unsigned long long)(deficit / (1024ull * 1024ull)),
                            reactionMs, source);
                std::fflush(stdout);

                EvictForDeficit(deficit, source);
                lastAction = GetTickCount();
            } else if (deficit == 0 && slicesEvicted > slicesRestored) {
                const bool budgetRecovered =
                    initialBudget != 0 && now.budget >= initialBudget * 9 / 10;
                const bool nvmlRecovered =
                    !nvmlNow || freeNow > baselineFree + sliceBytes;
                const bool recovered = budgetRecovered && nvmlRecovered;
                recoveryStreak = recovered ? recoveryStreak + 1 : 0;

                if (recoveryStreak >= 3 && cooldownOver) {
                    if (!recoveryLogged) {
                        std::printf("[%s] [SIGNAL] recovered budget=%llu MB "
                                    "nvml_free=%llu MB\n",
                                    WallClock().c_str(),
                                    (unsigned long long)(now.budget / (1024ull * 1024ull)),
                                    (unsigned long long)(nvmlNow ? freeNow / (1024ull * 1024ull) : 0));
                        std::fflush(stdout);
                        recoveryLogged = true;
                    }
                    RestoreAll();
                    recoveryStreak = 0;
                    recoveryLogged = false;
                    lastAction = GetTickCount();
                }
            }
        }
    }

    void ReleaseAll()
    {
        FreeStaging();
        for (auto& s : slices) {
            if (s.resource) {
                if (s.mapped) s.resource->Unmap(0, nullptr);
                s.resource->Release();
            }
        }
        slices.clear();
        if (budgetEvent) { CloseHandle(budgetEvent); budgetEvent = nullptr; }
        if (fenceEvent) { CloseHandle(fenceEvent); fenceEvent = nullptr; }
        if (fence) { fence->Release(); fence = nullptr; }
        if (list) { list->Release(); list = nullptr; }
        if (allocator) { allocator->Release(); allocator = nullptr; }
        if (queue) { queue->Release(); queue = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        if (adapter3) { adapter3->Release(); adapter3 = nullptr; }
        if (nvmlModule) { FreeLibrary(nvmlModule); nvmlModule = nullptr; }
    }
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"CHZM Chunked Lease Broker");

    uint32_t targetMb = 4096;
    uint32_t sliceMb = 1024;
    uint32_t dirtyMb = 512;
    uint32_t runSeconds = 35;
    bool allowBusy = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--target-mb") == 0 && i + 1 < argc)
            targetMb = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--slice-mb") == 0 && i + 1 < argc)
            sliceMb = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--dirty-mb") == 0 && i + 1 < argc)
            dirtyMb = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc)
            runSeconds = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--allow-busy") == 0)
            allowBusy = true;
    }

    Broker broker;
    if (!broker.Init()) return 1;

    BudgetInfo initial{};
    QueryBudget(broker.adapter3, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, initial);

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);

    std::printf("======================================================================\n");
    std::printf("CHZM-SiliconFuse Chunked Lease Broker v0.2\n");
    std::printf("======================================================================\n");
    std::printf("Start wall clock   : %s\n", WallClock().c_str());
    std::printf("Budget at start    : %llu MB\n",
                (unsigned long long)(initial.budget / (1024ull * 1024ull)));
    std::printf("Usage  at start    : %llu MB\n",
                (unsigned long long)(initial.currentUsage / (1024ull * 1024ull)));
    std::printf("Host RAM available : %llu MB\n",
                (unsigned long long)(mem.ullAvailPhys / (1024ull * 1024ull)));
    std::printf("Target pool        : %u MB in %u MB slices\n", targetMb, sliceMb);

    // Safety interlock: refuse to take VRAM from a live game or 3D workload.
    // This is the guard that was missing when a Cyberpunk session crashed.
    if (!allowBusy) {
        Sleep(1500);
        unsigned samples[5] = {};
        int count = 0;
        for (int i = 0; i < 5; ++i) {
            unsigned util = 0;
            if (broker.QueryNvmlUtil(util)) samples[count++] = util;
            Sleep(200);
        }
        if (count > 0) {
            std::sort(samples, samples + count);
            const unsigned median = samples[count / 2];
            if (median > 40) {
                std::printf("[ABORT] GPU is busy: %u%% median utilization. A game "
                            "or 3D app is using the adapter.\n", median);
                std::printf("        Close it, or pass --allow-busy. No memory was "
                            "allocated.\n");
                return 2;
            }
        }
    }

    if (!broker.AllocatePool((uint64_t)targetMb * 1024ull * 1024ull,
                             (uint64_t)sliceMb * 1024ull * 1024ull)) {
        std::printf("[FAIL] no Tier-1 slices allocated\n");
        return 1;
    }

    const uint64_t actual = broker.totalBytes;
    std::printf("Allocated pool     : %llu MB in %llu slices\n",
                (unsigned long long)(actual / (1024ull * 1024ull)),
                (unsigned long long)broker.slices.size());
    if (actual < 2048ull * 1024ull * 1024ull)
        std::printf("[WARN] pool below the 2048 MB minimum\n");

    uint64_t dirtyPerSlice = std::min<uint64_t>(
        (uint64_t)dirtyMb * 1024ull * 1024ull, broker.sliceBytes);
    while (dirtyPerSlice >= 64ull * 1024ull * 1024ull &&
           !broker.PrepareStaging(dirtyPerSlice)) {
        std::printf("[WARN] staging allocation failed for %llu MB, retrying\n",
                    (unsigned long long)(dirtyPerSlice / (1024ull * 1024ull)));
        dirtyPerSlice /= 2;
    }
    if (broker.stageBytes == 0) {
        std::printf("[FAIL] could not allocate host staging buffers\n");
        return 1;
    }

    broker.FillSlices(dirtyPerSlice);
    std::printf("Dirty working set  : %llu MB total (%llu MB per slice)\n",
                (unsigned long long)(broker.dirtyTotal / (1024ull * 1024ull)),
                (unsigned long long)(dirtyPerSlice / (1024ull * 1024ull)));

    if (FAILED(broker.adapter3->RegisterVideoMemoryBudgetChangeNotificationEvent(
            broker.budgetEvent, &broker.cookie))) {
        std::printf("[FAIL] RegisterVideoMemoryBudgetChangeNotificationEvent\n");
        return 1;
    }
    std::printf("Budget notification: registered (cookie %lu)\n",
                (unsigned long)broker.cookie);
    std::printf("----------------------------------------------------------------------\n");
    std::fflush(stdout);

    std::thread monitor(&Broker::Monitor, &broker);

    const DWORD started = GetTickCount();
    while (!broker.stop.load() &&
           (GetTickCount() - started) < runSeconds * 1000)
        Sleep(100);

    broker.stop.store(true);
    SetEvent(broker.budgetEvent);
    monitor.join();

    BudgetInfo finalInfo{};
    QueryBudget(broker.adapter3, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, finalInfo);

    std::printf("----------------------------------------------------------------------\n");
    std::printf("SUMMARY\n");
    std::printf("  Slices total     : %llu\n",
                (unsigned long long)broker.slices.size());
    std::printf("  Pool bytes       : %llu MB\n",
                (unsigned long long)(broker.totalBytes / (1024ull * 1024ull)));
    std::printf("  Resident at exit : %llu MB\n",
                (unsigned long long)(broker.ResidentBytes() / (1024ull * 1024ull)));
    std::printf("  Eviction rounds  : %d\n", broker.evictionRounds);
    std::printf("  Slices evicted   : %d\n", broker.slicesEvicted);
    std::printf("  Slices restored  : %d\n", broker.slicesRestored);
    std::printf("  Final budget     : %llu MB\n",
                (unsigned long long)(finalInfo.budget / (1024ull * 1024ull)));
    std::printf("  Final usage      : %llu MB\n",
                (unsigned long long)(finalInfo.currentUsage / (1024ull * 1024ull)));
    std::printf("  End wall clock   : %s\n", WallClock().c_str());
    std::fflush(stdout);
    return 0;
}
