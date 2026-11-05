#include "StorageBridge.h"

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
    buffer_.clear();
    buffer_.shrink_to_fit();
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
