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
#include "engine/PagingManager.h"
#include "engine/StandbyPurge.h"
#include "engine/StorageBridge.h"
#include "engine/VramPool.h"
#include "gui/MainWindow.h"

namespace {

constexpr uint64_t kMb = 1024ull * 1024ull;
constexpr uint64_t kVictimDefaultMb = 512;

uint64_t Fnv1a(uint64_t hash, const uint8_t* data, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= (uint64_t)data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Deterministic test pattern: any surviving byte is verifiable.
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
        return docs + L"\\STAGE7_TORTURE_REPORT.txt";
    }
    return LocalAppData() + L"\\STAGE7_TORTURE_REPORT.txt";
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

double WorkingSetMb(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return -1.0;
    PROCESS_MEMORY_COUNTERS counters{};
    const bool ok = GetProcessMemoryInfo(process, &counters, sizeof(counters)) != FALSE;
    CloseHandle(process);
    return ok ? (double)counters.WorkingSetSize / (1024.0 * 1024.0) : -1.0;
}

double PrivateCommitMb(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return -1.0;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    const bool ok = GetProcessMemoryInfo(process,
                                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                         sizeof(counters)) != FALSE;
    CloseHandle(process);
    return ok ? (double)counters.PrivateUsage / (1024.0 * 1024.0) : -1.0;
}

// ---------------------------------------------------------------- victim ----

// --cryo-victim [MB]: allocates private pages, fills them with the test
// pattern and waits for the verification event. It stays in the foreground of
// nothing, holds no window, so CryoSwap may touch it.
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
    uint64_t hash = 1469598103934665603ull;
    hash = Fnv1a(hash, buffer, total);

    const std::wstring statePath = VictimStatePath();
    FILE* state = _wfsopen(statePath.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (state) {
        fwprintf(state, L"pid=%lu\n", (unsigned long)GetCurrentProcessId());
        fwprintf(state, L"base=%llu\n", (unsigned long long)(uintptr_t)buffer);
        fwprintf(state, L"size_mb=%llu\n", (unsigned long long)sizeMb);
        fwprintf(state, L"hash=%llu\n", (unsigned long long)hash);
        fclose(state);
    }

    HANDLE verify = CreateEventW(nullptr, TRUE, FALSE, L"Global\\ChzmVictimVerify");
    if (!verify) return 6;

    const DWORD wait = WaitForSingleObject(verify, 180000);
    if (wait != WAIT_OBJECT_0) return 7;

    uint64_t after = 1469598103934665603ull;
    after = Fnv1a(after, buffer, total);

    size_t matching = 0;
    for (size_t i = 0; i < total; ++i) {
        if (buffer[i] == PatternByte(i)) ++matching;
    }
    const double percent = 100.0 * (double)matching / (double)total;

    const std::wstring resultPath = VictimResultPath();
    FILE* result = _wfsopen(resultPath.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
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

bool ZeroVictimPages(DWORD pid, const std::vector<CachedProcess>& frozen,
                     const std::vector<uint8_t>& zeros, uint64_t& zeroed)
{
    HANDLE process = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                                     PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, pid);
    if (!process) return false;
    zeroed = 0;
    for (const CachedProcess& cached : frozen) {
        if (cached.pid != pid) continue;
        for (const CachedRegion& region : cached.regions) {
            uint64_t offset = 0;
            while (offset < region.size) {
                const uint64_t chunk = (uint64_t)zeros.size() < region.size - offset
                                           ? (uint64_t)zeros.size()
                                           : region.size - offset;
                SIZE_T wrote = 0;
                if (!WriteProcessMemory(process,
                                        reinterpret_cast<void*>(region.address + offset),
                                        zeros.data(), (SIZE_T)chunk, &wrote)) {
                    CloseHandle(process);
                    return false;
                }
                zeroed += wrote;
                offset += wrote;
                if (wrote == 0) break;
            }
        }
    }
    CloseHandle(process);
    return true;
}

int RunTorture(VramPool& pool, CryoSwap& cryo, const std::wstring& dataDir, bool full)
{
    const std::wstring path = ReportPath();
    FILE* out = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (!out) return 6;

    Report(out, L"CHZM//SILICONFUSE  STAGE 7  TORTURE REPORT");
    SYSTEMTIME now{};
    GetLocalTime(&now);
    Report(out, L"generated=%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth,
           now.wDay, now.wHour, now.wMinute, now.wSecond);
    Report(out, L"mode=%ls", full ? L"torture (512 MB proof)" : L"self-test (quick)");
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
    Report(out, L"  exe_path        = %ls", exePath);
    Report(out, L"  exe_size_bytes  = %llu", (unsigned long long)exeBytes);
    Report(out, L"  rt_group_icon   = %ls", iconGroup ? L"PRESENT (id 1)" : L"MISSING");
    Report(out, L"");

    // ---------------------------------------------------------- TEST 2 -----
    PoolStats stats = pool.GetStats();
    Report(out, L"[TEST 2: COMMITGUARD + VRAM POOL]");
    Report(out, L"  adapter_vram_mb = %llu", (unsigned long long)(stats.adapterVramBytes / kMb));
    Report(out, L"  pool_mb         = %llu", (unsigned long long)(stats.poolBytes / kMb));
    Report(out, L"  slices          = %d x %llu MB", stats.sliceCount,
           (unsigned long long)(pool.SliceBytes() / kMb));
    Report(out, L"  commit_free_mb  = %llu (guard floor %llu)",
           (unsigned long long)FreeCommitMb(), (unsigned long long)kCommitGuardFloorMb);
    Report(out, L"  commit_limit_mb = %llu", (unsigned long long)TotalCommitLimitMb());

    std::vector<uint8_t> bench((size_t)(64ull * kMb), 0x5A);
    Timer benchTimer;
    benchTimer.Begin();
    const bool streamOk = cryo.StreamWrite(0, bench.data(), bench.size());
    const double streamMs = benchTimer.Ms();
    const double streamMbps =
        streamOk && streamMs > 0.0
            ? (double)bench.size() / (1024.0 * 1024.0) / (streamMs / 1000.0) : 0.0;
    Report(out, L"  avx2_ingress_mbps = %.1f (%.2f ms for 64 MB)", streamMbps, streamMs);

    double dmaMbps = 0.0;
    const bool dmaOk = pool.ReadCryoDmaInto(0, 64ull * kMb, bench.data(), dmaMbps);
    Report(out, dmaOk ? L"  dma_egress_mbps = %.1f" : L"  dma_egress_mbps = FAILED",
           dmaOk ? dmaMbps : 0.0);
    Report(out, L"");

    // ---------------------------------------------------------- TEST 3 -----
    DWORD victimPid = 0;
    double victimRestoreMs = 0.0;
    double victimMatch = 0.0;
    bool victimHashEqual = false;

    if (full) {
        DeleteFileW(VictimStatePath().c_str());
        DeleteFileW(VictimResultPath().c_str());
        HANDLE verify = CreateEventW(nullptr, TRUE, FALSE, L"Global\\ChzmVictimVerify");
        if (verify) ResetEvent(verify);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        std::wstring command = L"\"" + ExecutablePath() + L"\" --cryo-victim 512";
        Report(out, L"[TEST 3: 512 MB END-TO-END GDDR6 PROOF]");

        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &startup, &child)) {
            Report(out, L"  victim_spawn = FAILED");
        } else {
            VictimInfo info;
            for (int i = 0; i < 100 && !ParseVictimState(info); ++i) Sleep(100);
            victimPid = info.pid;
            Report(out, L"  --- STEP 1 (BASELINE) ---");
            Report(out, L"  victim_pid      = %lu", (unsigned long)info.pid);
            Report(out, L"  victim_alloc_mb = %llu (MEM_COMMIT|MEM_PRIVATE)", (unsigned long long)info.sizeMb);
            Report(out, L"  victim_hash     = %llu", (unsigned long long)info.hash);
            Report(out, L"  victim_ws_mb    = %.1f", WorkingSetMb(info.pid));
            Report(out, L"  victim_commit_mb= %.1f", PrivateCommitMb(info.pid));

            const uint64_t availBefore = AvailablePhysicalMb();
            const uint64_t availBeforeFreeze = availBefore;

            Timer freezeTimer;
            freezeTimer.Begin();
            FreezeResult freeze;
            const bool frozen = cryo.CryoFreeze(info.pid, freeze);

            Report(out, L"  --- STEP 2 (INGRESS 512 MB -> GDDR6) ---");
            Report(out, L"  frozen          = %d", frozen ? 1 : 0);
            Report(out, L"  gddr6_bytes_mb  = %llu", (unsigned long long)(freeze.bytes / kMb));
            Report(out, L"  regions         = %d", freeze.regions);
            Report(out, L"  threads_frozen  = %d", freeze.threads);
            Report(out, L"  ingress_ms      = %.2f (copy loop only)", freeze.ingressMs);
            Report(out, L"  freeze_total_ms = %.2f (suspend + MEM_RESET + quota)",
                   freeze.totalMs);
            Report(out, L"  ingress_gbps    = %.2f",
                   freeze.ingressMs > 0.0
                       ? (double)freeze.bytes / (1024.0 * 1024.0 * 1024.0) /
                             (freeze.ingressMs / 1000.0)
                       : 0.0);
            pool.LogSensors(L"after-ingress");
            PoolStats afterIngress = pool.GetStats();
            Report(out, L"  nvml_fb_used_mb = %llu", (unsigned long long)(afterIngress.fbUsedBytes / kMb));
            Report(out, L"  nvml_bar1_mb    = %llu / %llu",
                   (unsigned long long)(afterIngress.bar1UsedBytes / kMb),
                   (unsigned long long)(afterIngress.bar1TotalBytes / kMb));
            Report(out, L"  dxgi_used_mb    = %llu", (unsigned long long)(afterIngress.usageBytes / kMb));

            // STEP 3: prove nothing survives in DDR4 or pagefile.
            Report(out, L"  --- STEP 3 (EVICT + ZERO PHYSICAL DDR4) ---");
            std::vector<uint8_t> zeros((size_t)(16ull * kMb), 0);
            uint64_t zeroed = 0;
            const bool zeroOk = ZeroVictimPages(info.pid, cryo.SnapshotFrozen(), zeros, zeroed);
            Report(out, L"  zeroed_mb       = %llu (write succeeded=%d)",
                   (unsigned long long)(zeroed / kMb), zeroOk ? 1 : 0);

            // MEM_RESET_UNDO sub-test: with the pages overwritten and discarded
            // there is nothing left to recover.
            HANDLE victimHandle = OpenProcess(PROCESS_VM_OPERATION | PROCESS_QUERY_LIMITED_INFORMATION,
                                              FALSE, info.pid);
            bool undoOk = false;
            if (victimHandle) {
                std::vector<CachedProcess> snapshot = cryo.SnapshotFrozen();
                for (const CachedProcess& cached : snapshot) {
                    if (cached.pid != info.pid || cached.regions.empty()) continue;
                    void* undone = VirtualAllocEx(
                        victimHandle,
                        reinterpret_cast<void*>(cached.regions.front().address),
                        (SIZE_T)cached.regions.front().size, MEM_RESET_UNDO, PAGE_READWRITE);
                    undoOk = undone != nullptr;
                    break;
                }
                CloseHandle(victimHandle);
            }
            Report(out, L"  mem_reset_undo  = %ls", undoOk ? L"RESTORED (page survived)" : L"NULL (must use GDDR6)");

            const uint64_t availAfterZero = AvailablePhysicalMb();
            Report(out, L"  victim_ws_mb    = %.1f", WorkingSetMb(info.pid));
            Report(out, L"  victim_commit_mb= %.1f", PrivateCommitMb(info.pid));
            Report(out, L"  avail_ram_before_mb = %llu", (unsigned long long)availBeforeFreeze);
            Report(out, L"  avail_ram_after_mb  = %llu", (unsigned long long)availAfterZero);
            Report(out, L"  avail_ram_delta_mb  = %lld",
                   (long long)availAfterZero - (long long)availBeforeFreeze);

            const bool purged = PurgeLowPriorityStandbyList();
            Report(out, L"  standby_purge   = %ls", purged ? L"ok" : L"unavailable");

            // STEP 4: honest egress through the copy engine.
            Report(out, L"  --- STEP 4 (EGRESS GDDR6 -> DMA -> WriteProcessMemory) ---");
            RestoreResult restore;
            const bool restored = cryo.CryoRestore(info.pid, true, restore);
            victimRestoreMs = restore.totalMs;
            Report(out, L"  restored        = %d", restored ? 1 : 0);
            Report(out, L"  egress_bytes_mb = %llu", (unsigned long long)(restore.bytes / kMb));
            Report(out, L"  dma_regions     = %d", restore.dmaRegions);
            Report(out, L"  undo_regions    = %d", restore.undoRegions);
            Report(out, L"  dma_peak_mbps   = %.1f", restore.dmaMBps);
            Report(out, L"  checksum        = %ls", restore.checksumOk ? L"MATCH" : L"MISMATCH");
            Report(out, L"  restore_ms      = %.2f", restore.totalMs);
            pool.LogSensors(L"after-egress");

            // STEP 5: the victim verifies itself.
            if (verify) SetEvent(verify);
            for (int i = 0; i < 200 && GetFileAttributesW(VictimResultPath().c_str()) ==
                                         INVALID_FILE_ATTRIBUTES; ++i) {
                Sleep(50);
            }
            FILE* result = _wfsopen(VictimResultPath().c_str(), L"r, ccs=UTF-8", _SH_DENYNO);
            if (result) {
                wchar_t line[256];
                while (fgetws(line, 256, result)) {
                    double percent = 0.0;
                    unsigned long long value = 0;
                    if (swscanf_s(line, L"match_percent=%lf", &percent) == 1) victimMatch = percent;
                    if (swscanf_s(line, L"hash_equal=%llu", &value) == 1) victimHashEqual = value != 0;
                }
                fclose(result);
            }

            Report(out, L"  --- STEP 5 (VICTIM SELF-VERIFICATION) ---");
            Report(out, L"  match_percent   = %.4f%%", victimMatch);
            Report(out, L"  bit_for_bit     = %ls", victimHashEqual ? L"100 percent HASH EQUAL" : L"HASH DIFFERS");
            Report(out, L"  total_restore_ms= %.2f", victimRestoreMs);

            if (verify) CloseHandle(verify);
            TerminateProcess(child.hProcess, 0);
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
        }
    } else {
        Report(out, L"[TEST 3: SKIPPED (run --torture-test for the 512 MB proof)]");
    }
    Report(out, L"");

    // ---------------------------------------------------------- TEST 4 -----
    Report(out, L"[TEST 4: COMMITGUARD]");
    const uint64_t commitFree = FreeCommitMb();
    Report(out, L"  free_commit_mb  = %llu", (unsigned long long)commitFree);
    Report(out, L"  slice_allowed   = %ls",
           commitFree >= kCommitGuardFloorMb ? L"yes (above 6144 MB floor)" : L"refused");
    Report(out, L"  pressure_floor  = %llu MB (auto-trim 2 slices)", (unsigned long long)kCommitPressureFloorMb);

    // Exercise the resize path so CommitGuard is evaluated for every slice.
    pool.RequestResize(2048);
    Sleep(400);
    const PoolStats resizedDown = pool.GetStats();
    pool.RequestResize(4096);
    Sleep(600);
    PoolStats resizedUp = pool.GetStats();
    Report(out, L"  resize_2048_mb  = %d slices", resizedDown.sliceCount);
    Report(out, L"  resize_4096_mb  = %d slices", resizedUp.sliceCount);
    Report(out, L"  commit_after_mb = %llu", (unsigned long long)FreeCommitMb());
    Report(out, L"");

    // ---------------------------------------------------------- TEST 5 -----
    Report(out, L"[TEST 5: STANDBY PURGE + 4 HARDWARE SENSORS]");
    const uint64_t available = AvailablePhysicalMb();
    const bool purgeOk = PurgeLowPriorityStandbyList();
    Report(out, L"  avail_ram_mb    = %llu", (unsigned long long)available);
    Report(out, L"  purge_call      = %ls", purgeOk ? L"STATUS_SUCCESS" : L"unavailable (needs admin)");
    PoolStats sensors = pool.GetStats();
    Report(out, L"  1_nvml_fb_used_mb  = %llu / %llu",
           (unsigned long long)(sensors.fbUsedBytes / kMb),
           (unsigned long long)(sensors.fbTotalBytes / kMb));
    Report(out, L"  2_nvml_bar1_used_mb= %llu / %llu",
           (unsigned long long)(sensors.bar1UsedBytes / kMb),
           (unsigned long long)(sensors.bar1TotalBytes / kMb));
    Report(out, L"  3_dxgi_local_used_mb = %llu",
           (unsigned long long)(sensors.usageBytes / kMb));
    Report(out, L"  4_avx2_ingress_mbps  = %.1f", streamMbps);
    Report(out, L"  4_gpu_dma_egress_mbps= %.1f", dmaMbps);
    Report(out, L"");

    // ---------------------------------------------------------- TEST 6 -----
    Report(out, L"[TEST 6: ENGINE REGRESSION]");
    RestoreResult none;
    const bool notFrozen = !cryo.CryoRestore(0xFFFFFFFF, false, none);
    Report(out, L"  restore_unknown_pid_refused = %d", notFrozen ? 1 : 0);
    const int frozenNow = cryo.GetStats().frozen;
    Report(out, L"  frozen_processes = %d", frozenNow);
    Report(out, L"  pool_after_tests_mb = %llu",
           (unsigned long long)(pool.GetStats().poolBytes / kMb));
    Report(out, L"  free_commit_end_mb  = %llu", (unsigned long long)FreeCommitMb());
    Report(out, L"");
    Report(out, L"RESULT: %ls", L"TORTURE TEST FINISHED");

    fclose(out);
    pool.Log(L"[torture] report written to %ls", path.c_str());
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
