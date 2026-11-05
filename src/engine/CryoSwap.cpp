#include <initguid.h>

#include "CryoSwap.h"

#include "CommitGuard.h"
#include "StandbyPurge.h"
#include "VramPool.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <immintrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <set>
#include <string>
#include <vector>

#ifndef QUOTA_LIMITS_HARDWS_MAX_ENABLE
#define QUOTA_LIMITS_HARDWS_MAX_ENABLE 0x00000004
#endif

namespace {

constexpr uint64_t kMb = 1024ull * 1024ull;

constexpr uint64_t kMinWorkingSet = 100ull * kMb;
constexpr uint64_t kMaxFreezePerProcess = 512ull * kMb;
constexpr uint64_t kChunk = 64ull * kMb;
constexpr uint64_t kMinRegionBytes = 4ull * 1024;
constexpr int kMaxFreezePerCycle = 8;
constexpr ULONGLONG kMaxFreezeSeconds = 120;

constexpr SIZE_T kQuotaMin = 4ull * kMb;
constexpr SIZE_T kQuotaMax = 24ull * kMb;
constexpr float kAudioPeak = 0.0005f;

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

// --- ntdll process control -------------------------------------------------

typedef LONG(NTAPI* PfnNtProcess)(HANDLE);

PfnNtProcess SuspendFn()
{
    static PfnNtProcess fn = reinterpret_cast<PfnNtProcess>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSuspendProcess")));
    return fn;
}

PfnNtProcess ResumeFn()
{
    static PfnNtProcess fn = reinterpret_cast<PfnNtProcess>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtResumeProcess")));
    return fn;
}

bool SuspendProcess(HANDLE process)
{
    const PfnNtProcess fn = SuspendFn();
    return fn && fn(process) == 0;
}

bool ResumeProcess(HANDLE process)
{
    const PfnNtProcess fn = ResumeFn();
    return fn && fn(process) == 0;
}

int CountThreads(DWORD pid)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    int count = 0;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == pid) ++count;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

// --- audio ----------------------------------------------------------------

struct AudioMeterMinimal : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
};

const IID kIidAudioMeterInformation = {
    0xC02216F6, 0x8C67, 0x4B5B, { 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64 }
};

// IAudioMeterInformation vtable: 0..2 IUnknown, 3 GetPeakValue. Calling the
// slot explicitly keeps the call independent of the local interface type.
bool ReadPeak(void* meter, float& peak)
{
    if (!meter) return false;
    void** vtable = *reinterpret_cast<void***>(meter);
    if (!vtable || !vtable[3]) return false;
    typedef HRESULT(STDMETHODCALLTYPE* PfnGetPeakValue)(void*, float*);
    const PfnGetPeakValue fn = reinterpret_cast<PfnGetPeakValue>(vtable[3]);
    return SUCCEEDED(fn(meter, &peak));
}

std::set<DWORD> CollectAudioPids()
{
    std::set<DWORD> pids;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                   IID_IMMDeviceEnumerator,
                                   reinterpret_cast<void**>(&enumerator)))) {
        IMMDeviceCollection* devices = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                     &devices))) {
            UINT deviceCount = 0;
            devices->GetCount(&deviceCount);
            for (UINT i = 0; i < deviceCount; ++i) {
                IMMDevice* device = nullptr;
                if (FAILED(devices->Item(i, &device))) continue;

                IAudioSessionManager2* manager = nullptr;
                if (SUCCEEDED(device->Activate(IID_IAudioSessionManager2, CLSCTX_ALL,
                                               nullptr,
                                               reinterpret_cast<void**>(&manager)))) {
                    IAudioSessionEnumerator* sessions = nullptr;
                    if (SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
                        int sessionCount = 0;
                        sessions->GetCount(&sessionCount);
                        for (int s = 0; s < sessionCount; ++s) {
                            IAudioSessionControl* control = nullptr;
                            if (FAILED(sessions->GetSession(s, &control))) continue;

                            IAudioSessionControl2* control2 = nullptr;
                            if (SUCCEEDED(control->QueryInterface(
                                    IID_IAudioSessionControl2,
                                    reinterpret_cast<void**>(&control2)))) {
                                DWORD pid = 0;
                                if (SUCCEEDED(control2->GetProcessId(&pid)) && pid) {
                                    void* meter = nullptr;
                                    if (SUCCEEDED(control->QueryInterface(
                                            kIidAudioMeterInformation, &meter))) {
                                        float peak = 0.0f;
                                        if (ReadPeak(meter, peak) && peak > kAudioPeak) {
                                            pids.insert(pid);
                                        }
                                        reinterpret_cast<IUnknown*>(meter)->Release();
                                    }
                                }
                                control2->Release();
                            }
                            control->Release();
                        }
                        sessions->Release();
                    }
                    manager->Release();
                }
                device->Release();
            }
            devices->Release();
        }
        enumerator->Release();
    }

    if (SUCCEEDED(com)) CoUninitialize();
    return pids;
}

std::wstring BaseName(HANDLE process)
{
    wchar_t path[MAX_PATH * 2]{};
    DWORD size = (DWORD)(sizeof(path) / sizeof(path[0]));
    if (!QueryFullProcessImageNameW(process, 0, path, &size)) return L"";
    std::wstring full(path, size);
    const size_t slash = full.find_last_of(L"\\/");
    std::wstring base = slash == std::wstring::npos ? full : full.substr(slash + 1);
    for (wchar_t& c : base) c = (wchar_t)towlower(c);
    return base;
}

bool IsBlockedName(const std::wstring& name)
{
    static const wchar_t* kBlocked[] = {
        L"system", L"registry", L"memcompression", L"smss.exe", L"csrss.exe",
        L"wininit.exe", L"winlogon.exe", L"services.exe", L"lsass.exe",
        L"svchost.exe", L"dwm.exe", L"explorer.exe", L"fontdrvhost.exe",
        L"audiodg.exe", L"backgroundtaskhost.exe", L"searchindexer.exe",
    };
    for (const wchar_t* blocked : kBlocked) {
        if (name == blocked) return true;
    }
    return false;
}

DWORD ForegroundPid()
{
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid;
}

void ApplyEcoQos(HANDLE process, bool on)
{
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state));
}

uint64_t Fnv1a(uint64_t hash, const uint8_t* data, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= (uint64_t)data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void StreamToVram(void* dst, const void* src, size_t bytes)
{
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    size_t i = 0;
    while (i < bytes && (reinterpret_cast<uintptr_t>(d + i) & 31) != 0) {
        d[i] = s[i];
        ++i;
    }
    for (; i + 32 <= bytes; i += 32) {
        const __m256i value =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i));
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d + i), value);
    }
    for (; i < bytes; ++i) d[i] = s[i];
    _mm_sfence();
}

} // namespace

CryoSwap::CryoSwap() = default;

CryoSwap::~CryoSwap()
{
    Shutdown();
}

void CryoSwap::Log(const wchar_t* format, ...)
{
    if (!pool_) return;
    va_list args;
    va_start(args, format);
    wchar_t buffer[1024];
    vswprintf_s(buffer, 1024, format, args);
    va_end(args);
    pool_->Log(L"%ls", buffer);
}

bool CryoSwap::EnablePrivileges()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    const wchar_t* names[] = { SE_INC_BASE_PRIORITY_NAME,
                               SE_PROF_SINGLE_PROCESS_NAME, SE_DEBUG_NAME };
    bool all = true;
    for (const wchar_t* name : names) {
        LUID luid{};
        if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
            all = false;
            continue;
        }
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        if (!AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges),
                                   nullptr, nullptr) ||
            GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
            all = false;
        }
    }
    CloseHandle(token);
    return all;
}

bool CryoSwap::Init(VramPool* pool)
{
    pool_ = pool;
    return pool_ != nullptr;
}

void CryoSwap::Shutdown()
{
    StopMonitor();
    Reset();
}

void CryoSwap::StartMonitor()
{
    if (monitor_.joinable()) return;
    stop_.store(false);
    monitor_ = std::thread(&CryoSwap::MonitorLoop, this);
}

void CryoSwap::StopMonitor()
{
    stop_.store(true);
    if (monitor_.joinable()) monitor_.join();
}

void CryoSwap::SetEnabled(bool on)
{
    enabled_.store(on);
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.enabled = on;
}

bool CryoSwap::IsCritical(const std::wstring& name) const { return IsBlockedName(name); }

// Spreads cached pages evenly over the slices (round-robin per 16 MB chunk),
// so a 512 MB freeze lands as 128 MB in each of the four slices.
bool CryoSwap::PlaceChunk(uint32_t bytes, uint64_t& vramOffset)
{
    const uint32_t count = pool_ ? pool_->SliceCount() : 0;
    const uint64_t sliceBytes = pool_ ? pool_->SliceBytes() : 0;
    if (count == 0 || sliceBytes == 0) return false;
    if (sliceFill_.size() != count) sliceFill_.assign(count, 0);

    for (uint32_t attempt = 0; attempt < count; ++attempt) {
        const uint32_t slice = (nextSlice_++) % count;
        if (sliceFill_[slice] + bytes <= sliceBytes) {
            vramOffset = (uint64_t)slice * sliceBytes + sliceFill_[slice];
            sliceFill_[slice] += bytes;
            return true;
        }
    }
    return false;
}

bool CryoSwap::StreamWrite(uint64_t offset, const void* data, size_t bytes)
{
    void* destination = pool_ ? pool_->BeginSliceWrite(offset, bytes) : nullptr;
    if (!destination) return false;
    StreamToVram(destination, data, bytes);
    pool_->EndSliceWrite(offset);
    return true;
}

bool CryoSwap::CryoFreeze(DWORD pid, FreezeResult& result)
{
    result = FreezeResult{};
    if (!pool_) return false;
    if (IsFrozen(pid)) return false;

    Timer totalTimer;
    totalTimer.Begin();

    HANDLE process = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION |
                                     PROCESS_SET_QUOTA | PROCESS_VM_READ |
                                     PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
                                 FALSE, pid);
    if (!process) return false;

    PROCESS_MEMORY_COUNTERS counters{};
    if (!GetProcessMemoryInfo(process, &counters, sizeof(counters))) {
        CloseHandle(process);
        return false;
    }
    result.wsBefore = counters.WorkingSetSize;

    const int threadsBefore = CountThreads(pid);
    if (threadsBefore <= 0 || !SuspendProcess(process)) {
        CloseHandle(process);
        return false;
    }
    Sleep(8);
    const int threadsAfter = CountThreads(pid);
    result.threads = threadsAfter;
    if (threadsAfter != threadsBefore) {
        Log(L"[cryo] pid=%lu thread list unstable (%d -> %d), aborting",
            (unsigned long)pid, threadsBefore, threadsAfter);
        ResumeProcess(process);
        CloseHandle(process);
        return false;
    }

    // Strictly private, writable data pages: MEM_RESET only honours those.
    std::vector<CachedRegion> regions;
    uint64_t planned = 0;
    MEMORY_BASIC_INFORMATION info{};
    const uint8_t* address = nullptr;
    const uint8_t* limit = reinterpret_cast<const uint8_t*>(0x7FFFFFFF0000ull);
    while (address < limit && planned < kMaxFreezePerProcess) {
        if (VirtualQueryEx(process, address, &info, sizeof(info)) != sizeof(info)) break;
        const uint8_t* base = static_cast<const uint8_t*>(info.BaseAddress);
        const uint64_t size = (uint64_t)info.RegionSize;
        if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE &&
            info.Protect == PAGE_READWRITE && size >= kMinRegionBytes) {
            CachedRegion region;
            region.address = (uint64_t)base;
            region.size = std::min(size, kMaxFreezePerProcess - planned);
            regions.push_back(region);
            planned += region.size;
        }
        if (size == 0) break;
        address = base + size;
        if (address <= base) break;
    }

    if (regions.empty()) {
        ResumeProcess(process);
        CloseHandle(process);
        return false;
    }

    // Ingress: RAM -> GDDR6 through the WC-mapped slices (AVX2 streaming),
    // round-robin per chunk across the slices.
    std::vector<uint8_t> bounce((size_t)kChunk);
    uint64_t written = 0;
    bool ingressOk = true;
    Timer ingressTimer;
    ingressTimer.Begin();
    for (CachedRegion& region : regions) {
        uint64_t hash = 1469598103934665603ull;
        uint64_t offset = 0;
        while (offset < region.size) {
            const uint64_t chunk = std::min(region.size - offset, (uint64_t)kChunk);
            SIZE_T read = 0;
            if (!ReadProcessMemory(process,
                                   reinterpret_cast<const void*>(region.address + offset),
                                   bounce.data(), (SIZE_T)chunk, &read) || read == 0) {
                ingressOk = false;
                break;
            }
            hash = Fnv1a(hash, bounce.data(), (size_t)read);

            uint64_t vramOffset = 0;
            if (!PlaceChunk((uint32_t)read, vramOffset) ||
                !StreamWrite(vramOffset, bounce.data(), (size_t)read)) {
                ingressOk = false;
                break;
            }
            CachedChunk placed;
            placed.vramOffset = vramOffset;
            placed.bytes = (uint32_t)read;
            region.chunks.push_back(placed);

            written += read;
            offset += read;
        }
        region.hash = hash;
        if (!ingressOk) break;
    }
    result.bytes = written;
    result.regions = (int)regions.size();
    result.ingressMs = ingressTimer.Ms();

    if (!ingressOk || written == 0) {
        ResumeProcess(process);
        CloseHandle(process);
        return false;
    }

    // Discard the physical pages: MEM_RESET keeps them out of pagefile.sys.
    for (const CachedRegion& region : regions) {
        VirtualAllocEx(process, reinterpret_cast<void*>(region.address),
                       (SIZE_T)region.size, MEM_RESET, PAGE_READWRITE);
    }
    SetProcessWorkingSetSizeEx(process, kQuotaMin, kQuotaMax,
                               QUOTA_LIMITS_HARDWS_MAX_ENABLE);
    EmptyWorkingSet(process);
    ApplyEcoQos(process, true);

    PROCESS_MEMORY_COUNTERS after{};
    if (GetProcessMemoryInfo(process, &after, sizeof(after))) {
        result.wsAfter = after.WorkingSetSize;
    } else {
        result.wsAfter = result.wsBefore;
    }

    CachedProcess cached;
    cached.pid = pid;
    cached.wsBefore = result.wsBefore;
    cached.bytes = written;
    cached.frozenAt = GetTickCount64();
    cached.regions = std::move(regions);

    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        cache_.push_back(std::move(cached));
    }

    result.ok = true;
    result.totalMs = totalTimer.Ms();
    Log(L"[cryo] frozen pid=%lu ws=%.0f->%.0f MB gddr6=%llu MB regions=%d threads=%d "
        L"ingress=%.0f MB/s total=%.0f ms",
        (unsigned long)pid, (double)result.wsBefore / (1024.0 * 1024.0),
        (double)result.wsAfter / (1024.0 * 1024.0),
        (unsigned long long)(written / kMb), result.regions, result.threads,
        result.ingressMs > 0.0 ? (double)written / (1024.0 * 1024.0) /
                                     (result.ingressMs / 1000.0) : 0.0,
        result.totalMs);
    // Handles stay open across the suspend window on purpose: the process is
    // suspended, we keep nothing after this function.
    CloseHandle(process);
    return true;
}

bool CryoSwap::CryoRestore(DWORD pid, bool forceDma, RestoreResult& result)
{
    result = RestoreResult{};

    CachedProcess entry;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = std::find_if(cache_.begin(), cache_.end(),
                               [pid](const CachedProcess& p) { return p.pid == pid; });
        if (it == cache_.end()) return false;
        entry = *it;
        cache_.erase(it);
    }

    HANDLE process = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION |
                                     PROCESS_SET_QUOTA | PROCESS_VM_READ |
                                     PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
                                 FALSE, pid);
    if (!process) return false;

    Timer timer;
    timer.Begin();

    SetProcessWorkingSetSizeEx(process, (SIZE_T)-1, (SIZE_T)-1, 0);
    ApplyEcoQos(process, false);

    std::vector<uint8_t> staging((size_t)kChunk);
    uint64_t restored = 0;

    for (const CachedRegion& region : entry.regions) {
        bool undone = false;
        if (!forceDma) {
            void* undoneBase = VirtualAllocEx(process,
                                              reinterpret_cast<void*>(region.address),
                                              (SIZE_T)region.size, MEM_RESET_UNDO,
                                              PAGE_READWRITE);
            undone = undoneBase != nullptr;
        }

        if (undone) {
            ++result.undoRegions;
            restored += region.size;
            continue;
        }

        ++result.dmaRegions;
        uint64_t hash = 1469598103934665603ull;
        uint64_t regionOffset = 0;
        for (const CachedChunk& chunk : region.chunks) {
            double chunkMbps = 0.0;
            if (!pool_->ReadCryoDmaInto(chunk.vramOffset, chunk.bytes, staging.data(),
                                        chunkMbps)) {
                result.checksumOk = false;
                break;
            }
            if (chunkMbps > result.dmaMBps) result.dmaMBps = chunkMbps;
            hash = Fnv1a(hash, staging.data(), chunk.bytes);
            SIZE_T wrote = 0;
            if (!WriteProcessMemory(process,
                                    reinterpret_cast<void*>(region.address + regionOffset),
                                    staging.data(), (SIZE_T)chunk.bytes, &wrote) ||
                wrote != chunk.bytes) {
                result.checksumOk = false;
                break;
            }
            regionOffset += chunk.bytes;
            restored += chunk.bytes;
        }
        if (hash != region.hash) {
            result.checksumOk = false;
            Log(L"[cryo] pid=%lu region 0x%llX checksum mismatch", (unsigned long)pid,
                (unsigned long long)region.address);
        }
    }

    ResumeProcess(process);
    CloseHandle(process);

    result.bytes = restored;
    result.totalMs = timer.Ms();
    result.ok = result.checksumOk;

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.lastWakeMs = result.totalMs;
        stats_.lastEgressMBps = result.dmaMBps;
    }
    Log(L"[cryo] restored pid=%lu %llu MB in %.2f ms (dma=%d undo=%d checksum=%ls)",
        (unsigned long)pid, (unsigned long long)(restored / kMb), result.totalMs,
        result.dmaRegions, result.undoRegions, result.checksumOk ? L"MATCH" : L"MISMATCH");
    return result.ok;
}

bool CryoSwap::IsFrozen(DWORD pid) const
{
    std::lock_guard<std::mutex> lock(cacheMutex_);
    for (const CachedProcess& cached : cache_) {
        if (cached.pid == pid) return true;
    }
    return false;
}

bool CryoSwap::WakeNow(DWORD pid, double& wakeMs)
{
    RestoreResult result;
    if (!CryoRestore(pid, false, result)) return false;
    wakeMs = result.totalMs;
    return true;
}

std::vector<CachedProcess> CryoSwap::SnapshotFrozen() const
{
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return cache_;
}

void CryoSwap::PublishCycle(int frozen, int skippedAudio, int skippedCritical,
                            int skippedSmall, uint64_t hotBytes, uint64_t savedBytes,
                            uint64_t cycleMs, double ingressMBps)
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.compressed += frozen;
    stats_.skippedAudio += skippedAudio;
    stats_.skippedCritical += skippedCritical;
    stats_.skippedSmall += skippedSmall;
    stats_.hotBytes += hotBytes;
    stats_.savedBytes += savedBytes;
    stats_.lastCycleMs = cycleMs;
    stats_.lastIngressMBps = ingressMBps;
}

int CryoSwap::RunCycle(DWORD onlyPid)
{
    if (!pool_) return 0;
    std::unique_lock<std::mutex> guard(cycleMutex_, std::try_to_lock);
    if (!guard.owns_lock()) return 0;

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.busy = true;
    }

    Timer cycleTimer;
    cycleTimer.Begin();

    if (AvailablePhysicalMb() < 1536) {
        const bool purged = PurgeLowPriorityStandbyList();
        Log(L"[cryo] low memory: standby purge %ls",
            purged ? L"ok" : L"unavailable (needs admin)");
    }

    const std::set<DWORD> audio = CollectAudioPids();
    const DWORD foreground = ForegroundPid();
    const DWORD self = GetCurrentProcessId();

    std::vector<DWORD> pids(4096);
    DWORD needed = 0;
    if (!EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &needed)) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.busy = false;
        return 0;
    }
    size_t count = needed / sizeof(DWORD);
    if (count > pids.size()) count = pids.size();

    int frozen = 0;
    int skippedAudio = 0;
    int skippedCritical = 0;
    int skippedSmall = 0;
    uint64_t hotBytes = 0;
    uint64_t savedBytes = 0;
    double ingressMBps = 0.0;

    for (size_t i = 0; i < count && frozen < kMaxFreezePerCycle; ++i) {
        const DWORD pid = pids[i];
        if (!pid || pid == 4 || pid == self) continue;
        if (onlyPid != 0 && pid != onlyPid) continue;
        if (pid == foreground) continue;
        if (audio.count(pid)) { ++skippedAudio; continue; }
        if (IsFrozen(pid)) continue;

        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                     FALSE, pid);
        if (!process) continue;
        PROCESS_MEMORY_COUNTERS counters{};
        const bool haveCounters =
            GetProcessMemoryInfo(process, &counters, sizeof(counters)) != FALSE;
        const std::wstring name = BaseName(process);
        CloseHandle(process);
        if (!haveCounters) continue;
        if (counters.WorkingSetSize < kMinWorkingSet) { ++skippedSmall; continue; }
        if (IsCritical(name)) { ++skippedCritical; continue; }

        FreezeResult freeze;
        Timer timer;
        timer.Begin();
        if (CryoFreeze(pid, freeze)) {
            freeze.ingressMs = timer.Ms();
            ++frozen;
            hotBytes += freeze.bytes;
            if (freeze.wsBefore > freeze.wsAfter) {
                savedBytes += freeze.wsBefore - freeze.wsAfter;
            }
            if (freeze.ingressMs > 0.0) {
                ingressMBps = (double)freeze.bytes / (1024.0 * 1024.0) /
                              (freeze.ingressMs / 1000.0);
            }
        }
    }

    const uint64_t cycleMs = (uint64_t)cycleTimer.Ms();
    PublishCycle(frozen, skippedAudio, skippedCritical, skippedSmall, hotBytes,
                 savedBytes, cycleMs, ingressMBps);

    Log(L"[cryo] cycle: frozen=%d hot=%llu MB saved=%llu MB audio=%d critical=%d "
        L"small=%d ingress=%.0f MB/s in %llu ms",
        frozen, (unsigned long long)(hotBytes / kMb),
        (unsigned long long)(savedBytes / kMb), skippedAudio, skippedCritical,
        skippedSmall, ingressMBps, (unsigned long long)cycleMs);

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.busy = false;
    return frozen;
}

int CryoSwap::Reset()
{
    std::vector<DWORD> pids;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        for (const CachedProcess& cached : cache_) pids.push_back(cached.pid);
    }

    int restored = 0;
    for (DWORD pid : pids) {
        RestoreResult result;
        if (CryoRestore(pid, false, result)) ++restored;
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        cache_.clear();
    }
    sliceFill_.clear();
    nextSlice_ = 0;

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.savedBytes = 0;
    stats_.hotBytes = 0;
    stats_.compressed = 0;
    stats_.frozen = 0;
    return restored;
}

CryoStats CryoSwap::GetStats() const
{
    CryoStats copy;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        copy = stats_;
    }
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        copy.frozen = (int)cache_.size();
    }
    return copy;
}

void CryoSwap::MonitorLoop()
{
    DWORD lastForeground = 0;
    ULONGLONG lastCycle = GetTickCount64();

    while (!stop_.load()) {
        const DWORD foreground = ForegroundPid();
        if (foreground && foreground != lastForeground) {
            lastForeground = foreground;
            if (IsFrozen(foreground)) {
                double ms = 0.0;
                WakeNow(foreground, ms);
            }
        }

        // Safety net: a process nobody comes back to must not stay suspended.
        {
            const ULONGLONG now = GetTickCount64();
            std::vector<DWORD> stale;
            {
                std::lock_guard<std::mutex> lock(cacheMutex_);
                for (const CachedProcess& cached : cache_) {
                    if (now - cached.frozenAt > kMaxFreezeSeconds * 1000ull) {
                        stale.push_back(cached.pid);
                    }
                }
            }
            for (DWORD pid : stale) {
                RestoreResult result;
                Log(L"[cryo] pid=%lu frozen for %llu s, safety restore",
                    (unsigned long)pid, (unsigned long long)kMaxFreezeSeconds);
                CryoRestore(pid, false, result);
            }
        }

        if (enabled_.load()) {
            const ULONGLONG now = GetTickCount64();
            if (now - lastCycle >= 15000) {
                lastCycle = now;
                if (AvailablePhysicalMb() < 1024) RunCycle();
            }
        }

        Sleep(80);
    }
}
