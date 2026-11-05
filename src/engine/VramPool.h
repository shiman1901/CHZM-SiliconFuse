#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class StorageBridge;

struct PoolStats {
    uint64_t poolBytes = 0;
    uint64_t residentBytes = 0;
    uint64_t dirtyBytes = 0;
    int sliceCount = 0;
    int residentSlices = 0;

    uint64_t budgetBytes = 0;
    uint64_t usageBytes = 0;
    uint64_t nvmlFreeBytes = 0;
    bool nvmlAvailable = false;

    // Hardware sensors: framebuffer, BAR1 window and the DXGI local budget.
    uint64_t fbUsedBytes = 0;
    uint64_t fbTotalBytes = 0;
    uint64_t bar1UsedBytes = 0;
    uint64_t bar1TotalBytes = 0;

    double lastEvictMs = 0.0;
    double lastRestoreMs = 0.0;
    double lastDmaMBps = 0.0;

    bool busy = false;
    bool evacuated = false;

    // Adaptive sizing: target chosen at startup (or by the UI) and the
    // adapter capacity it was derived from.
    uint32_t targetMb = 0;
    uint32_t autoTargetMb = 0;
    uint64_t adapterVramBytes = 0;
};

// 4 x 1 GB Tier-1 pool in GDDR6. Each slice is an independent D3D12
// CUSTOM / WRITE_COMBINE / L1 resource with its own host spill range.
class VramPool {
public:
    VramPool();
    ~VramPool();

    // targetMb == 0 selects the automatic size for the detected adapter.
    bool Init(StorageBridge* bridge, uint32_t targetMb, uint32_t sliceMb,
              const std::wstring& logPath);
    void Shutdown();

    // Adaptive sizing. The monitor thread owns the slice vector; the UI only
    // posts an intent and reads stats.
    uint64_t AdapterVramBytes() const { return adapterVramBytes_; }
    uint32_t AutoTargetMb() const { return autoTargetMb_; }
    uint32_t TargetMb() const { return targetMb_.load(); }
    void RequestResize(uint32_t targetMb);

    bool StartMonitor();
    void StopMonitor();

    // Manual override from the UI. The monitor thread performs the work.
    void RequestEvictAll();
    void RequestRestoreAll();
    bool IsEvacuated() const;
    bool IsBusy() const;

    PoolStats GetStats() const;
    uint64_t TotalBytes() const { return totalBytes_; }
    void Log(const wchar_t* format, ...);

    // CryoSwap hot-page cache: a linear byte space laid over the slices.
    uint32_t SliceCount() const;
    uint64_t SliceBytes() const { return sliceBytes_; }
    uint64_t CryoCapacity() const;
    // Maps one slice region for a streaming write. The pool lock stays held
    // until EndSliceWrite, so the monitor thread cannot evict underneath.
    void* BeginSliceWrite(uint64_t offset, uint64_t bytes);
    void EndSliceWrite(uint64_t offset);
    // Reads cached pages back through the copy engine; mbps gets the real
    // transfer rate shown as "PCIe DMA".
    bool ReadCryoDma(uint64_t offset, uint64_t bytes, double& mbps);
    // Same, but lands in a host buffer (DMA -> readback staging -> memcpy).
    bool ReadCryoDmaInto(uint64_t offset, uint64_t bytes, void* dst, double& mbps);
    // One log line with framebuffer, BAR1, DXGI and DMA counters.
    void LogSensors(const wchar_t* tag);

private:
    bool CreateDevice();
    bool BeginList();
    bool SubmitAndWait();
    bool AllocateSlices(uint64_t targetBytes, uint64_t sliceBytes);
    bool AddSlice(uint64_t bytes);
    bool ResizeTo(uint32_t targetMb);
    bool PrepareStaging(uint64_t bytes);

    bool EvictSlice(size_t index, double& totalMs);
    bool RestoreSlice(size_t index, double& totalMs);
    bool EvictAll(double& totalMs);
    bool RestoreAll(double& totalMs);
    void MonitorLoop();
    void PublishStats();

    bool QueryBudget(uint64_t& budget, uint64_t& usage) const;
    bool QueryNvmlFree(uint64_t& freeBytes) const;
    bool QueryNvmlUtil(unsigned& percent) const;
    bool QueryNvmlBar1(uint64_t& usedBytes, uint64_t& totalBytes) const;

    // D3D12
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12CommandAllocator* allocator_ = nullptr;
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;

    IDXGIAdapter3* adapter3_ = nullptr;
    HANDLE budgetEvent_ = nullptr;
    DWORD cookie_ = 0;

    // NVML
    HMODULE nvmlModule_ = nullptr;
    void* nvmlDevice_ = nullptr;
    void* nvmlGetMemory_ = nullptr;
    void* nvmlGetUtil_ = nullptr;
    void* nvmlGetBar1_ = nullptr;

    struct Slice {
        ID3D12Resource* resource = nullptr;
        void* cpu = nullptr;
        uint64_t size = 0;
        uint64_t dirty = 0;
        uint64_t spillOffset = 0;
        bool inVram = true;
        bool mapped = false;
    };
    std::vector<Slice> slices_;

    ID3D12Resource* readStage_ = nullptr;
    void* readPtr_ = nullptr;
    ID3D12Resource* writeStage_ = nullptr;
    void* writePtr_ = nullptr;
    uint64_t stageBytes_ = 0;

    StorageBridge* bridge_ = nullptr;
    std::wstring logPath_;
    FILE* logFile_ = nullptr;

    uint64_t totalBytes_ = 0;
    uint64_t dirtyTotal_ = 0;
    uint64_t sliceBytes_ = 0;
    uint64_t adapterVramBytes_ = 0;
    uint32_t autoTargetMb_ = 0;
    double lastDmaMBps_ = 0.0;

    std::thread monitor_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> monitorRunning_{ false };
    std::atomic<bool> requestEvict_{ false };
    std::atomic<bool> requestRestore_{ false };
    std::atomic<bool> requestResize_{ false };
    std::atomic<uint32_t> targetMb_{ 0 };
    std::atomic<uint32_t> pendingTargetMb_{ 0 };
    std::atomic<bool> busy_{ false };
    std::atomic<bool> evacuated_{ false };
    bool shutdownDone_ = false;

    mutable std::mutex statsMutex_;
    mutable std::mutex opMutex_;
    uint64_t writeOffset_ = 0;
    PoolStats stats_;
};
