#include "VramPool.h"
#include "StorageBridge.h"

#include <immintrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <share.h>

namespace {

constexpr uint64_t kMb = 1024ull * 1024ull;
constexpr uint64_t kDirtyPerSlice = 64ull * kMb;

struct Timer {
    LARGE_INTEGER freq{}, start{};
    Timer() { QueryPerformanceFrequency(&freq); }
    void Begin() { QueryPerformanceCounter(&start); }
    double Ms() const
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
    }
};

void Avx2Fill(void* dst, size_t bytes, uint64_t seed)
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

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes)
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

bool CreateCustomL1(ID3D12Device* device, uint64_t bytes, ID3D12Resource** out)
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

bool CreateHeapBuffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type,
                      D3D12_RESOURCE_STATES state, ID3D12Resource** out)
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

struct NvmlMemory { unsigned long long total, free, used; };
struct NvmlUtilization { unsigned int gpu, memory; };
typedef int (*PfnNvmlInit)(void);
typedef int (*PfnNvmlHandle)(unsigned int, void**);
typedef int (*PfnNvmlMemory)(void*, NvmlMemory*);
typedef int (*PfnNvmlUtil)(void*, NvmlUtilization*);

} // namespace

VramPool::VramPool() = default;

VramPool::~VramPool()
{
    Shutdown();
}

void VramPool::Log(const wchar_t* format, ...)
{
    if (!logFile_) return;
    va_list args;
    va_start(args, format);
    vfwprintf(logFile_, format, args);
    va_end(args);
    fwprintf(logFile_, L"\n");
    fflush(logFile_);
}

bool VramPool::Init(StorageBridge* bridge, uint32_t targetMb, uint32_t sliceMb,
                    const std::wstring& logPath)
{
    bridge_ = bridge;
    logPath_ = logPath;
    logFile_ = _wfsopen(logPath.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (!logFile_) return false;

    Log(L"[pool] CHZM SiliconFuse engine starting");

    if (!CreateDevice()) return false;

    const uint64_t targetBytes = (uint64_t)targetMb * kMb;
    sliceBytes_ = (uint64_t)sliceMb * kMb;

    // Staging must cover one dirty window.
    if (!PrepareStaging(kDirtyPerSlice)) {
        Log(L"[pool] staging allocation failed");
        return false;
    }

    unsigned util = 0;
    const bool haveUtil = QueryNvmlUtil(util);
    if (haveUtil && util > 40) {
        Log(L"[pool] GPU busy (%u%%), pool allocation deferred", util);
    } else {
        if (!AllocateSlices(targetBytes, sliceBytes_)) {
            Log(L"[pool] no slices allocated");
        }
    }

    budgetEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (budgetEvent_) {
        if (FAILED(adapter3_->RegisterVideoMemoryBudgetChangeNotificationEvent(
                budgetEvent_, &cookie_))) {
            Log(L"[pool] budget notification registration failed");
        }
    }

    PublishStats();
    return true;
}

bool VramPool::CreateDevice()
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

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
    if (!adapter1) return false;

    if (FAILED(adapter1->QueryInterface(IID_PPV_ARGS(&adapter3_))) ||
        FAILED(D3D12CreateDevice(adapter1, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&device_)))) {
        adapter1->Release();
        return false;
    }
    adapter1->Release();

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(device_->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue_))) ||
        FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                               IID_PPV_ARGS(&allocator_))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                                          allocator_, nullptr,
                                          IID_PPV_ARGS(&list_))) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&fence_)))) {
        return false;
    }
    list_->Close();
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    nvmlModule_ = LoadLibraryW(L"nvml.dll");
    if (nvmlModule_) {
        auto init = reinterpret_cast<PfnNvmlInit>(
            reinterpret_cast<void*>(GetProcAddress(nvmlModule_, "nvmlInit_v2")));
        auto getHandle = reinterpret_cast<PfnNvmlHandle>(
            reinterpret_cast<void*>(GetProcAddress(nvmlModule_, "nvmlDeviceGetHandleByIndex_v2")));
        nvmlGetMemory_ = reinterpret_cast<void*>(
            GetProcAddress(nvmlModule_, "nvmlDeviceGetMemoryInfo"));
        nvmlGetUtil_ = reinterpret_cast<void*>(
            GetProcAddress(nvmlModule_, "nvmlDeviceGetUtilizationRates"));
        if (init && getHandle && init() == 0 && getHandle(0, &nvmlDevice_) == 0) {
            Log(L"[pool] NVML available");
        } else {
            nvmlDevice_ = nullptr;
        }
    }
    return fenceEvent_ != nullptr;
}

bool VramPool::QueryBudget(uint64_t& budget, uint64_t& usage) const
{
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!adapter3_ ||
        FAILED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return false;
    budget = info.Budget;
    usage = info.CurrentUsage;
    return true;
}

bool VramPool::QueryNvmlFree(uint64_t& freeBytes) const
{
    if (!nvmlDevice_ || !nvmlGetMemory_) return false;
    NvmlMemory memory{};
    auto fn = reinterpret_cast<PfnNvmlMemory>(nvmlGetMemory_);
    if (fn(nvmlDevice_, &memory) != 0) return false;
    freeBytes = memory.free;
    return true;
}

bool VramPool::QueryNvmlUtil(unsigned& percent) const
{
    if (!nvmlDevice_ || !nvmlGetUtil_) return false;
    NvmlUtilization util{};
    auto fn = reinterpret_cast<PfnNvmlUtil>(nvmlGetUtil_);
    if (fn(nvmlDevice_, &util) != 0) return false;
    percent = util.gpu;
    return true;
}

bool VramPool::PrepareStaging(uint64_t bytes)
{
    stageBytes_ = bytes;
    if (!CreateHeapBuffer(device_, bytes, D3D12_HEAP_TYPE_READBACK,
                          D3D12_RESOURCE_STATE_COPY_DEST, &readStage_))
        return false;
    if (FAILED(readStage_->Map(0, nullptr, &readPtr_))) return false;
    if (!CreateHeapBuffer(device_, bytes, D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ, &writeStage_))
        return false;
    if (FAILED(writeStage_->Map(0, nullptr, &writePtr_))) return false;
    return true;
}

bool VramPool::AddSlice(uint64_t bytes)
{
    Slice slice;
    if (!CreateCustomL1(device_, bytes, &slice.resource)) return false;
    if (FAILED(slice.resource->Map(0, nullptr, &slice.cpu))) {
        slice.resource->Release();
        return false;
    }
    slice.size = bytes;
    slice.dirty = kDirtyPerSlice;
    slice.spillOffset = (uint64_t)slices_.size() * kDirtyPerSlice;
    slice.mapped = true;
    slice.inVram = true;
    slices_.push_back(slice);
    totalBytes_ += bytes;
    dirtyTotal_ += slice.dirty;
    return true;
}

bool VramPool::AllocateSlices(uint64_t targetBytes, uint64_t sliceBytes)
{
    while (!slices_.empty()) {
        auto& s = slices_.back();
        if (s.mapped) s.resource->Unmap(0, nullptr);
        s.resource->Release();
        slices_.pop_back();
    }
    totalBytes_ = 0;
    dirtyTotal_ = 0;

    while (totalBytes_ + sliceBytes <= targetBytes) {
        if (!AddSlice(sliceBytes)) break;
    }

    for (size_t i = 0; i < slices_.size(); ++i) {
        Slice& s = slices_[i];
        Avx2Fill(s.cpu, (size_t)s.dirty, 0x5EEDF00Dull + i * 0x1000000ull);
        s.resource->Unmap(0, nullptr);
        s.mapped = false;
        s.cpu = nullptr;
    }

    Log(L"[pool] allocated %llu MB in %llu slices",
        (unsigned long long)(totalBytes_ / kMb),
        (unsigned long long)slices_.size());
    return !slices_.empty();
}

bool VramPool::BeginList()
{
    if (FAILED(allocator_->Reset())) return false;
    return SUCCEEDED(list_->Reset(allocator_, nullptr));
}

bool VramPool::SubmitAndWait()
{
    if (FAILED(list_->Close())) return false;
    ID3D12CommandList* lists[] = { list_ };
    queue_->ExecuteCommandLists(1, lists);
    const UINT64 target = ++fenceValue_;
    if (FAILED(queue_->Signal(fence_, target))) return false;
    if (fence_->GetCompletedValue() < target) {
        if (FAILED(fence_->SetEventOnCompletion(target, fenceEvent_))) return false;
        if (WaitForSingleObject(fenceEvent_, 60000) != WAIT_OBJECT_0) return false;
    }
    return true;
}

bool VramPool::EvictSlice(size_t index, double& totalMs)
{
    Slice& slice = slices_[index];
    if (!slice.inVram) return true;

    Timer timer;
    timer.Begin();

    if (slice.mapped) {
        slice.resource->Unmap(0, nullptr);
        slice.mapped = false;
        slice.cpu = nullptr;
    }

    if (slice.dirty) {
        if (!BeginList()) return false;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = slice.resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list_->ResourceBarrier(1, &barrier);
        list_->CopyBufferRegion(readStage_, 0, slice.resource, 0, slice.dirty);
        if (!SubmitAndWait()) return false;
        std::memcpy(bridge_->Buffer() + slice.spillOffset, readPtr_, (size_t)slice.dirty);
        bridge_->MarkDirty(slice.spillOffset, slice.dirty);
    }

    ID3D12Pageable* pageable = slice.resource;
    device_->Evict(1, &pageable);
    slice.resource->Release();
    slice.resource = nullptr;
    slice.inVram = false;

    totalMs = timer.Ms();
    if (slice.dirty && totalMs > 0.0) {
        lastDmaMBps_ = (double)slice.dirty / (1024.0 * 1024.0) / (totalMs / 1000.0);
    }
    Log(L"[pool] slice %zu evicted in %.2f ms", index, totalMs);
    return true;
}

bool VramPool::RestoreSlice(size_t index, double& totalMs)
{
    Slice& slice = slices_[index];
    if (slice.inVram) return true;

    Timer timer;
    timer.Begin();

    if (!CreateCustomL1(device_, slice.size, &slice.resource)) return false;
    slice.inVram = true;

    if (slice.dirty) {
        if (!BeginList()) return false;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = slice.resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list_->ResourceBarrier(1, &barrier);
        std::memcpy(writePtr_, bridge_->Buffer() + slice.spillOffset, (size_t)slice.dirty);
        list_->CopyBufferRegion(slice.resource, 0, writeStage_, 0, slice.dirty);
        if (!SubmitAndWait()) return false;
    }

    totalMs = timer.Ms();
    if (slice.dirty && totalMs > 0.0) {
        lastDmaMBps_ = (double)slice.dirty / (1024.0 * 1024.0) / (totalMs / 1000.0);
    }
    Log(L"[pool] slice %zu restored in %.2f ms", index, totalMs);
    return true;
}

bool VramPool::EvictAll(double& totalMs)
{
    totalMs = 0.0;
    for (size_t i = 0; i < slices_.size(); ++i) {
        if (!slices_[i].inVram) continue;
        double one = 0.0;
        if (!EvictSlice(i, one)) return false;
        totalMs += one;
    }
    return true;
}

bool VramPool::RestoreAll(double& totalMs)
{
    totalMs = 0.0;
    for (size_t i = 0; i < slices_.size(); ++i) {
        if (slices_[i].inVram) continue;
        double one = 0.0;
        if (!RestoreSlice(i, one)) return false;
        totalMs += one;
    }
    return true;
}

bool VramPool::StartMonitor()
{
    stop_.store(false);
    monitor_ = std::thread(&VramPool::MonitorLoop, this);
    return true;
}

void VramPool::StopMonitor()
{
    stop_.store(true);
    if (budgetEvent_) SetEvent(budgetEvent_);
    if (monitor_.joinable()) monitor_.join();
}

void VramPool::RequestEvictAll()
{
    requestRestore_.store(false);
    requestEvict_.store(true);
}

void VramPool::RequestRestoreAll()
{
    requestEvict_.store(false);
    requestRestore_.store(true);
}

bool VramPool::IsEvacuated() const { return evacuated_.load(); }
bool VramPool::IsBusy() const { return busy_.load(); }

void VramPool::MonitorLoop()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    uint64_t baselineFree = 0;
    QueryNvmlFree(baselineFree);

    while (!stop_.load()) {
        WaitForSingleObjectEx(budgetEvent_, 100, FALSE);

        uint64_t budget = 0, usage = 0;
        QueryBudget(budget, usage);
        uint64_t freeNow = 0;
        const bool nvmlOk = QueryNvmlFree(freeNow);

        if (requestEvict_.exchange(false)) {
            busy_.store(true);
            double ms = 0.0;
            if (!slices_.empty() && EvictAll(ms)) {
                evacuated_.store(true);
                stats_.lastEvictMs = ms;
                Log(L"[pool] manual eviction complete: %.2f ms", ms);
            }
            busy_.store(false);
        }

        if (requestRestore_.exchange(false)) {
            busy_.store(true);
            double ms = 0.0;
            if (slices_.empty()) {
                AllocateSlices(4096 * kMb, sliceBytes_);
            }
            if (RestoreAll(ms)) {
                evacuated_.store(false);
                stats_.lastRestoreMs = ms;
                Log(L"[pool] manual restore complete: %.2f ms", ms);
            }
            busy_.store(false);
        }

        // Automatic policy only when the user has not forced a state.
        if (!evacuated_.load() && !slices_.empty()) {
            const uint64_t tolerance = sliceBytes_;
            const uint64_t target = baselineFree > tolerance ? baselineFree - tolerance : 0;
            uint64_t deficit = usage > budget ? usage - budget : 0;
            if (nvmlOk && freeNow < target) {
                deficit = std::max(deficit, target - freeNow);
            }

            if (deficit > 0) {
                busy_.store(true);
                uint64_t freed = 0;
                double total = 0.0;
                for (size_t i = 0; i < slices_.size(); ++i) {
                    if (!slices_[i].inVram) continue;
                    double one = 0.0;
                    if (!EvictSlice(i, one)) break;
                    total += one;
                    freed += slices_[i].size;
                    if (freed >= deficit) break;
                }
                stats_.lastEvictMs = total;
                int resident = 0;
                for (const auto& s : slices_) if (s.inVram) ++resident;
                evacuated_ = resident == 0;
                busy_.store(false);
            }
        }

        PublishStats();

        // No slices at all (startup guard): retry when the GPU is idle.
        if (slices_.empty() && !stop_.load()) {
            unsigned util = 0;
            if (QueryNvmlUtil(util) && util < 20) {
                AllocateSlices(4096 * kMb, sliceBytes_);
                PublishStats();
            }
        }
    }
}

void VramPool::PublishStats()
{
    PoolStats stats;
    stats.poolBytes = totalBytes_;
    stats.sliceCount = (int)slices_.size();
    stats.dirtyBytes = dirtyTotal_;

    uint64_t resident = 0;
    int residentCount = 0;
    for (const auto& s : slices_) {
        if (s.inVram) {
            resident += s.size;
            ++residentCount;
        }
    }
    stats.residentBytes = resident;
    stats.residentSlices = residentCount;
    stats.evacuated = residentCount == 0;
    stats.busy = busy_.load();

    uint64_t budget = 0, usage = 0;
    if (QueryBudget(budget, usage)) {
        stats.budgetBytes = budget;
        stats.usageBytes = usage;
    }
    uint64_t freeBytes = 0;
    stats.nvmlAvailable = QueryNvmlFree(freeBytes);
    stats.nvmlFreeBytes = freeBytes;
    stats.lastEvictMs = stats_.lastEvictMs;
    stats.lastRestoreMs = stats_.lastRestoreMs;
    stats.lastDmaMBps = lastDmaMBps_;

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_ = stats;
}

PoolStats VramPool::GetStats() const
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void VramPool::Shutdown()
{
    if (shutdownDone_) return;
    shutdownDone_ = true;

    Log(L"[pool] shutdown: monitor");
    StopMonitor();

    Log(L"[pool] shutdown: slices");
    for (auto& s : slices_) {
        if (s.resource) {
            if (s.mapped) s.resource->Unmap(0, nullptr);
            s.resource->Release();
        }
    }
    slices_.clear();

    Log(L"[pool] shutdown: staging");
    if (readStage_) {
        if (readPtr_) readStage_->Unmap(0, nullptr);
        readStage_->Release();
        readStage_ = nullptr;
    }
    if (writeStage_) {
        if (writePtr_) writeStage_->Unmap(0, nullptr);
        writeStage_->Release();
        writeStage_ = nullptr;
    }

    Log(L"[pool] shutdown: events");
    if (budgetEvent_) {
        CloseHandle(budgetEvent_);
        budgetEvent_ = nullptr;
    }
    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }

    Log(L"[pool] shutdown: gpu objects");
    if (fence_) { fence_->Release(); fence_ = nullptr; }
    if (list_) { list_->Release(); list_ = nullptr; }
    if (allocator_) { allocator_->Release(); allocator_ = nullptr; }
    if (queue_) { queue_->Release(); queue_ = nullptr; }

    Log(L"[pool] shutdown: device");
    if (device_) { device_->Release(); device_ = nullptr; }
    if (adapter3_) { adapter3_->Release(); adapter3_ = nullptr; }
    if (nvmlModule_) { FreeLibrary(nvmlModule_); nvmlModule_ = nullptr; }

    Log(L"[pool] shutdown: log");
    if (logFile_) {
        fclose(logFile_);
        logFile_ = nullptr;
    }
}
