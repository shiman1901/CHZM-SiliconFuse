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

    // Fail-safe mirror for CryoSwap hibernation. File-backed on purpose: a
    // RAM mirror would defeat the whole point of freeing physical memory, and
    // a disk mirror survives VRAM eviction, a TDR or a driver reset.
    // Opened with FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH so the
    // Cache Manager never hides the data in DDR4 and every byte really lands
    // on the NVMe.
    bool InitCryoMirror(const std::wstring& directory, uint64_t capacityBytes);
    bool ReopenCryoMirror();   // close/reopen without truncation (chaos test)
    bool WriteMirror(uint64_t offset, const void* data, uint64_t bytes);
    bool ReadMirror(uint64_t offset, void* dst, uint64_t bytes);
    uint64_t MirrorCapacity() const { return mirrorCapacity_; }
    uint64_t MirrorBytes() const { return mirrorBytes_.load(); }
    bool MirrorDurable() const { return mirrorDurable_.load(); }
    // Test-only: flips one byte inside the mirrored copy (Chaos 3B).
    bool CorruptMirrorByte(uint64_t offset);

    struct IoSnapshot {
        uint64_t writeBytes = 0;
        uint64_t readBytes = 0;
    };
    static IoSnapshot QueryIo();

private:
    void WriterLoop();

    std::vector<uint8_t> buffer_;
    std::wstring path_;
    FILE* file_ = nullptr;
    FILE* log_ = nullptr;

    HANDLE mirror_ = INVALID_HANDLE_VALUE;
    std::wstring mirrorPath_;
    uint64_t mirrorCapacity_ = 0;
    std::mutex mirrorMutex_;
    std::atomic<uint64_t> mirrorBytes_{ 0 };
    std::atomic<bool> mirrorDurable_{ false };

    std::mutex ioMutex_;
    std::thread writer_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> dirty_{ false };
    std::atomic<uint64_t> dirtyBytes_{ 0 };
    std::atomic<uint64_t> fileBytes_{ 0 };
    std::atomic<double> lastReadUs_{ 0.0 };
    std::atomic<double> lastFlushMs_{ 0.0 };
};
