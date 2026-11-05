#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Host-side landing zone for evicted slices.
//
// Eviction copies VRAM into this buffer first; a Windows read served from here
// costs one memcpy and never touches the disk. A background thread persists
// the buffer to a spill file, so the slow path is asynchronous by construction.
class StorageBridge {
public:
    StorageBridge();
    ~StorageBridge();

    bool Init(const std::wstring& directory, uint64_t capacityBytes);
    void Shutdown();

    uint8_t* Buffer() { return buffer_.data(); }
    uint64_t Capacity() const { return buffer_.size(); }

    // Host read path: memcpy from the staging buffer. This is the path that
    // must never block on I/O.
    bool ReadFromStaging(uint64_t offset, void* dst, size_t length);

    void MarkDirty(uint64_t offset, uint64_t length);
    bool FlushNow();
    void SetLogFile(FILE* log) { log_ = log; }

    uint64_t DirtyBytes() const { return dirtyBytes_.load(); }
    double LastReadUs() const { return lastReadUs_.load(); }
    double LastFlushMs() const { return lastFlushMs_.load(); }
    uint64_t FileBytes() const { return fileBytes_.load(); }

private:
    void WriterLoop();

    std::vector<uint8_t> buffer_;
    std::wstring path_;
    FILE* file_ = nullptr;
    FILE* log_ = nullptr;

    std::mutex ioMutex_;
    std::thread writer_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> dirty_{ false };
    std::atomic<uint64_t> dirtyBytes_{ 0 };
    std::atomic<uint64_t> fileBytes_{ 0 };
    std::atomic<double> lastReadUs_{ 0.0 };
    std::atomic<double> lastFlushMs_{ 0.0 };
};
