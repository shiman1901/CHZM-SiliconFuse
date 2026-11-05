#include "StorageBridge.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

double QpcMs(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER freq)
{
    return (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}

} // namespace

StorageBridge::StorageBridge() = default;

StorageBridge::~StorageBridge()
{
    Shutdown();
}

bool StorageBridge::Init(const std::wstring& directory, uint64_t capacityBytes)
{
    CreateDirectoryW(directory.c_str(), nullptr);
    path_ = directory + L"\\spill.bin";

    buffer_.assign((size_t)capacityBytes, 0);
    if (buffer_.empty()) return false;

    _wfopen_s(&file_, path_.c_str(), L"w+b");
    if (!file_) {
        if (log_) fwprintf(log_, L"[bridge] cannot open spill file\n");
        return false;
    }
    stop_.store(false);
    writer_ = std::thread(&StorageBridge::WriterLoop, this);
    if (log_) {
        fwprintf(log_, L"[bridge] staging %llu MB, spill %s\n",
                 (unsigned long long)(capacityBytes / (1024ull * 1024ull)),
                 path_.c_str());
        fflush(log_);
    }
    return true;
}

void StorageBridge::Shutdown()
{
    stop_.store(true);
    if (writer_.joinable()) writer_.join();
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> mirrorLock(mirrorMutex_);
        if (mirror_ != INVALID_HANDLE_VALUE) {
            CloseHandle(mirror_);
            mirror_ = INVALID_HANDLE_VALUE;
        }
    }
    buffer_.clear();
    buffer_.shrink_to_fit();
}

namespace {

constexpr DWORD kMirrorSector = 4096;
constexpr DWORD kMirrorChunk = 64u * 1024u * 1024u;

bool IsAligned(uint64_t value) { return (value % kMirrorSector) == 0; }

} // namespace

bool StorageBridge::InitCryoMirror(const std::wstring& directory, uint64_t capacityBytes)
{
    std::lock_guard<std::mutex> lock(mirrorMutex_);
    if (mirror_ != INVALID_HANDLE_VALUE) {
        CloseHandle(mirror_);
        mirror_ = INVALID_HANDLE_VALUE;
    }
    mirrorPath_ = directory + L"\\cryo_mirror.bin";
    mirrorCapacity_ = capacityBytes;
    mirrorBytes_.store(0);
    mirrorDurable_.store(false);
    mirror_ = CreateFileW(mirrorPath_.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING |
                              FILE_FLAG_WRITE_THROUGH | FILE_FLAG_SEQUENTIAL_SCAN,
                          nullptr);
    mirrorDurable_.store(mirror_ != INVALID_HANDLE_VALUE);
    return mirror_ != INVALID_HANDLE_VALUE;
}

bool StorageBridge::ReopenCryoMirror()
{
    std::lock_guard<std::mutex> lock(mirrorMutex_);
    if (mirrorPath_.empty()) return false;
    if (mirror_ != INVALID_HANDLE_VALUE) {
        CloseHandle(mirror_);
        mirror_ = INVALID_HANDLE_VALUE;
    }
    mirror_ = CreateFileW(mirrorPath_.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING |
                              FILE_FLAG_WRITE_THROUGH | FILE_FLAG_SEQUENTIAL_SCAN,
                          nullptr);
    mirrorDurable_.store(mirror_ != INVALID_HANDLE_VALUE);
    return mirror_ != INVALID_HANDLE_VALUE;
}

bool StorageBridge::WriteMirror(uint64_t offset, const void* data, uint64_t bytes)
{
    if (mirror_ == INVALID_HANDLE_VALUE || !data || bytes == 0) return false;
    if (offset + bytes > mirrorCapacity_) return false;
    // Unbuffered handles demand sector-aligned offsets, sizes and buffers.
    if (!IsAligned(offset) || !IsAligned(bytes) ||
        (reinterpret_cast<uintptr_t>(data) % kMirrorSector) != 0) {
        mirrorDurable_.store(false);
        return false;
    }

    std::lock_guard<std::mutex> lock(mirrorMutex_);
    LARGE_INTEGER position{};
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(mirror_, position, nullptr, FILE_BEGIN)) {
        mirrorDurable_.store(false);
        return false;
    }

    const uint8_t* source = static_cast<const uint8_t*>(data);
    uint64_t remaining = bytes;
    while (remaining > 0) {
        const DWORD chunk = (DWORD)std::min<uint64_t>(remaining, kMirrorChunk);
        DWORD written = 0;
        if (!WriteFile(mirror_, source, chunk, &written, nullptr) ||
            written != chunk) {
            mirrorDurable_.store(false);
            return false;
        }
        source += written;
        remaining -= written;
    }
    if (offset + bytes > mirrorBytes_.load()) mirrorBytes_.store(offset + bytes);
    mirrorDurable_.store(true);
    return true;
}

bool StorageBridge::ReadMirror(uint64_t offset, void* dst, uint64_t bytes)
{
    if (mirror_ == INVALID_HANDLE_VALUE || !dst || bytes == 0) return false;
    if (offset + bytes > mirrorBytes_.load()) return false;
    if (!IsAligned(offset) || !IsAligned(bytes) ||
        (reinterpret_cast<uintptr_t>(dst) % kMirrorSector) != 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mirrorMutex_);
    LARGE_INTEGER position{};
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(mirror_, position, nullptr, FILE_BEGIN)) return false;

    uint8_t* target = static_cast<uint8_t*>(dst);
    uint64_t remaining = bytes;
    while (remaining > 0) {
        const DWORD chunk = (DWORD)std::min<uint64_t>(remaining, kMirrorChunk);
        DWORD read = 0;
        if (!ReadFile(mirror_, target, chunk, &read, nullptr) || read == 0) return false;
        target += read;
        remaining -= read;
    }
    return true;
}

bool StorageBridge::CorruptMirrorByte(uint64_t offset)
{
    if (mirror_ == INVALID_HANDLE_VALUE) return false;
    const uint64_t sector = offset - (offset % kMirrorSector);
    if (sector + kMirrorSector > mirrorBytes_.load()) return false;

    // Unbuffered I/O needs a sector-aligned buffer.
    auto* sectorBuffer = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kMirrorSector, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!sectorBuffer) return false;
    bool ok = ReadMirror(sector, sectorBuffer, kMirrorSector);
    if (ok) {
        sectorBuffer[offset % kMirrorSector] ^= 0xFF;
        ok = WriteMirror(sector, sectorBuffer, kMirrorSector);
    }
    VirtualFree(sectorBuffer, 0, MEM_RELEASE);
    return ok;
}

StorageBridge::IoSnapshot StorageBridge::QueryIo()
{
    IoSnapshot snapshot;
    IO_COUNTERS counters{};
    if (GetProcessIoCounters(GetCurrentProcess(), &counters)) {
        snapshot.writeBytes = counters.WriteTransferCount;
        snapshot.readBytes = counters.ReadTransferCount;
    }
    return snapshot;
}

bool StorageBridge::ReadFromStaging(uint64_t offset, void* dst, size_t length)
{
    if (offset + length > buffer_.size() || !dst) return false;

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    std::memcpy(dst, buffer_.data() + offset, length);

    QueryPerformanceCounter(&end);
    lastReadUs_.store(QpcMs(start, end, freq) * 1000.0);
    return true;
}

void StorageBridge::MarkDirty(uint64_t offset, uint64_t length)
{
    if (offset + length > buffer_.size()) return;
    dirtyBytes_.store(dirtyBytes_.load() + length);
    dirty_.store(true);
}

bool StorageBridge::FlushNow()
{
    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    std::lock_guard<std::mutex> lock(ioMutex_);
    if (!file_) return false;

    fseek(file_, 0, SEEK_SET);
    const size_t written = fwrite(buffer_.data(), 1, buffer_.size(), file_);
    fflush(file_);

    QueryPerformanceCounter(&end);
    lastFlushMs_.store(QpcMs(start, end, freq));
    fileBytes_.store((uint64_t)written);
    dirty_.store(false);
    return written == buffer_.size();
}

void StorageBridge::WriterLoop()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (!stop_.load()) {
        Sleep(1000);
        if (!dirty_.load()) continue;
        FlushNow();
        if (log_) {
            fwprintf(log_, L"[bridge] spill flushed: %llu MB in %.2f ms\n",
                     (unsigned long long)(fileBytes_.load() / (1024ull * 1024ull)),
                     lastFlushMs_.load());
            fflush(log_);
        }
    }
}
