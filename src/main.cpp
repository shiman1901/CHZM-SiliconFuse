#include <windows.h>
#include <psapi.h>
#include <shlobj.h>

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

uint8_t PatternByte(uint64_t index) { return (uint8_t)((index * 167ull + 13ull) & 0xFF); }

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
        return docs + L"\\STAGE8_BENCHMARK_MATRIX.txt";
    }
    return LocalAppData() + L"\\STAGE8_BENCHMARK_MATRIX.txt";
}

std::wstring VictimStatePath() { return LocalAppData() + L"\\victim_state.txt"; }
std::wstring VictimResultPath() { return LocalAppData() + L"\\victim_result.txt"; }

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

// Hashes a byte range of another process through ReadProcessMemory (AVX2 hash).
bool HashRemoteRange(DWORD pid, uint64_t address, uint64_t bytes, uint64_t& hash,
                     uint64_t& hashed)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) return false;

    std::vector<uint8_t> buffer((size_t)std::min(kHashChunk, bytes));
    Hash64 hasher;
    hashed = 0;
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
        hashed += read;
        offset += read;
    }
    hash = hasher.Final();
    CloseHandle(process);
    return true;
}

bool TouchRemoteRange(DWORD pid, uint64_t address, uint64_t bytes)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) return false;
    std::vector<uint8_t> buffer((size_t)std::min<uint64_t>(kHashChunk, bytes));
    uint64_t offset = 0;
    bool ok = true;
    while (offset < bytes) {
        const uint64_t chunk = std::min(bytes - offset, kHashChunk);
        SIZE_T read = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<const void*>(address + offset),
                               buffer.data(), (SIZE_T)chunk, &read) || read == 0) {
            ok = false;
            break;
        }
        offset += read;
    }
    CloseHandle(process);
    return ok;
}

// ---------------------------------------------------------------- victim ----

int RunCryoVictim(const std::wstring& arguments)
{
    uint64_t sizeMb = kVictimDefaultMb;
    const size_t flag = arguments.find(L"--cryo-victim");
    if (flag != std::wstring::npos) {
        const wchar_t* rest = arguments.c_str() + flag + wcslen(L"--cryo-victim");
        const long value = wcstol(rest, nullptr, 10);
        if (value > 0) sizeMb = (uint64_t)value;
    }
    const size_t total = (size_t)(sizeMb * kMb);

    auto* buffer = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buffer) return 5;

    for (size_t i = 0; i < total; ++i) buffer[i] = PatternByte(i);
    const uint64_t hash = Hash64::Compute(buffer, total);

    const std::wstring statePath = VictimStatePath();
    FILE* state = _wfsopen(statePath.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (state) {
        fwprintf(state, L"pid=%lu\n", (unsigned long)GetCurrentProcessId());
        fwprintf(state, L"base=%llu\n", (unsigned long long)(uintptr_t)buffer);
        fwprintf(state, L"size_mb=%llu\n", (unsigned long long)sizeMb);
        fwprintf(state, L"hash=%llu\n", (unsigned long long)hash);
        fclose(state);
    }

    HANDLE verify = CreateEventW(nullptr, TRUE, FALSE, L"Global\\ChzmVictimVerify2");
    if (!verify) return 6;

    if (WaitForSingleObject(verify, 180000) != WAIT_OBJECT_0) return 7;

    const uint64_t after = Hash64::Compute(buffer, total);
    size_t matching = 0;
    for (size_t i = 0; i < total; ++i) {
        if (buffer[i] == PatternByte(i)) ++matching;
    }
    const double percent = 100.0 * (double)matching / (double)total;

    FILE* result = _wfsopen(VictimResultPath().c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
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

// ------------------------------------------------------------- torture ------

struct VictimInfo {
    DWORD pid = 0;
    uint64_t base = 0;
    uint64_t sizeMb = 0;
    uint64_t hash = 0;
};

bool ParseVictimState(VictimInfo& info)
{
    FILE* file = _wfsopen(VictimStatePath().c_str(), L"r, ccs=UTF-8", _SH_DENYNO);
    if (!file) return false;
    wchar_t line[256];
    bool ok = false;
    while (fgetws(line, 256, file)) {
        unsigned long long value = 0;
        if (swscanf_s(line, L"pid=%llu", &value) == 1) info.pid = (DWORD)value;
        if (swscanf_s(line, L"base=%llu", &value) == 1) info.base = value;
        if (swscanf_s(line, L"size_mb=%llu", &value) == 1) info.sizeMb = value;
        if (swscanf_s(line, L"hash=%llu", &value) == 1) { info.hash = value; ok = true; }
    }
    fclose(file);
    return ok && info.pid != 0;
}

struct MatrixRow {
    uint64_t sizeMb = 0;
    // Mode A / B
    double wsBefore = 0.0;
    double commitMb = 0.0;
    double wsAfterEmpty = 0.0;
    uint64_t faultsBefore = 0;
    uint64_t faultsAfterTouch = 0;
    // Mode C
    double freezeTotalMs = 0.0;
    double suspendMs = 0.0;
    double rpmMs = 0.0;
    double vramMs = 0.0;
    double trimMs = 0.0;
    double freezeHashMs = 0.0;
    double wsAfter = 0.0;
    int64_t availDelta = 0;
    double dmaMs = 0.0;
    double hashMs = 0.0;
    double wpmMs = 0.0;
    double resumeMs = 0.0;
    double restoreMs = 0.0;
    double dmaMBps = 0.0;
    uint64_t origHash = 0;
    uint64_t zeroedHash = 0;
    uint64_t restoredHash = 0;
    uint64_t zeroedMb = 0;
    int regions = 0;
    bool bitMatch = false;
    bool checksum = false;
};

bool RunOneSize(VramPool& pool, CryoSwap& cryo, uint64_t sizeMb, MatrixRow& row,
                FILE* out)
{
    row.sizeMb = sizeMb;
    DeleteFileW(VictimStatePath().c_str());
    DeleteFileW(VictimResultPath().c_str());
    HANDLE verify = CreateEventW(nullptr, TRUE, FALSE, L"Global\\ChzmVictimVerify2");
    if (verify) ResetEvent(verify);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    std::wstring command = L"\"" + ExecutablePath() + L"\" --cryo-victim " +
                           std::to_wstring(sizeMb);
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &child)) {
        Report(out, L"  victim_spawn(size=%llu) = FAILED", (unsigned long long)sizeMb);
        if (verify) CloseHandle(verify);
        return false;
    }

    VictimInfo info;
    for (int i = 0; i < 100 && !ParseVictimState(info); ++i) Sleep(100);
    if (info.pid == 0) {
        Report(out, L"  victim_state(size=%llu) = TIMEOUT", (unsigned long long)sizeMb);
        TerminateProcess(child.hProcess, 0);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        if (verify) CloseHandle(verify);
        return false;
    }

    const uint64_t rangeBytes = info.sizeMb * kMb;

    // --- Mode A: baseline, hands off -------------------------------------
    ProcNumbers before = ReadProcessNumbers(info.pid);
    row.wsBefore = before.wsMb;
    row.commitMb = before.commitMb;
    row.faultsBefore = before.pageFaults;

    // --- Mode B: plain EmptyWorkingSet -----------------------------------
    HANDLE victim = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, info.pid);
    if (victim) {
        EmptyWorkingSet(victim);
        CloseHandle(victim);
    }
    ProcNumbers afterEmpty = ReadProcessNumbers(info.pid);
    row.wsAfterEmpty = afterEmpty.wsMb;

    TouchRemoteRange(info.pid, info.base, rangeBytes);
    ProcNumbers afterTouch = ReadProcessNumbers(info.pid);
    row.faultsAfterTouch = afterTouch.pageFaults;

    // --- Mode C: full CHZM cycle ----------------------------------------
    uint64_t origHash = 0, origHashed = 0;
    HashRemoteRange(info.pid, info.base, rangeBytes, origHash, origHashed);

    const uint64_t availBefore = AvailablePhysicalMb();
    FreezeResult freeze;
    const bool frozen = cryo.CryoFreeze(info.pid, freeze);
    row.freezeTotalMs = freeze.totalMs;
    row.suspendMs = freeze.suspendMs;
    row.rpmMs = freeze.rpmReadMs;
    row.vramMs = freeze.vramWriteMs;
    row.trimMs = freeze.resetTrimMs;
    row.freezeHashMs = freeze.hashMs;
    row.regions = freeze.regions;

    // Zero the physical pages, then discard them again.
    {
        HANDLE process = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                                         PROCESS_QUERY_LIMITED_INFORMATION,
                                     FALSE, info.pid);
        if (process) {
            std::vector<uint8_t> zeros((size_t)(16ull * kMb), 0);
            uint64_t offset = 0;
            while (offset < rangeBytes) {
                const uint64_t chunk = std::min(rangeBytes - offset,
                                                (uint64_t)zeros.size());
                SIZE_T wrote = 0;
                if (!WriteProcessMemory(process,
                                        reinterpret_cast<void*>(info.base + offset),
                                        zeros.data(), (SIZE_T)chunk, &wrote) ||
                    wrote == 0) {
                    break;
                }
                offset += wrote;
            }
            VirtualAllocEx(process, reinterpret_cast<void*>(info.base),
                           (SIZE_T)rangeBytes, MEM_RESET, PAGE_READWRITE);
            SetProcessWorkingSetSizeEx(process, 4ull * kMb, 24ull * kMb,
                                       QUOTA_LIMITS_HARDWS_MAX_ENABLE);
            EmptyWorkingSet(process);
            row.zeroedMb = offset / kMb;
            CloseHandle(process);
        }
    }
    PurgeLowPriorityStandbyList();

    ProcNumbers afterZero = ReadProcessNumbers(info.pid);
    row.wsAfter = afterZero.wsMb;
    row.availDelta = (int64_t)AvailablePhysicalMb() - (int64_t)availBefore;

    uint64_t zeroedHash = 0, zeroedHashed = 0;
    HashRemoteRange(info.pid, info.base, rangeBytes, zeroedHash, zeroedHashed);
    row.zeroedHash = zeroedHash;

    RestoreResult restore;
    const bool restored = cryo.CryoRestore(info.pid, true, restore);
    row.dmaMs = restore.dmaGpuMs;
    row.hashMs = restore.hashVerifyMs;
    row.wpmMs = restore.wpmWriteMs;
    row.resumeMs = restore.resumeMs;
    row.restoreMs = restore.totalMs;
    row.dmaMBps = restore.dmaMBps;
    row.checksum = restore.checksumOk;

    uint64_t restoredHash = 0, restoredHashed = 0;
    HashRemoteRange(info.pid, info.base, rangeBytes, restoredHash, restoredHashed);

    if (verify) SetEvent(verify);
    for (int i = 0; i < 200 && GetFileAttributesW(VictimResultPath().c_str()) ==
                                 INVALID_FILE_ATTRIBUTES; ++i) {
        Sleep(50);
    }
    bool hashEqual = false;
    FILE* result = _wfsopen(VictimResultPath().c_str(), L"r, ccs=UTF-8", _SH_DENYNO);
    if (result) {
        wchar_t line[256];
        while (fgetws(line, 256, result)) {
            unsigned long long value = 0;
            if (swscanf_s(line, L"hash_equal=%llu", &value) == 1) hashEqual = value != 0;
        }
        fclose(result);
    }

    row.origHash = origHash;
    row.restoredHash = restoredHash;
    row.bitMatch = hashEqual && restoredHash == origHash && zeroedHash != origHash;

    if (verify) CloseHandle(verify);
    TerminateProcess(child.hProcess, 0);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);

    pool.LogSensors(L"matrix-run");
    (void)frozen;
    (void)restored;
    return true;
}

int RunTorture(VramPool& pool, CryoSwap& cryo, const std::wstring& dataDir, bool full)
{
    const std::wstring path = ReportPath();
    FILE* out = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (!out) return 6;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    Report(out, L"CHZM//SILICONFUSE  STAGE 8  BENCHMARK MATRIX");
    Report(out, L"generated=%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth,
           now.wDay, now.wHour, now.wMinute, now.wSecond);
    Report(out, L"mode=%ls", full ? L"torture (matrix 64/128/256/512 MB)" : L"quick");
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
    Report(out, L"");

    // ---------------------------------------------------------- TEST 2 -----
    PoolStats stats = pool.GetStats();
    Report(out, L"[TEST 2: POOL + COMMITGUARD + RAW BANDWIDTH]");
    Report(out, L"  adapter_vram_mb = %llu", (unsigned long long)(stats.adapterVramBytes / kMb));
    Report(out, L"  pool_mb         = %llu", (unsigned long long)(stats.poolBytes / kMb));
    Report(out, L"  slices          = %d x %llu MB", stats.sliceCount,
           (unsigned long long)(pool.SliceBytes() / kMb));
    Report(out, L"  commit_free_mb  = %llu (guard floor %llu)",
           (unsigned long long)FreeCommitMb(), (unsigned long long)kCommitGuardFloorMb);

    std::vector<uint8_t> bench((size_t)(64ull * kMb), 0x5A);
    Timer benchTimer;
    benchTimer.Begin();
    const bool streamOk = cryo.StreamWrite(0, bench.data(), bench.size());
    const double streamMs = benchTimer.Ms();
    const double streamMbps = streamOk && streamMs > 0.0
                                  ? (double)bench.size() / (1024.0 * 1024.0) /
                                        (streamMs / 1000.0)
                                  : 0.0;
    Report(out, L"  avx2_vram_write_mbps = %.1f (%.2f ms / 64 MB)", streamMbps, streamMs);

    double dmaMbps = 0.0;
    const bool dmaOk = pool.ReadCryoDmaInto(0, 64ull * kMb, bench.data(), dmaMbps);
    Report(out, dmaOk ? L"  gpu_dma_read_mbps = %.1f" : L"  gpu_dma_read_mbps = FAILED",
           dmaOk ? dmaMbps : 0.0);

    benchTimer.Begin();
    const uint64_t hash64 = Hash64::Compute(bench.data(), bench.size());
    const double hashMs = benchTimer.Ms();
    Report(out, L"  avx2_hash_ms_64mb = %.2f (%.1f GB/s, hash=%llu)", hashMs,
           hashMs > 0.0 ? (double)bench.size() / (1024.0 * 1024.0 * 1024.0) /
                              (hashMs / 1000.0)
                        : 0.0,
           (unsigned long long)hash64);
    Report(out, L"  projected_512mb_hash_ms = %.2f", hashMs * 8.0);
    Report(out, L"");

    // ---------------------------------------------------------- TEST 3-5 ---
    std::vector<MatrixRow> rows;
    if (full) {
        const uint64_t sizes[] = { 64, 128, 256, 512 };
        for (uint64_t sizeMb : sizes) {
            MatrixRow row;
            if (RunOneSize(pool, cryo, sizeMb, row, out)) rows.push_back(row);
        }
    }

    Report(out, L"[TEST 3: MODE A — BASELINE WINDOWS (no intervention)]");
    Report(out, L"  Size_MB | WS_Before_MB | PrivateCommit_MB | Saved_RAM_MB");
    Report(out, L"  --------+--------------+------------------+-------------");
    for (const MatrixRow& row : rows) {
        Report(out, L"  %7llu | %12.1f | %16.1f | %12d",
               (unsigned long long)row.sizeMb, row.wsBefore, row.commitMb, 0);
    }
    Report(out, L"");

    Report(out, L"[TEST 4: MODE B — PLAIN EmptyWorkingSet (no GDDR6)]");
    Report(out, L"  Size_MB | WS_Before_MB | WS_After_MB | PageFaults_Before | "
                L"PageFaults_After_Touch | Fault_Delta");
    Report(out, L"  --------+--------------+-------------+-------------------+"
                L"------------------------+------------");
    for (const MatrixRow& row : rows) {
        Report(out, L"  %7llu | %12.1f | %11.1f | %17llu | %22llu | %11lld",
               (unsigned long long)row.sizeMb, row.wsBefore, row.wsAfterEmpty,
               (unsigned long long)row.faultsBefore,
               (unsigned long long)row.faultsAfterTouch,
               (long long)row.faultsAfterTouch - (long long)row.faultsBefore);
    }
    Report(out, L"");

    Report(out, L"[TEST 5: MODE C — CHZM CRYOSWAP GDDR6]");
    Report(out, L"  Size_MB | WS_Before | WS_After | AvailRAM_Delta | DMA_ms | Hash_ms | "
                L"WPM_ms | Total_Restore_ms | BitMatch");
    Report(out, L"  --------+-----------+----------+----------------+--------+---------+"
                L"--------+------------------+---------");
    for (const MatrixRow& row : rows) {
        Report(out, L"  %7llu | %9.1f | %8.1f | %14lld | %6.1f | %7.2f | %6.1f | "
                    L"%16.2f | %ls",
               (unsigned long long)row.sizeMb, row.wsBefore, row.wsAfter,
               (long long)row.availDelta, row.dmaMs, row.hashMs, row.wpmMs,
               row.restoreMs, row.bitMatch ? L"100%" : L"FAIL");
    }
    Report(out, L"");

    for (const MatrixRow& row : rows) {
        Report(out, L"  --- SIZE %llu MB ---", (unsigned long long)row.sizeMb);
        Report(out, L"  freeze_total_ms = %.2f (suspend %.2f | rpm %.2f | avx2_vram %.2f | "
                    L"hash %.2f | trim %.2f)",
               row.freezeTotalMs, row.suspendMs, row.rpmMs, row.vramMs,
               row.freezeHashMs, row.trimMs);
        Report(out, L"  restore_total_ms= %.2f (dma %.2f | hash %.2f | wpm %.2f | resume %.2f)",
               row.restoreMs, row.dmaMs, row.hashMs, row.wpmMs, row.resumeMs);
        Report(out, L"  regions         = %d | dma_peak_mbps = %.1f", row.regions, row.dmaMBps);
        Report(out, L"  zeroed_mb       = %llu", (unsigned long long)row.zeroedMb);
        Report(out, L"  orig_hash       = %llu", (unsigned long long)row.origHash);
        Report(out, L"  zeroed_hash     = %llu (differs=%d)", (unsigned long long)row.zeroedHash,
               row.zeroedHash != row.origHash ? 1 : 0);
        Report(out, L"  restored_hash   = %llu (equal=%d)", (unsigned long long)row.restoredHash,
               row.restoredHash == row.origHash ? 1 : 0);
        Report(out, L"  gddr6_checksum  = %ls", row.checksum ? L"MATCH" : L"MISMATCH");
        Report(out, L"");
    }

    // ---------------------------------------------------------- TEST 6 -----
    Report(out, L"[TEST 6: COMMITGUARD]");
    const uint64_t commitFree = FreeCommitMb();
    Report(out, L"  free_commit_mb  = %llu (floor %llu)", (unsigned long long)commitFree,
           (unsigned long long)kCommitGuardFloorMb);
    pool.RequestResize(2048);
    Sleep(300);
    const int downSlices = pool.GetStats().sliceCount;
    pool.RequestResize(4096);
    Sleep(500);
    const int upSlices = pool.GetStats().sliceCount;
    Report(out, L"  resize_2048_mb  = %d slices", downSlices);
    Report(out, L"  resize_4096_mb  = %d slices", upSlices);
    Report(out, L"");

    // ---------------------------------------------------------- TEST 7 -----
    Report(out, L"[TEST 7: SENSORS + STANDBY PURGE]");
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
    Report(out, L"  pool_end_mb       = %llu",
           (unsigned long long)(pool.GetStats().poolBytes / kMb));
    Report(out, L"  free_commit_end_mb= %llu", (unsigned long long)FreeCommitMb());
    Report(out, L"");
    Report(out, L"RESULT: %ls", L"STAGE 8 MATRIX FINISHED");

    fclose(out);
    pool.Log(L"[torture] stage 8 report written to %ls", path.c_str());
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

    VramPool pool;
    if (!pool.Init(&bridge, 0, 1024, logPath)) {
        if (mutex) CloseHandle(mutex);
        return 3;
    }

    CryoSwap cryo;
    cryo.Init(&pool);

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
