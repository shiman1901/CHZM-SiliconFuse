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

    double lastEvictMs = 0.0;
    double lastRestoreMs = 0.0;
    double lastDmaMBps = 0.0;

    bool busy = false;
    bool evacuated = false;
};

// 4 x 1 GB Tier-1 pool in GDDR6. Each slice is an independent D3D12
// CUSTOM / WRITE_COMBINE / L1 resource with its own host spill range.
class VramPool {
public:
    VramPool();
    ~VramPool();

    bool Init(StorageBridge* bridge, uint32_t targetMb, uint32_t sliceMb,
              const std::wstring& logPath);
    void Shutdown();

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

private:
    bool CreateDevice();
    bool BeginList();
    bool SubmitAndWait();
    bool AllocateSlices(uint64_t targetBytes, uint64_t sliceBytes);
    bool AddSlice(uint64_t bytes);
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
    double lastDmaMBps_ = 0.0;

    std::thread monitor_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> requestEvict_{ false };
    std::atomic<bool> requestRestore_{ false };
    std::atomic<bool> busy_{ false };
    std::atomic<bool> evacuated_{ false };
    bool shutdownDone_ = false;

    mutable std::mutex statsMutex_;
    PoolStats stats_;
};
