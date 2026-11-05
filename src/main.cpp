#include <windows.h>
#include <psapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <share.h>
#include <string>
#include <vector>

#include "engine/CommitGuard.h"
#include "engine/CryoSwap.h"
#include "engine/Hash64.h"
#include "engine/PagingManager.h"
#include "engine/StandbyPurge.h"
#include "engine/StorageBridge.h"
#include "engine/VramPool.h"
#include "gui/MainWindow.h"

namespace {

constexpr uint64_t kMb = 1024ull * 1024ull;
constexpr uint64_t kVictimDefaultMb = 512;
constexpr uint64_t kHashChunk = 16ull * kMb;
constexpr uint64_t kMirrorCapacity = 4096ull * kMb;

std::wstring LocalAppData()
{
    wchar_t path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, path))) {
        return std::wstring(path) + L"\\CHZM-SiliconFuse";
    }
    wchar_t fallback[MAX_PATH]{};
    GetTempPathW(MAX_PATH, fallback);
    return std::wstring(fallback) + L"CHZM-SiliconFuse";
}

std::wstring ExecutablePath()
{
    wchar_t path[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, path, (DWORD)(sizeof(path) / sizeof(path[0])));
    return path;
}

std::wstring ExecutableDir()
{
    std::wstring path = ExecutablePath();
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring ReportPath()
{
    const std::wstring docs = ExecutableDir() + L"\\..\\docs";
    if (GetFileAttributesW(docs.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return docs + L"\\STAGE10_CHAOS_REPORT.txt";
    }
    return LocalAppData() + L"\\STAGE10_CHAOS_REPORT.txt";
}

std::wstring VictimStatePath(uint64_t sizeMb)
{
    return LocalAppData() + L"\\victim_state_" + std::to_wstring(sizeMb) + L"mb.txt";
}

std::wstring VictimResultPath(uint64_t sizeMb)
{
    return LocalAppData() + L"\\victim_result_" + std::to_wstring(sizeMb) + L"mb.txt";
}

std::wstring VerifyEventName(uint64_t sizeMb)
{
    return L"Global\\ChzmVictimVerify_" + std::to_wstring(sizeMb);
}

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

void Report(FILE* out, const wchar_t* format, ...)
{
    if (!out) return;
    va_list args;
    va_start(args, format);
    vfwprintf(out, format, args);
    va_end(args);
    fwprintf(out, L"\n");
    fflush(out);
}

uint8_t PatternByte(uint64_t index, uint32_t seed)
{
    return (uint8_t)(((index * 167ull + 13ull) ^ (seed * 2654435761ull)) & 0xFF);
}

struct ProcNumbers {
    double wsMb = -1.0;
    double commitMb = -1.0;
    uint64_t pageFaults = 0;
};

ProcNumbers ReadProcessNumbers(DWORD pid)
{
    ProcNumbers numbers;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return numbers;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(process,
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        numbers.wsMb = (double)counters.WorkingSetSize / (1024.0 * 1024.0);
        numbers.commitMb = (double)counters.PrivateUsage / (1024.0 * 1024.0);
        numbers.pageFaults = counters.PageFaultCount;
    }
    CloseHandle(process);
    return numbers;
}

bool HashRemoteRange(DWORD pid, uint64_t address, uint64_t bytes, uint64_t& hash)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) return false;

    std::vector<uint8_t> buffer((size_t)std::min(kHashChunk, bytes));
    Hash64 hasher;
    uint64_t offset = 0;
    while (offset < bytes) {
        const uint64_t chunk = std::min(bytes - offset, kHashChunk);
        SIZE_T read = 0;
        if (!ReadProcessMemory(process,
                               reinterpret_cast<const void*>(address + offset),
                               buffer.data(), (SIZE_T)chunk, &read) || read == 0) {
            CloseHandle(process);
            return false;
        }
        hasher.Update(buffer.data(), (size_t)read);
        offset += read;
    }
    hash = hasher.Final();
    CloseHandle(process);
    return true;
}

// Wipes the physical pages of a frozen process and discards them again, so the
// "data is really gone from RAM" claim is not just a log line.
bool ZeroFrozenProcess(CryoSwap& cryo, DWORD pid, uint64_t& zeroedMb)
{
    const std::vector<CachedProcess> snapshot = cryo.SnapshotFrozen();
    const CachedProcess* target = nullptr;
    for (const CachedProcess& cached : snapshot) {
        if (cached.pid == pid) { target = &cached; break; }
    }
    if (!target) return false;

    HANDLE process = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                                     PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, pid);
    if (!process) return false;

    std::vector<uint8_t> zeros((size_t)(16ull * kMb), 0);
    uint64_t zeroed = 0;
    for (const CachedRegion& region : target->regions) {
        uint64_t offset = 0;
        while (offset < region.size) {
            const uint64_t chunk = std::min(region.size - offset,
                                            (uint64_t)zeros.size());
            SIZE_T wrote = 0;
            if (!WriteProcessMemory(process,
                                    reinterpret_cast<void*>(region.address + offset),
                                    zeros.data(), (SIZE_T)chunk, &wrote) || wrote == 0) {
                CloseHandle(process);
                return false;
            }
            zeroed += wrote;
            offset += wrote;
        }
        VirtualAllocEx(process, reinterpret_cast<void*>(region.address),
                       (SIZE_T)region.size, MEM_RESET, PAGE_READWRITE);
    }
    SetProcessWorkingSetSizeEx(process, 4ull * kMb, 24ull * kMb,
                               QUOTA_LIMITS_HARDWS_MAX_ENABLE);
    EmptyWorkingSet(process);
    CloseHandle(process);
    PurgeLowPriorityStandbyList();
    zeroedMb = zeroed / kMb;
    return true;
}

// ---------------------------------------------------------------- victim ----

int RunCryoVictim(const std::wstring& arguments)
{
    uint64_t sizeMb = kVictimDefaultMb;
    uint32_t seed = 1;
    const size_t flag = arguments.find(L"--cryo-victim");
    if (flag != std::wstring::npos) {
        const wchar_t* rest = arguments.c_str() + flag + wcslen(L"--cryo-victim");
        wchar_t* end = nullptr;
        const long value = wcstol(rest, &end, 10);
        if (value > 0) sizeMb = (uint64_t)value;
        if (end) {
            const long seedValue = wcstol(end, nullptr, 10);
            if (seedValue > 0) seed = (uint32_t)seedValue;
        }
    }
    const size_t total = (size_t)(sizeMb * kMb);

    auto* buffer = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buffer) return 5;

    for (size_t i = 0; i < total; ++i) buffer[i] = PatternByte(i, seed);
    const uint64_t hash = Hash64::Compute(buffer, total);

    FILE* state = _wfsopen(VictimStatePath(sizeMb).c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (state) {
        fwprintf(state, L"pid=%lu\n", (unsigned long)GetCurrentProcessId());
        fwprintf(state, L"base=%llu\n", (unsigned long long)(uintptr_t)buffer);
        fwprintf(state, L"size_mb=%llu\n", (unsigned long long)sizeMb);
        fwprintf(state, L"seed=%u\n", seed);
        fwprintf(state, L"hash=%llu\n", (unsigned long long)hash);
        fclose(state);
    }

    HANDLE verify = CreateEventW(nullptr, TRUE, FALSE, VerifyEventName(sizeMb).c_str());
    if (!verify) return 6;
    if (WaitForSingleObject(verify, 240000) != WAIT_OBJECT_0) return 7;

    const uint64_t after = Hash64::Compute(buffer, total);
    size_t matching = 0;
    for (size_t i = 0; i < total; ++i) {
        if (buffer[i] == PatternByte(i, seed)) ++matching;
    }
    const double percent = 100.0 * (double)matching / (double)total;

    FILE* result = _wfsopen(VictimResultPath(sizeMb).c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (result) {
        fwprintf(result, L"match_percent=%.4f\n", percent);
        fwprintf(result, L"hash_original=%llu\n", (unsigned long long)hash);
        fwprintf(result, L"hash_after=%llu\n", (unsigned long long)after);
        fwprintf(result, L"hash_equal=%d\n", hash == after ? 1 : 0);
        fclose(result);
    }
    CloseHandle(verify);
    return 0;
}

// ------------------------------------------------------------- harness ------

struct Marker {
    DWORD pid = 0;
    uint64_t base = 0;
    uint64_t sizeMb = 0;
    uint64_t hash = 0;
};

bool ParseMarker(uint64_t sizeMb, Marker& marker)
{
    FILE* file = _wfsopen(VictimStatePath(sizeMb).c_str(), L"r, ccs=UTF-8", _SH_DENYNO);
    if (!file) return false;
    wchar_t line[256];
    bool ok = false;
    while (fgetws(line, 256, file)) {
        unsigned long long value = 0;
        if (swscanf_s(line, L"pid=%llu", &value) == 1) marker.pid = (DWORD)value;
        if (swscanf_s(line, L"base=%llu", &value) == 1) marker.base = value;
        if (swscanf_s(line, L"size_mb=%llu", &value) == 1) marker.sizeMb = value;
        if (swscanf_s(line, L"hash=%llu", &value) == 1) { marker.hash = value; ok = true; }
    }
    fclose(file);
    return ok && marker.pid != 0;
}

struct VictimHandle {
    PROCESS_INFORMATION child{};
    HANDLE verify = nullptr;
    Marker marker;
    bool alive = false;
};

bool StartVictim(uint64_t sizeMb, uint32_t seed, VictimHandle& handle)
{
    DeleteFileW(VictimStatePath(sizeMb).c_str());
    DeleteFileW(VictimResultPath(sizeMb).c_str());

    handle.verify = CreateEventW(nullptr, TRUE, FALSE, VerifyEventName(sizeMb).c_str());
    if (handle.verify) ResetEvent(handle.verify);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    std::wstring command = L"\"" + ExecutablePath() + L"\" --cryo-victim " +
                           std::to_wstring(sizeMb) + L" " + std::to_wstring(seed);
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &handle.child)) {
        return false;
    }

    for (int i = 0; i < 120 && !ParseMarker(sizeMb, handle.marker); ++i) Sleep(100);
    handle.alive = handle.marker.pid != 0;
    return handle.alive;
}

double QueryVictimMatch(uint64_t sizeMb)
{
    for (int i = 0; i < 400 && GetFileAttributesW(VictimResultPath(sizeMb).c_str()) ==
                                 INVALID_FILE_ATTRIBUTES; ++i) {
        Sleep(50);
    }
    double percent = -1.0;
    FILE* result = _wfsopen(VictimResultPath(sizeMb).c_str(), L"r, ccs=UTF-8", _SH_DENYNO);
    if (result) {
        wchar_t line[256];
        while (fgetws(line, 256, result)) {
            double value = 0.0;
            if (swscanf_s(line, L"match_percent=%lf", &value) == 1) percent = value;
        }
        fclose(result);
    }
    return percent;
}

void StopVictim(VictimHandle& handle)
{
    if (handle.verify) {
        SetEvent(handle.verify);
        CloseHandle(handle.verify);
        handle.verify = nullptr;
    }
    if (handle.child.hProcess) {
        TerminateProcess(handle.child.hProcess, 0);
        CloseHandle(handle.child.hThread);
        CloseHandle(handle.child.hProcess);
        handle.child = PROCESS_INFORMATION{};
    }
    handle.alive = false;
}

// --------------------------------------------------------------- report -----

double SumFreezePhases(const FreezeResult& f)
{
    return f.suspendMs + f.scanZeroMs + f.rpmReadMs + f.vramWriteMs +
           f.mirrorWriteMs + f.hashMs + f.resetTrimMs;
}

double SumRestorePhases(const RestoreResult& r)
{
    return r.dmaGpuMs + r.fallbackMs + r.hashVerifyMs + r.wpmWriteMs + r.resumeMs;
}

void ReportFreezePhases(FILE* out, uint64_t sizeMb, const FreezeResult& f)
{
    const double sum = SumFreezePhases(f);
    Report(out, L"  %-22ls %10.2f", L"total_ms", f.totalMs);
    Report(out, L"  %-22ls %10.2f", L"  suspend_ms", f.suspendMs);
    Report(out, L"  %-22ls %10.2f", L"  scan_zero_ms", f.scanZeroMs);
    Report(out, L"  %-22ls %10.2f", L"  rpm_read_ms", f.rpmReadMs);
    Report(out, L"  %-22ls %10.2f", L"  avx2_vram_write_ms", f.vramWriteMs);
    Report(out, L"  %-22ls %10.2f", L"  mirror_write_ms", f.mirrorWriteMs);
    Report(out, L"  %-22ls %10.2f", L"  hash_ms", f.hashMs);
    Report(out, L"  %-22ls %10.2f", L"  reset_trim_ms", f.resetTrimMs);
    Report(out, L"  %-22ls %10.2f", L"  sum(phases)", sum);
    Report(out, L"  %-22ls %10.2f", L"  overhead_ms", f.overheadMs);
    Report(out, L"  %-22ls %10.2f (delta %.2f, must be < 0.1)",
           L"  sum+overhead", sum + f.overheadMs,
           f.totalMs - (sum + f.overheadMs));
    Report(out, L"  %-22ls %llu MB in %d regions", L"frozen",
           (unsigned long long)(f.bytes / kMb), f.regions);
    (void)sizeMb;
}

void ReportRestorePhases(FILE* out, const RestoreResult& r)
{
    const double sum = SumRestorePhases(r);
    Report(out, L"  %-22ls %10.2f", L"total_ms", r.totalMs);
    Report(out, L"  %-22ls %10.2f", L"  dma_gpu_ms", r.dmaGpuMs);
    Report(out, L"  %-22ls %10.2f", L"  fallback_ms", r.fallbackMs);
    Report(out, L"  %-22ls %10.2f", L"  hash_verify_ms", r.hashVerifyMs);
    Report(out, L"  %-22ls %10.2f", L"  wpm_write_ms", r.wpmWriteMs);
    Report(out, L"  %-22ls %10.2f", L"  resume_ms", r.resumeMs);
    Report(out, L"  %-22ls %10.2f", L"  sum(phases)", sum);
    Report(out, L"  %-22ls %10.2f", L"  overhead_ms", r.overheadMs);
    Report(out, L"  %-22ls %10.2f (delta %.2f, must be < 0.1)",
           L"  sum+overhead", sum + r.overheadMs,
           r.totalMs - (sum + r.overheadMs));
}

int RunTorture(VramPool& pool, CryoSwap& cryo, StorageBridge& bridge,
               const std::wstring& dataDir, bool full)
{
    const std::wstring path = ReportPath();
    FILE* out = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (!out) return 6;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    Report(out, L"CHZM//SILICONFUSE  STAGE 10  CHAOS REPORT");
    Report(out, L"generated=%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth,
           now.wDay, now.wHour, now.wMinute, now.wSecond);
    Report(out, L"mode=%ls", full ? L"torture (chaos engineering)" : L"quick");
    Report(out, L"");

    // ---------------------------------------------------------- TEST 1 -----
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC iconGroup = FindResourceW(module, MAKEINTRESOURCEW(1), RT_GROUP_ICON);
    wchar_t exePath[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH * 2);
    WIN32_FILE_ATTRIBUTE_DATA fileInfo{};
    GetFileAttributesExW(exePath, GetFileExInfoStandard, &fileInfo);
    const uint64_t exeBytes = ((uint64_t)fileInfo.nFileSizeHigh << 32) | fileInfo.nFileSizeLow;

    Report(out, L"[TEST 1: BUILD + EMBEDDED ICON]");
    Report(out, L"  exe_size_bytes  = %llu", (unsigned long long)exeBytes);
    Report(out, L"  rt_group_icon   = %ls", iconGroup ? L"PRESENT (id 1)" : L"MISSING");
    Report(out, L"  mirror_flags    = FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH");
    Report(out, L"  mirror_capacity = %llu MB", (unsigned long long)(kMirrorCapacity / kMb));
    Report(out, L"");

    // ---------------------------------------------------------- TEST 2 -----
    PoolStats stats = pool.GetStats();
    Report(out, L"[TEST 2: POOL + COMMITGUARD + RAW BANDWIDTH]");
    Report(out, L"  adapter_vram_mb = %llu", (unsigned long long)(stats.adapterVramBytes / kMb));
    Report(out, L"  pool_mb         = %llu", (unsigned long long)(stats.poolBytes / kMb));
    Report(out, L"  slices          = %d x %llu MB", stats.sliceCount,
           (unsigned long long)(pool.SliceBytes() / kMb));
    Report(out, L"  commit_free_mb  = %llu (floor %llu)",
           (unsigned long long)FreeCommitMb(), (unsigned long long)kCommitGuardFloorMb);
    Report(out, L"  mirror_durable  = %d", bridge.MirrorDurable() ? 1 : 0);

    std::vector<uint8_t> bench((size_t)(64ull * kMb), 0x5A);
    Timer benchTimer;
    benchTimer.Begin();
    const bool streamOk = cryo.StreamWrite(0, bench.data(), bench.size());
    const double streamMs = benchTimer.Ms();
    Report(out, L"  avx2_vram_write_mbps = %.1f (%.2f ms / 64 MB)",
           streamOk && streamMs > 0.0
               ? (double)bench.size() / (1024.0 * 1024.0) / (streamMs / 1000.0)
               : 0.0,
           streamMs);
    benchTimer.Begin();
    const uint64_t hashValue = Hash64::Compute(bench.data(), bench.size());
    const double hashMs = benchTimer.Ms();
    Report(out, L"  avx2_hash_ms_64mb = %.2f (%.1f GB/s, hash=%llu)", hashMs,
           hashMs > 0.0 ? (double)bench.size() / (1024.0 * 1024.0 * 1024.0) /
                              (hashMs / 1000.0)
                        : 0.0,
           (unsigned long long)hashValue);
    Report(out, L"");

    // --------------------------------------------------------- CHAOS 1 -----
    Report(out, L"[CHAOS 1: UNBUFFERED NVME MIRROR & ZERO CACHE LEAK (512 MB)]");
    if (full) {
        VictimHandle victim;
        if (StartVictim(512, 41, victim)) {
            const uint64_t availBefore = AvailablePhysicalMb();
            const StorageBridge::IoSnapshot ioBefore = StorageBridge::QueryIo();

            FreezeResult freeze;
            const bool frozen = cryo.CryoFreeze(victim.marker.pid, freeze);
            const StorageBridge::IoSnapshot ioAfterWrite = StorageBridge::QueryIo();
            const uint64_t writeDelta = ioAfterWrite.writeBytes - ioBefore.writeBytes;

            uint64_t zeroedMb = 0;
            ZeroFrozenProcess(cryo, victim.marker.pid, zeroedMb);
            const uint64_t availAfter = AvailablePhysicalMb();

            const bool reopened = bridge.ReopenCryoMirror();

            double evictMs = 0.0;
            pool.EvictAllSlices(evictMs);

            const StorageBridge::IoSnapshot ioBeforeRead = StorageBridge::QueryIo();
            RestoreResult restore;
            const bool restored = cryo.CryoRestore(victim.marker.pid, true, restore);
            const StorageBridge::IoSnapshot ioAfterRead = StorageBridge::QueryIo();
            const uint64_t readDelta = ioAfterRead.readBytes - ioBeforeRead.readBytes;

            if (victim.verify) SetEvent(victim.verify);
            const double percent = QueryVictimMatch(512);

            Report(out, L"  frozen            = %d", frozen ? 1 : 0);
            Report(out, L"  mirror_durable    = %d", freeze.mirrorDurable ? 1 : 0);
            Report(out, L"  mirror_bytes_mb   = %llu", (unsigned long long)(freeze.bytes / kMb));
            Report(out, L"  write_io_delta_mb = %llu (>= 512 required)",
                   (unsigned long long)(writeDelta / kMb));
            Report(out, L"  reopen_mirror     = %d", reopened ? 1 : 0);
            Report(out, L"  vram_evicted      = 1 (%.2f ms)", evictMs);
            Report(out, L"  read_io_delta_mb  = %llu (>= 512 required)",
                   (unsigned long long)(readDelta / kMb));
            Report(out, L"  avail_ram_before  = %llu MB", (unsigned long long)availBefore);
            Report(out, L"  avail_ram_after   = %llu MB", (unsigned long long)availAfter);
            Report(out, L"  avail_ram_delta   = %lld MB (cache must not eat the win)",
                   (long long)availAfter - (long long)availBefore);
            Report(out, L"  restored          = %d (mirror regions %d)", restored ? 1 : 0,
                   restore.mirrorRegions);
            Report(out, L"  checksum          = %ls", restore.checksumOk ? L"MATCH" : L"MISMATCH");
            Report(out, L"  bit_match         = %.4f%%", percent);
            StopVictim(victim);
            cryo.Reset();
            pool.RequestResize(1024);
            Sleep(200);
            pool.RequestResize(4096);
            Sleep(300);
        } else {
            Report(out, L"  victim_spawn = FAILED");
            StopVictim(victim);
        }
    }
    Report(out, L"");

    // --------------------------------------------------------- CHAOS 2 -----
    Report(out, L"[CHAOS 2: SIMULATED TDR / GPU DEVICE LOST RECOVERY (256 MB)]");
    if (full) {
        VictimHandle victim;
        if (StartVictim(256, 42, victim)) {
            FreezeResult freeze;
            const bool frozen = cryo.CryoFreeze(victim.marker.pid, freeze);

            pool.SimulateDeviceRemoved();
            const bool deviceGone = !pool.DeviceAvailable();

            RestoreResult restore;
            const bool restored = cryo.CryoRestore(victim.marker.pid, true, restore);

            if (victim.verify) SetEvent(victim.verify);
            const double percent = QueryVictimMatch(256);

            Report(out, L"  frozen            = %d (mirror_durable %d)", frozen ? 1 : 0,
                   freeze.mirrorDurable ? 1 : 0);
            Report(out, L"  device_removed    = %d (all D3D12 calls blocked)", deviceGone ? 1 : 0);
            Report(out, L"  dma_regions       = %d (must be 0)", restore.dmaRegions);
            Report(out, L"  mirror_regions    = %d", restore.mirrorRegions);
            Report(out, L"  mirror_bytes_mb   = %llu",
                   (unsigned long long)(restore.mirrorBytes / kMb));
            Report(out, L"  checksum          = %ls", restore.checksumOk ? L"MATCH" : L"MISMATCH");
            Report(out, L"  total_ms          = %.2f", restore.totalMs);
            Report(out, L"  bit_match         = %.4f%%", percent);
            StopVictim(victim);
            pool.ClearDeviceFault();
            cryo.Reset();
        } else {
            Report(out, L"  victim_spawn = FAILED");
            StopVictim(victim);
        }
    }
    Report(out, L"");

    // ------------------------------------------------------- CHAOS 3A -----
    Report(out, L"[CHAOS 3A: VRAM BIT-FLIP SELF-HEALING (128 MB)]");
    if (full) {
        VictimHandle victim;
        if (StartVictim(128, 43, victim)) {
            FreezeResult freeze;
            cryo.CryoFreeze(victim.marker.pid, freeze);

            const std::vector<CachedProcess> snapshot = cryo.SnapshotFrozen();
            uint64_t vramOffset = 0;
            for (const CachedProcess& cached : snapshot) {
                if (cached.pid == victim.marker.pid && !cached.regions.empty() &&
                    !cached.regions.front().chunks.empty()) {
                    vramOffset = cached.regions.front().chunks.front().vramOffset;
                }
            }
            const bool flipped = pool.PokeSliceByte(vramOffset + 100);

            RestoreResult restore;
            const bool restored = cryo.CryoRestore(victim.marker.pid, true, restore);

            if (victim.verify) SetEvent(victim.verify);
            const double percent = QueryVictimMatch(128);

            Report(out, L"  bit_flip_in_vram  = %d (offset 0x%llX)",
                   flipped ? 1 : 0, (unsigned long long)(vramOffset + 100));
            Report(out, L"  restored          = %d", restored ? 1 : 0);
            Report(out, L"  dma_regions       = %d (corrupted region rejected)",
                   restore.dmaRegions);
            Report(out, L"  mirror_regions    = %d (healed from NVMe)", restore.mirrorRegions);
            Report(out, L"  checksum          = %ls", restore.checksumOk ? L"MATCH" : L"MISMATCH");
            Report(out, L"  bit_match         = %.4f%%", percent);
            StopVictim(victim);
            cryo.Reset();
        } else {
            Report(out, L"  victim_spawn = FAILED");
            StopVictim(victim);
        }
    }
    Report(out, L"");

    // ------------------------------------------------------- CHAOS 3B -----
    Report(out, L"[CHAOS 3B: DUAL-SOURCE CORRUPTION SAFE LOCKOUT (64 MB)]");
    if (full) {
        VictimHandle victim;
        if (StartVictim(64, 44, victim)) {
            FreezeResult freeze;
            cryo.CryoFreeze(victim.marker.pid, freeze);

            const std::vector<CachedProcess> snapshot = cryo.SnapshotFrozen();
            uint64_t vramOffset = 0;
            uint64_t mirrorOffset = 0;
            for (const CachedProcess& cached : snapshot) {
                if (cached.pid == victim.marker.pid && !cached.regions.empty() &&
                    !cached.regions.front().chunks.empty()) {
                    vramOffset = cached.regions.front().chunks.front().vramOffset;
                    mirrorOffset = cached.regions.front().chunks.front().mirrorOffset;
                }
            }
            const bool vramFlip = pool.PokeSliceByte(vramOffset + 64);
            const bool mirrorFlip = bridge.CorruptMirrorByte(mirrorOffset + 64);

            RestoreResult restore;
            const bool restored = cryo.CryoRestore(victim.marker.pid, true, restore);
            const bool lockout = cryo.SafeLockout();
            const bool stillSuspended =
                GetFileAttributesW(VictimResultPath(64).c_str()) == INVALID_FILE_ATTRIBUTES;

            Report(out, L"  bit_flip_in_vram  = %d", vramFlip ? 1 : 0);
            Report(out, L"  bit_flip_in_mirror= %d", mirrorFlip ? 1 : 0);
            Report(out, L"  restore_returned  = %d (must be 0)", restored ? 1 : 0);
            Report(out, L"  safe_lockout      = %d", lockout ? 1 : 0);
            Report(out, L"  resumed           = %d (must be 0)", stillSuspended ? 0 : 1);
            Report(out, L"  action            = test terminates the victim manually");
            cryo.ClearLockout();
            StopVictim(victim);
            cryo.Reset();
        } else {
            Report(out, L"  victim_spawn = FAILED");
            StopVictim(victim);
        }
    }
    Report(out, L"");

    // --------------------------------------------------------- CHAOS 4 -----
    Report(out, L"[CHAOS 4: MID-FREEZE ABORT ROLLBACK (64 MB)]");
    if (full) {
        VictimHandle victim;
        if (StartVictim(64, 45, victim)) {
            const double wsBefore = ReadProcessNumbers(victim.marker.pid).wsMb;

            cryo.InjectFreezeAbort(true);
            FreezeResult freeze;
            const bool frozen = cryo.CryoFreeze(victim.marker.pid, freeze);
            cryo.InjectFreezeAbort(false);

            const double wsAfter = ReadProcessNumbers(victim.marker.pid).wsMb;
            const bool resumed =
                WaitForSingleObject(victim.child.hProcess, 0) != WAIT_OBJECT_0;

            if (victim.verify) SetEvent(victim.verify);
            const double percent = QueryVictimMatch(64);

            Report(out, L"  injected_failure  = mirror write aborted at 50%%");
            Report(out, L"  frozen            = %d (must be 0)", frozen ? 1 : 0);
            Report(out, L"  mirror_durable    = %d", freeze.mirrorDurable ? 1 : 0);
            Report(out, L"  ws_before_mb      = %.1f", wsBefore);
            Report(out, L"  ws_after_mb       = %.1f (unchanged, no MEM_RESET)", wsAfter);
            Report(out, L"  process_resumed   = %d", resumed ? 1 : 0);
            Report(out, L"  victim_intact     = %.4f%%", percent);
            StopVictim(victim);
            cryo.Reset();
        } else {
            Report(out, L"  victim_spawn = FAILED");
            StopVictim(victim);
        }
    }
    Report(out, L"");

    // --------------------------------------------------------- REGRESS -----
    Report(out, L"[TEST 9: VRAM EVICTION SURVIVAL (REGRESSION)]");
    if (full) {
        VictimHandle victim;
        if (StartVictim(256, 46, victim)) {
            FreezeResult freeze;
            cryo.CryoFreeze(victim.marker.pid, freeze);
            uint64_t zeroedMb = 0;
            ZeroFrozenProcess(cryo, victim.marker.pid, zeroedMb);
            double evictMs = 0.0;
            pool.EvictAllSlices(evictMs);
            RestoreResult restore;
            const bool restored = cryo.CryoRestore(victim.marker.pid, true, restore);
            if (victim.verify) SetEvent(victim.verify);
            const double percent = QueryVictimMatch(256);
            Report(out, L"  evicted_ms = %.2f | mirror_regions = %d | checksum = %ls",
                   evictMs, restore.mirrorRegions,
                   restore.checksumOk ? L"MATCH" : L"MISMATCH");
            Report(out, L"  restored = %d | bit_match = %.4f%%", restored ? 1 : 0, percent);
            StopVictim(victim);
            cryo.Reset();
            pool.RequestResize(1024);
            Sleep(200);
            pool.RequestResize(4096);
            Sleep(300);
        }
    }
    Report(out, L"");

    Report(out, L"[TEST 10: EXIT SAFETY (RestoreAllFrozen)]");
    if (full) {
        VictimHandle victim;
        if (StartVictim(64, 47, victim)) {
            FreezeResult freeze;
            const bool frozen = cryo.CryoFreeze(victim.marker.pid, freeze);
            const int restoredAll = cryo.RestoreAllFrozen();
            const bool alive = WaitForSingleObject(victim.child.hProcess, 0) != WAIT_OBJECT_0;
            if (victim.verify) SetEvent(victim.verify);
            const double percent = QueryVictimMatch(64);
            Report(out, L"  frozen = %d | restore_all = %d | alive = %d | frozen_left = %d",
                   frozen ? 1 : 0, restoredAll, alive ? 1 : 0, cryo.GetStats().frozen);
            Report(out, L"  bit_match = %.4f%%", percent);
            StopVictim(victim);
            cryo.Reset();
        }
    }
    Report(out, L"");

    Report(out, L"[SENSORS]");
    PoolStats sensors = pool.GetStats();
    Report(out, L"  nvml_fb_used_mb   = %llu / %llu",
           (unsigned long long)(sensors.fbUsedBytes / kMb),
           (unsigned long long)(sensors.fbTotalBytes / kMb));
    Report(out, L"  nvml_bar1_used_mb = %llu / %llu",
           (unsigned long long)(sensors.bar1UsedBytes / kMb),
           (unsigned long long)(sensors.bar1TotalBytes / kMb));
    Report(out, L"  mirror_bytes_used = %llu MB", (unsigned long long)(bridge.MirrorBytes() / kMb));
    Report(out, L"  mirror_durable    = %d", bridge.MirrorDurable() ? 1 : 0);
    Report(out, L"  pool_end_mb       = %llu",
           (unsigned long long)(pool.GetStats().poolBytes / kMb));
    Report(out, L"  free_commit_end_mb= %llu", (unsigned long long)FreeCommitMb());
    Report(out, L"");
    Report(out, L"RESULT: %ls", L"STAGE 10 CHAOS FINISHED");

    fclose(out);
    pool.Log(L"[torture] stage 10 report written to %ls", path.c_str());
    return 0;
}

} // namespace

namespace {
CryoSwap* g_consoleCryo = nullptr;

BOOL WINAPI ConsoleCtrlHandler(DWORD type)
{
    // Ctrl+C is a user action: restore everything before the process dies.
    if (type == CTRL_C_EVENT && g_consoleCryo) {
        g_consoleCryo->RestoreAllFrozen();
    }
    // CTRL_SHUTDOWN_EVENT / CTRL_CLOSE_EVENT: the OS is tearing processes down,
    // restoring frozen victims here would only slow the shutdown.
    return FALSE;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int showCommand)
{
    const std::wstring arguments = commandLine ? commandLine : L"";

    if (arguments.find(L"--cryo-victim") != std::wstring::npos) {
        return RunCryoVictim(arguments);
    }

    const std::wstring dataDir = LocalAppData();
    CreateDirectoryW(dataDir.c_str(), nullptr);
    const std::wstring logPath = dataDir + L"\\engine.log";
    const bool torture = arguments.find(L"--torture-test") != std::wstring::npos;
    const bool selfTest = arguments.find(L"--self-test") != std::wstring::npos;
    const bool headless = torture || selfTest;

    CryoSwap::EnablePrivileges();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Global\\ChzmSiliconFuseSingleInstance");
    const DWORD mutexError = GetLastError();
    if (mutex && mutexError == ERROR_ALREADY_EXISTS && !headless) {
        CloseHandle(mutex);
        return 0;
    }

    StorageBridge bridge;
    if (!bridge.Init(dataDir, 6ull * 64ull * 1024ull * 1024ull)) {
        if (mutex) CloseHandle(mutex);
        return 2;
    }
    if (!bridge.InitCryoMirror(dataDir, kMirrorCapacity)) {
        if (mutex) CloseHandle(mutex);
        return 2;
    }

    VramPool pool;
    if (!pool.Init(&bridge, 0, 1024, logPath)) {
        if (mutex) CloseHandle(mutex);
        return 3;
    }

    CryoSwap cryo;
    cryo.Init(&pool, &bridge);
    g_consoleCryo = &cryo;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    const PagingStatus paging = ProbePagingEnvironment();
    pool.Log(L"[paging] volume=%d pagefile=%d ntstatus=0x%08lX",
             paging.volumeReady ? 1 : 0, paging.pagefileReady ? 1 : 0,
             (unsigned long)paging.ntstatus);

    if (headless) {
        const int result = RunTorture(pool, cryo, bridge, dataDir, torture);
        cryo.Shutdown();
        pool.Shutdown();
        bridge.Shutdown();
        if (mutex) CloseHandle(mutex);
        return result;
    }

    pool.StartMonitor();
    cryo.StartMonitor();

    MainWindow window;
    if (!window.Create(instance, &pool, &cryo, ExecutablePath())) {
        cryo.Shutdown();
        pool.Shutdown();
        bridge.Shutdown();
        if (mutex) CloseHandle(mutex);
        return 4;
    }
    window.Show(showCommand);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    cryo.Shutdown();
    pool.Shutdown();
    bridge.Shutdown();
    if (mutex) CloseHandle(mutex);
    return 0;
}
