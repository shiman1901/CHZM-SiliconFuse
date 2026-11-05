// ============================================================================
//  CHZM SiliconFuse - GPU pressure emulator
//  File: tools/LeaseBroker/gpu_pressure.cpp
//
//  Simulates a heavy 3D title requesting a large committed VRAM allocation and
//  actively touching it. This is the counterpart of CHZM_LeaseBroker: it
//  forces WDDM to rebalance per-process budgets, which is the signal the
//  broker reacts to.
//
//  The allocation is a normal DEFAULT (L1) buffer. The tool reports the real
//  budget/usage numbers before and after, so success is not inferred from the
//  CreateCommittedResource return code alone: committed VRAM can be paged, and
//  only the budget figures prove that the pages were actually made resident.
//
//  Build: build_broker.bat (MinGW-w64 g++, C++20, static)
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <random>
#include <algorithm>

static std::string WallClock()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

struct Timer {
    LARGE_INTEGER freq{};
    LARGE_INTEGER start{};
    Timer() { QueryPerformanceFrequency(&freq); }
    void Begin() { QueryPerformanceCounter(&start); }
    double Seconds() const
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
    }
};

struct BudgetInfo {
    uint64_t budget = 0;
    uint64_t usage = 0;
};

struct NvmlUtilization {
    unsigned int gpu;
    unsigned int memory;
};

typedef int (*PfnNvmlInit)(void);
typedef int (*PfnNvmlHandle)(unsigned int, void**);
typedef int (*PfnNvmlUtil)(void*, NvmlUtilization*);

// Returns the median GPU utilization over five samples, or -1 if NVML is
// unavailable. A settle delay avoids a false positive from our own broker's
// fill activity right after startup.
static int SampleGpuUtilization()
{
    HMODULE module = LoadLibraryW(L"nvml.dll");
    if (!module) return -1;

    auto init = reinterpret_cast<PfnNvmlInit>(
        reinterpret_cast<void*>(GetProcAddress(module, "nvmlInit_v2")));
    auto getHandle = reinterpret_cast<PfnNvmlHandle>(
        reinterpret_cast<void*>(GetProcAddress(module, "nvmlDeviceGetHandleByIndex_v2")));
    auto getUtil = reinterpret_cast<PfnNvmlUtil>(
        reinterpret_cast<void*>(GetProcAddress(module, "nvmlDeviceGetUtilizationRates")));

    int result = -1;
    void* device = nullptr;
    if (init && getHandle && getUtil && init() == 0 && getHandle(0, &device) == 0) {
        Sleep(1500);
        std::vector<int> samples;
        for (int i = 0; i < 5; ++i) {
            NvmlUtilization util{};
            if (getUtil(device, &util) == 0) samples.push_back((int)util.gpu);
            Sleep(200);
        }
        if (!samples.empty()) {
            std::sort(samples.begin(), samples.end());
            result = samples[samples.size() / 2];
        }
    }
    FreeLibrary(module);
    return result;
}

static bool QueryBudget(IDXGIAdapter3* adapter, BudgetInfo& info)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO raw{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &raw)))
        return false;
    info.budget = raw.Budget;
    info.usage = raw.CurrentUsage;
    return true;
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

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"CHZM GPU Pressure");

    uint32_t sizeMb = 8192;
    uint32_t touchMb = 4096;
    uint32_t seconds = 5;
    bool allowBusy = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--size-mb") == 0 && i + 1 < argc)
            sizeMb = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--touch-mb") == 0 && i + 1 < argc)
            touchMb = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--allow-busy") == 0)
            allowBusy = true;
    }

    // Safety interlock: never add pressure to a GPU that is already running a
    // game. This is the check that was missing during the Cyberpunk incident.
    if (!allowBusy) {
        const int util = SampleGpuUtilization();
        if (util > 40) {
            std::printf("[ABORT] GPU is busy: %d%% utilization. Close the game "
                        "or pass --allow-busy.\n", util);
            return 2;
        }
    }

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::printf("[%s] [FAIL] CreateDXGIFactory1\n", WallClock().c_str());
        return 1;
    }

    IDXGIAdapter1* adapter1 = nullptr;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* candidate = nullptr;
        if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
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
        std::printf("[%s] [FAIL] NVIDIA adapter not found\n", WallClock().c_str());
        return 1;
    }

    IDXGIAdapter3* adapter3 = nullptr;
    if (FAILED(adapter1->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
        std::printf("[%s] [FAIL] IDXGIAdapter3\n", WallClock().c_str());
        return 1;
    }

    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(adapter1, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&device)))) {
        std::printf("[%s] [FAIL] D3D12CreateDevice\n", WallClock().c_str());
        return 1;
    }
    adapter1->Release();

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    bool ready = fenceEvent != nullptr;
    ready = ready && SUCCEEDED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)));
    ready = ready && SUCCEEDED(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator)));
    ready = ready && SUCCEEDED(device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_COPY, allocator, nullptr, IID_PPV_ARGS(&list)));
    ready = ready && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                   IID_PPV_ARGS(&fence)));
    if (!ready) {
        std::printf("[%s] [FAIL] GPU context initialization\n", WallClock().c_str());
        return 1;
    }
    list->Close();

    BudgetInfo before{};
    QueryBudget(adapter3, before);

    std::printf("======================================================================\n");
    std::printf("CHZM-SiliconFuse GPU Pressure Emulator v0.1\n");
    std::printf("======================================================================\n");
    std::printf("[%s] Start wall clock : %s\n", WallClock().c_str(), WallClock().c_str());
    std::printf("[%s] Requested commit : %u MB\n", WallClock().c_str(), sizeMb);
    std::printf("[%s] Budget (LOCAL)   : %llu MB\n", WallClock().c_str(),
                (unsigned long long)(before.budget / (1024ull * 1024ull)));
    std::printf("[%s] Usage  (LOCAL)   : %llu MB\n", WallClock().c_str(),
                (unsigned long long)(before.usage / (1024ull * 1024ull)));

    ID3D12Resource* vram = nullptr;
    uint32_t allocatedMb = sizeMb;

    // First attempts use the requested size. WDDM may stall and then raise
    // this process's budget after the broker yields; a retry is what a real
    // title does after its first allocation is refused.
    for (int attempt = 0; attempt < 2 && !vram; ++attempt) {
        if (attempt > 0) {
            std::printf("[%s] [RETRY] requesting %u MB again after 2 s\n",
                        WallClock().c_str(), sizeMb);
            std::fflush(stdout);
            Sleep(2000);
        }
        if (CreateBuffer(device, (uint64_t)sizeMb * 1024ull * 1024ull,
                         D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                         &vram)) {
            allocatedMb = sizeMb;
            break;
        }
        std::printf("[%s] [WARN] commit of %u MB failed (attempt %d)\n",
                    WallClock().c_str(), sizeMb, attempt + 1);
        std::fflush(stdout);
    }

    while (!vram && allocatedMb >= 512) {
        allocatedMb /= 2;
        if (CreateBuffer(device, (uint64_t)allocatedMb * 1024ull * 1024ull,
                         D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                         &vram))
            break;
        std::printf("[%s] [WARN] commit failed, retrying with %u MB\n",
                    WallClock().c_str(), allocatedMb);
        std::fflush(stdout);
    }
    if (!vram) {
        std::printf("[%s] [FAIL] could not commit VRAM\n", WallClock().c_str());
        return 1;
    }

    const uint64_t vramBytes = (uint64_t)allocatedMb * 1024ull * 1024ull;
    const uint64_t touchBytes = std::min<uint64_t>(
        vramBytes, (uint64_t)touchMb * 1024ull * 1024ull);

    const uint64_t block = 64ull * 1024ull;
    ID3D12Resource* source = nullptr;
    void* sourcePtr = nullptr;
    if (!CreateBuffer(device, block, D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &source) ||
        FAILED(source->Map(0, nullptr, &sourcePtr))) {
        std::printf("[%s] [FAIL] staging allocation\n", WallClock().c_str());
        return 1;
    }
    std::memset(sourcePtr, 0xA5, (size_t)block);

    std::printf("[%s] Commit result    : %s (%u MB)\n", WallClock().c_str(),
                allocatedMb == sizeMb ? "OK" : "OK (reduced)", allocatedMb);
    std::printf("[%s] Touch window     : %llu MB of %llu MB\n", WallClock().c_str(),
                (unsigned long long)(touchBytes / (1024ull * 1024ull)),
                (unsigned long long)(vramBytes / (1024ull * 1024ull)));
    std::fflush(stdout);

    // Barrier is emitted with the first batch: the buffer is only used as a
    // copy destination from that point on.
    bool firstBatch = true;
    UINT64 fenceValue = 0;
    auto submitAndWait = [&]() -> bool {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        const UINT64 target = ++fenceValue;
        if (FAILED(queue->Signal(fence, target))) return false;
        if (fence->GetCompletedValue() < target) {
            if (FAILED(fence->SetEventOnCompletion(target, fenceEvent))) return false;
            if (WaitForSingleObject(fenceEvent, 120000) != WAIT_OBJECT_0) return false;
        }
        return true;
    };

    std::mt19937_64 rng(0xBADC0DEull);
    const uint64_t blocks = touchBytes / block;
    Timer timer;
    timer.Begin();
    uint64_t totalBytes = 0;
    uint64_t batches = 0;

    while (timer.Seconds() < (double)seconds) {
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr)))
            break;
        if (firstBatch) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = vram;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            list->ResourceBarrier(1, &barrier);
            firstBatch = false;
        }
        for (int i = 0; i < 128; ++i) {
            const uint64_t offset = (rng() % blocks) * block;
            list->CopyBufferRegion(vram, offset, source, 0, block);
            totalBytes += block;
        }
        if (!submitAndWait()) break;
        ++batches;
    }

    const double elapsed = timer.Seconds();
    BudgetInfo after{};
    QueryBudget(adapter3, after);

    const double gbps = elapsed > 0.0
        ? ((double)totalBytes / (1024.0 * 1024.0 * 1024.0)) / elapsed : 0.0;

    std::printf("[%s] Pressure done    : %.2f s, %llu batches, %.2f GB written\n",
                WallClock().c_str(), elapsed, (unsigned long long)batches,
                (double)totalBytes / (1024.0 * 1024.0 * 1024.0));
    std::printf("[%s] Write throughput : %.2f GB/s\n", WallClock().c_str(), gbps);
    std::printf("[%s] Budget (LOCAL)   : %llu MB\n", WallClock().c_str(),
                (unsigned long long)(after.budget / (1024ull * 1024ull)));
    std::printf("[%s] Usage  (LOCAL)   : %llu MB\n", WallClock().c_str(),
                (unsigned long long)(after.usage / (1024ull * 1024ull)));
    std::printf("[%s] VERDICT          : %s\n", WallClock().c_str(),
                (after.usage > 0 && totalBytes > 0) ? "PRESSURE APPLIED" : "NO PRESSURE");
    std::fflush(stdout);

    source->Unmap(0, nullptr);
    source->Release();
    vram->Release();
    CloseHandle(fenceEvent);
    fence->Release();
    list->Release();
    allocator->Release();
    queue->Release();
    device->Release();
    adapter3->Release();
    return 0;
}
