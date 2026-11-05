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
        return docs + L"\\STAGE9_VALIDATION_REPORT.txt";
    }
    return LocalAppData() + L"\\STAGE9_VALIDATION_REPORT.txt";
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

int RunTorture(VramPool& pool, CryoSwap& cryo, const std::wstring& dataDir, bool full)
{
    const std::wstring path = ReportPath();
    FILE* out = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (!out) return 6;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    Report(out, L"CHZM//SILICONFUSE  STAGE 9  VALIDATION REPORT");
    Report(out, L"generated=%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth,
           now.wDay, now.wHour, now.wMinute, now.wSecond);
    Report(out, L"mode=%ls", full ? L"torture (fail-safe validation)" : L"quick");
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
    Report(out, L"  mirror_capacity = %llu MB (disk-backed fail-safe)",
           (unsigned long long)(kMirrorCapacity / kMb));
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
    double dmaMbps = 0.0;
    const bool dmaOk = pool.ReadCryoDmaInto(0, 64ull * kMb, bench.data(), dmaMbps);
    Report(out, dmaOk ? L"  gpu_dma_read_mbps = %.1f" : L"  gpu_dma_read_mbps = FAILED",
           dmaOk ? dmaMbps : 0.0);
    benchTimer.Begin();
    const uint64_t hashValue = Hash64::Compute(bench.data(), bench.size());
    const double hashMs = benchTimer.Ms();
    Report(out, L"  avx2_hash_ms_64mb = %.2f (%.1f GB/s, hash=%llu)", hashMs,
           hashMs > 0.0 ? (double)bench.size() / (1024.0 * 1024.0 * 1024.0) /
                              (hashMs / 1000.0)
                        : 0.0,
           (unsigned long long)hashValue);
    Report(out, L"");

    // ---------------------------------------------------------- TEST A -----
    Report(out, L"[TEST A: EXACT TIMER ACCOUNTING (512 MB & 1024 MB)]");
    if (full) {
        const uint64_t sizes[] = { 512, 1024 };
        for (uint64_t sizeMb : sizes) {
            VictimHandle victim;
            Report(out, L"  --- %llu MB ---", (unsigned long long)sizeMb);
            if (!StartVictim(sizeMb, 3, victim)) {
                Report(out, L"  victim_spawn = FAILED");
                StopVictim(victim);
                continue;
            }
            FreezeResult freeze;
            const bool frozen = cryo.CryoFreeze(victim.marker.pid, freeze);
            Report(out, L"  frozen = %d", frozen ? 1 : 0);
            ReportFreezePhases(out, sizeMb, freeze);

            uint64_t zeroedMb = 0;
            ZeroFrozenProcess(cryo, victim.marker.pid, zeroedMb);
            Report(out, L"  zeroed_mb = %llu", (unsigned long long)zeroedMb);

            RestoreResult restore;
            const bool restored = cryo.CryoRestore(victim.marker.pid, true, restore);
            Report(out, L"  restored = %d (mirror regions %d)", restored ? 1 : 0,
                   restore.mirrorRegions);
            ReportRestorePhases(out, restore);

            if (victim.verify) SetEvent(victim.verify);
            const double percent = QueryVictimMatch(sizeMb);
            Report(out, L"  victim_bit_match = %.4f%%", percent);
            Report(out, L"");
            StopVictim(victim);
            cryo.Reset();
        }
    }
    Report(out, L"");

    // ---------------------------------------------------------- TEST B -----
    Report(out, L"[TEST B: MULTI-PROCESS CONCURRENT CRYOSWAP (3 VICTIMS)]");
    if (full) {
        struct Spec { uint64_t sizeMb; uint32_t seed; };
        const Spec specs[] = { { 64, 11 }, { 128, 22 }, { 256, 33 } };
        VictimHandle victims[3];

        for (int i = 0; i < 3; ++i) {
            StartVictim(specs[i].sizeMb, specs[i].seed, victims[i]);
        }

        const uint64_t availBefore = AvailablePhysicalMb();
        double totalFrozenMs = 0.0;
        int frozen = 0;
        for (int i = 0; i < 3; ++i) {
            if (!victims[i].alive) continue;
            FreezeResult freeze;
            if (cryo.CryoFreeze(victims[i].marker.pid, freeze)) {
                ++frozen;
                totalFrozenMs += freeze.totalMs;
            }
        }

        uint64_t zeroedTotal = 0;
        for (int i = 0; i < 3; ++i) {
            if (!victims[i].alive) continue;
            uint64_t zeroed = 0;
            ZeroFrozenProcess(cryo, victims[i].marker.pid, zeroed);
            zeroedTotal += zeroed;
        }
        const uint64_t availAfter = AvailablePhysicalMb();

        Report(out, L"  victims_frozen   = %d / 3", frozen);
        Report(out, L"  freeze_total_ms  = %.2f", totalFrozenMs);
        Report(out, L"  zeroed_total_mb  = %llu", (unsigned long long)zeroedTotal);
        Report(out, L"  avail_ram_before = %llu MB", (unsigned long long)availBefore);
        Report(out, L"  avail_ram_after  = %llu MB", (unsigned long long)availAfter);
        Report(out, L"  avail_ram_delta  = %lld MB",
               (long long)availAfter - (long long)availBefore);
        Report(out, L"");

        // Restore in a deliberately shuffled order (256, 64, 128).
        const int order[3] = { 2, 0, 1 };
        for (int idx : order) {
            if (!victims[idx].alive) continue;
            RestoreResult restore;
            const bool restored = cryo.CryoRestore(victims[idx].marker.pid, true, restore);
            if (victims[idx].verify) SetEvent(victims[idx].verify);
            const double percent = QueryVictimMatch(specs[idx].sizeMb);
            Report(out, L"  victim %4llu MB: restored=%d total=%.2f ms dma=%.2f wpm=%.2f "
                        L"bit_match=%.4f%%",
                   (unsigned long long)specs[idx].sizeMb, restored ? 1 : 0,
                   restore.totalMs, restore.dmaGpuMs, restore.wpmWriteMs, percent);
            StopVictim(victims[idx]);
        }
        for (int i = 0; i < 3; ++i) StopVictim(victims[i]);
        cryo.Reset();
    }
    Report(out, L"");

    // ---------------------------------------------------------- TEST C -----
    Report(out, L"[TEST C: VRAM EVICTION SURVIVAL (FAULT-TOLERANCE PROOF)]");
    if (full) {
        VictimHandle victim;
        if (StartVictim(256, 7, victim)) {
            FreezeResult freeze;
            const bool frozen = cryo.CryoFreeze(victim.marker.pid, freeze);
            Report(out, L"  frozen           = %d (%llu MB, %d regions)",
                   frozen ? 1 : 0, (unsigned long long)(freeze.bytes / kMb),
                   freeze.regions);

            uint64_t zeroedMb = 0;
            ZeroFrozenProcess(cryo, victim.marker.pid, zeroedMb);
            Report(out, L"  zeroed_mb        = %llu", (unsigned long long)zeroedMb);

            double evictMs = 0.0;
            const bool evicted = pool.EvictAllSlices(evictMs);
            Report(out, L"  vram_evicted     = %d (%.2f ms, all %d slices released)",
                   evicted ? 1 : 0, evictMs, pool.GetStats().sliceCount);
            PoolStats afterEvict = pool.GetStats();
            Report(out, L"  resident_slices  = %d / %d after eviction",
                   afterEvict.residentSlices, afterEvict.sliceCount);

            RestoreResult restore;
            const bool restored = cryo.CryoRestore(victim.marker.pid, true, restore);
            Report(out, L"  restored         = %d", restored ? 1 : 0);
            Report(out, L"  mirror_regions   = %d (%llu MB read from StorageBridge)",
                   restore.mirrorRegions, (unsigned long long)(restore.mirrorBytes / kMb));
            Report(out, L"  dma_regions      = %d", restore.dmaRegions);
            Report(out, L"  checksum         = %ls", restore.checksumOk ? L"MATCH" : L"MISMATCH");
            Report(out, L"  fallback_ms      = %.2f", restore.fallbackMs);
            Report(out, L"  total_ms         = %.2f", restore.totalMs);

            if (victim.verify) SetEvent(victim.verify);
            const double percent = QueryVictimMatch(256);
            Report(out, L"  victim_bit_match = %.4f%%", percent);
            StopVictim(victim);
            cryo.Reset();

            // The slices are gone after the fault injection; rebuild them so
            // the remaining tests start from a healthy pool.
            pool.RequestResize(1024);
            Sleep(200);
            pool.RequestResize(4096);
            Sleep(300);
            Report(out, L"  pool_rebuilt     = %d slices resident",
                   pool.GetStats().residentSlices);
        } else {
            Report(out, L"  victim_spawn = FAILED");
            StopVictim(victim);
        }
    }
    Report(out, L"");

    // ---------------------------------------------------------- TEST 6 -----
    Report(out, L"[TEST 6: COMMITGUARD]");
    Report(out, L"  free_commit_mb  = %llu (floor %llu)", (unsigned long long)FreeCommitMb(),
           (unsigned long long)kCommitGuardFloorMb);
    pool.RequestResize(2048);
    Sleep(300);
    const int downSlices = pool.GetStats().sliceCount;
    pool.RequestResize(4096);
    Sleep(500);
    Report(out, L"  resize_2048_mb  = %d slices", downSlices);
    Report(out, L"  resize_4096_mb  = %d slices", pool.GetStats().sliceCount);
    Report(out, L"");

    // ---------------------------------------------------------- TEST 7 -----
    Report(out, L"[TEST 7: SENSORS + STANDBY PURGE + EXIT SAFETY]");
    PoolStats sensors = pool.GetStats();
    const bool purgeOk = PurgeLowPriorityStandbyList();
    Report(out, L"  nvml_fb_used_mb   = %llu / %llu",
           (unsigned long long)(sensors.fbUsedBytes / kMb),
           (unsigned long long)(sensors.fbTotalBytes / kMb));
    Report(out, L"  nvml_bar1_used_mb = %llu / %llu",
           (unsigned long long)(sensors.bar1UsedBytes / kMb),
           (unsigned long long)(sensors.bar1TotalBytes / kMb));
    Report(out, L"  dxgi_local_used_mb= %llu", (unsigned long long)(sensors.usageBytes / kMb));
    Report(out, L"  standby_purge     = %ls", purgeOk ? L"ok" : L"unavailable (needs admin)");
    Report(out, L"  frozen_processes  = %d", cryo.GetStats().frozen);
    Report(out, L"  mirror_bytes_used = %llu MB",
           (unsigned long long)(pool.Bridge() ? pool.Bridge()->MirrorBytes() / kMb : 0));
    Report(out, L"  pool_end_mb       = %llu",
           (unsigned long long)(pool.GetStats().poolBytes / kMb));
    Report(out, L"");

    // ---------------------------------------------------------- TEST 8 -----
    Report(out, L"[TEST 8: EXIT SAFETY (RestoreAllFrozen)]");
    if (full) {
        VictimHandle victim;
        if (StartVictim(64, 9, victim)) {
            FreezeResult freeze;
            const bool frozen = cryo.CryoFreeze(victim.marker.pid, freeze);
            const int restoredAll = cryo.RestoreAllFrozen();
            const bool stillRunning =
                WaitForSingleObject(victim.child.hProcess, 0) != WAIT_OBJECT_0;
            if (victim.verify) SetEvent(victim.verify);
            const double percent = QueryVictimMatch(64);
            Report(out, L"  frozen           = %d", frozen ? 1 : 0);
            Report(out, L"  restore_all_frozen = %d", restoredAll);
            Report(out, L"  process_alive    = %d (resumed by the exit path)",
                   stillRunning ? 1 : 0);
            Report(out, L"  frozen_left      = %d", cryo.GetStats().frozen);
            Report(out, L"  victim_bit_match = %.4f%%", percent);
            StopVictim(victim);
            cryo.Reset();
        } else {
            Report(out, L"  victim_spawn = FAILED");
            StopVictim(victim);
        }
    }
    Report(out, L"");
    Report(out, L"RESULT: %ls", L"STAGE 9 VALIDATION FINISHED");

    fclose(out);
    pool.Log(L"[torture] stage 9 report written to %ls", path.c_str());
    return 0;
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

    const PagingStatus paging = ProbePagingEnvironment();
    pool.Log(L"[paging] volume=%d pagefile=%d ntstatus=0x%08lX",
             paging.volumeReady ? 1 : 0, paging.pagefileReady ? 1 : 0,
             (unsigned long)paging.ntstatus);

    if (headless) {
        const int result = RunTorture(pool, cryo, dataDir, torture);
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
