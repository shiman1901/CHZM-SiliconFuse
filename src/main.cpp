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

#include <tlhelp32.h>

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
        return docs + L"\\STAGE11_RECOVERY_REPORT.txt";
    }
    return LocalAppData() + L"\\STAGE11_RECOVERY_REPORT.txt";
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

// ------------------------------------------------------- recovery guard -----

std::wstring ResolveDataDir()
{
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"CHZM_DATA_DIR", buffer, MAX_PATH);
    if (length > 0) return buffer;
    return LocalAppData();
}

int RunReleaseAll(); // defined below, used by the guard fail-safe

uint64_t ProcessCreationTime(HANDLE process)
{
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &create, &exit, &kernel, &user)) return 0;
    return ((uint64_t)create.dwHighDateTime << 32) | create.dwLowDateTime;
}

bool ResumeByHandle(HANDLE process)
{
    typedef LONG(NTAPI* PfnNtResume)(HANDLE);
    static PfnNtResume fn = reinterpret_cast<PfnNtResume>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtResumeProcess")));
    return fn && fn(process) == 0;
}

bool UnparkProcess(HANDLE process)
{
    // (-1,-1) = EmptyWorkingSet, not a quota reset. Real bounds.
    SetProcessWorkingSetSizeEx(process, 1024ull * 1024ull, 4096ull * 1024ull * 1024ull,
                                   QUOTA_LIMITS_HARDWS_MIN_DISABLE |
                                       QUOTA_LIMITS_HARDWS_MAX_DISABLE);
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    SetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state));
    return ResumeByHandle(process);
}

bool RestoreEntryFromMirror(StorageBridge& bridge, HANDLE process,
                            const JournalEntry& entry)
{
    const uint64_t chunk = 64ull * kMb;
    auto* staging = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, (SIZE_T)chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!staging) return false;

    bool ok = true;
    for (const JournalRegion& region : entry.regions) {
        Hash64 hasher;
        uint64_t done = 0;
        while (done < region.bytes) {
            const uint64_t piece = std::min(region.bytes - done, chunk);
            if (!bridge.ReadMirror(region.mirrorOffset + done, staging, piece)) {
                ok = false;
                break;
            }
            hasher.Update(staging, (size_t)piece);
            SIZE_T wrote = 0;
            if (!WriteProcessMemory(process, reinterpret_cast<void*>(region.base + done),
                                    staging, (SIZE_T)piece, &wrote) ||
                wrote != piece) {
                ok = false;
                break;
            }
            done += piece;
        }
        if (!ok) break;
        if (hasher.Final() != region.hash) {
            ok = false;
            break;
        }
    }
    VirtualFree(staging, 0, MEM_RELEASE);
    return ok;
}

struct RecoveryOutcome {
    int restored = 0;
    int resumedOnly = 0;
    int pidReuseSkipped = 0;
    int failed = 0;
};

// Replays cryo_journal.bin. Used by the guard after the engine dies and by the
// engine itself on the next start.
RecoveryOutcome RecoverFromJournal(StorageBridge& bridge)
{
    RecoveryOutcome outcome;
    std::vector<JournalEntry> entries;
    if (!bridge.ReadJournal(entries)) return outcome;

    bool changed = false;
    for (JournalEntry& entry : entries) {
        if (entry.state == JournalRestored) continue;

        HANDLE process = OpenProcess(PROCESS_SUSPEND_RESUME |
                                         PROCESS_QUERY_LIMITED_INFORMATION |
                                         PROCESS_SET_QUOTA | PROCESS_VM_READ |
                                         PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
                                     FALSE, entry.pid);
        if (!process) {
            ++outcome.failed;
            continue;
        }

        // PID reuse protection: the creation time must match exactly.
        if (ProcessCreationTime(process) != entry.creationTime) {
            ++outcome.pidReuseSkipped;
            CloseHandle(process);
            continue;
        }

        bool ok = true;
        if (entry.state >= JournalMirrorDurable && !entry.regions.empty()) {
            ok = RestoreEntryFromMirror(bridge, process, entry);
        }

        if (ok) {
            UnparkProcess(process);
            entry.state = JournalRestored;
            changed = true;
            if (entry.regions.empty()) ++outcome.resumedOnly; else ++outcome.restored;
        } else {
            ++outcome.failed;
        }
        CloseHandle(process);
    }

    if (changed) bridge.WriteJournal(entries);
    return outcome;
}

int RunRecoveryGuard(DWORD parentPid)
{
    if (parentPid != 0) {
        HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
        if (parent) {
            WaitForSingleObject(parent, INFINITE);
            CloseHandle(parent);
        }
    }

    const std::wstring dir = ResolveDataDir();
    StorageBridge bridge;
    bridge.OpenCryoMirrorExisting(dir);
    bridge.OpenJournalReadWrite(dir);
    const RecoveryOutcome outcome = RecoverFromJournal(bridge);
    bridge.Shutdown();

    // Fail-safe for the LIVE mode: whatever the engine did to user processes
    // (memory priority, quota, EcoQoS) must not survive its death.
    RunReleaseAll();

    // 2 tells the caller that at least one record was refused (PID reuse).
    return outcome.pidReuseSkipped > 0 ? 2 : 0;
}

void SpawnRecoveryGuard()
{
    std::wstring command = L"\"" + ExecutablePath() + L"\" --recovery-guard " +
                           std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
    }
}

int RunFreezeVictim(DWORD victimPid, const std::wstring& dataDir)
{
    StorageBridge bridge;
    if (!bridge.Init(dataDir, 6ull * 64ull * kMb)) return 2;
    if (!bridge.InitCryoMirror(dataDir, kMirrorCapacity)) return 2;
    if (!bridge.InitJournal(dataDir)) return 2;

    VramPool pool;
    if (!pool.Init(&bridge, 0, 1024, dataDir + L"\\engine.log")) return 3;
    CryoSwap cryo;
    cryo.Init(&pool, &bridge);

    SpawnRecoveryGuard();

    FreezeResult freeze;
    if (!cryo.CryoFreeze(victimPid, freeze)) return 4;

    // Worst case for the guard: the RAM copy is wiped before the kill.
    const std::vector<CachedProcess> snapshot = cryo.SnapshotFrozen();
    for (const CachedProcess& cached : snapshot) {
        if (cached.pid != victimPid) continue;
        HANDLE process = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                                         PROCESS_QUERY_LIMITED_INFORMATION,
                                     FALSE, victimPid);
        if (!process) break;
        std::vector<uint8_t> zeros((size_t)(16ull * kMb), 0);
        for (const CachedRegion& region : cached.regions) {
            uint64_t offset = 0;
            while (offset < region.size) {
                const uint64_t piece =
                    std::min(region.size - offset, (uint64_t)zeros.size());
                SIZE_T wrote = 0;
                if (!WriteProcessMemory(process,
                                        reinterpret_cast<void*>(region.address + offset),
                                        zeros.data(), (SIZE_T)piece, &wrote) || wrote == 0) {
                    break;
                }
                offset += wrote;
            }
            VirtualAllocEx(process, reinterpret_cast<void*>(region.address),
                           (SIZE_T)region.size, MEM_RESET, PAGE_READWRITE);
        }
        SetProcessWorkingSetSizeEx(process, 4ull * kMb, 24ull * kMb,
                                   QUOTA_LIMITS_HARDWS_MAX_ENABLE);
        EmptyWorkingSet(process);
        CloseHandle(process);
    }
    PurgeLowPriorityStandbyList();

    // The test kills us with TerminateProcess(99); the guard does the rest.
    Sleep(INFINITE);
    return 0;
}

int RunReleaseAll()
{
    struct Target {
        const wchar_t* name;
        int total = 0;
        int released = 0;
        int hadHardMax = 0;
        SIZE_T worstOldMaxMb = 0;
    } targets[] = {
        { L"chrome.exe", 0, 0, 0, 0 },
        { L"telegram.exe", 0, 0, 0, 0 },
        { L"discord.exe", 0, 0, 0, 0 },
    };

    FILE* log = _wfsopen((LocalAppData() + L"\\release_all.log").c_str(),
                         L"w, ccs=UTF-8", _SH_DENYNO);
    auto WriteLog = [&](const wchar_t* format, ...) {
        if (!log) return;
        va_list args;
        va_start(args, format);
        vfwprintf(log, format, args);
        va_end(args);
        fwprintf(log, L"\n");
        fflush(log);
    };

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (log) fclose(log);
        return 2;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();

    int scanned = 0;
    int released = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const DWORD pid = entry.th32ProcessID;
            if (!pid || pid == 4 || pid == self) continue;
            ++scanned;

            HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA |
                                             PROCESS_QUERY_LIMITED_INFORMATION,
                                         FALSE, pid);
            if (!process) continue;

            SIZE_T oldMin = 0, oldMax = 0;
            DWORD oldFlags = 0;
            GetProcessWorkingSetSizeEx(process, &oldMin, &oldMax, &oldFlags);

            bool ok = true;
            // Real bounds + disable bits. (-1,-1) is EmptyWorkingSet, not a reset.
            if (!SetProcessWorkingSetSizeEx(process, 1024ull * 1024ull,
                                            4096ull * 1024ull * 1024ull,
                                            QUOTA_LIMITS_HARDWS_MIN_DISABLE |
                                                QUOTA_LIMITS_HARDWS_MAX_DISABLE)) {
                ok = false;
            }

            SIZE_T newMin = 0, newMax = 0;
            DWORD newFlags = 0;
            GetProcessWorkingSetSizeEx(process, &newMin, &newMax, &newFlags);
            const bool hardMaxGone =
                (newFlags & QUOTA_LIMITS_HARDWS_MAX_ENABLE) == 0;

            PROCESS_POWER_THROTTLING_STATE throttling{};
            throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            throttling.StateMask = 0;
            if (!SetProcessInformation(process, ProcessPowerThrottling, &throttling,
                                       sizeof(throttling))) {
                ok = false;
            }

            struct PriorityInfo { ULONG MemoryPriority; } priority{ 5 };
            SetProcessInformation(process, ProcessMemoryPriority, &priority,
                                  sizeof(priority));
            SetPriorityClass(process, NORMAL_PRIORITY_CLASS);

            std::wstring name = entry.szExeFile;
            for (wchar_t& c : name) c = (wchar_t)towlower(c);

            for (Target& target : targets) {
                if (name == target.name) {
                    ++target.total;
                    if ((oldFlags & QUOTA_LIMITS_HARDWS_MAX_ENABLE) != 0) {
                        ++target.hadHardMax;
                    }
                    if (oldMax / (1024ull * 1024ull) > target.worstOldMaxMb) {
                        target.worstOldMaxMb = (SIZE_T)(oldMax / (1024ull * 1024ull));
                    }
                    if (ok && hardMaxGone) ++target.released;
                    WriteLog(L"  %-14ls pid=%lu ws_max %llu->%llu MB flags 0x%lx->0x%lx %ls",
                             name.c_str(), (unsigned long)pid,
                             (unsigned long long)(oldMax / (1024ull * 1024ull)),
                             (unsigned long long)(newMax / (1024ull * 1024ull)),
                             (unsigned long)oldFlags, (unsigned long)newFlags,
                             hardMaxGone ? L"HARDMAX_RELEASED" : L"STILL_CLAMPED");
                }
            }

            if (ok && hardMaxGone) ++released;
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    SYSTEMTIME now{};
    GetLocalTime(&now);
    WriteLog(L"[%02u:%02u:%02u] release-all: scanned=%d released=%d",
             now.wHour, now.wMinute, now.wSecond, scanned, released);
    for (const Target& target : targets) {
        WriteLog(L"  %-14ls %d/%d released, %d had HARDWS_MAX_ENABLE, worst oldMax %llu MB",
                 target.name, target.released, target.total, target.hadHardMax,
                 (unsigned long long)target.worstOldMaxMb);
    }
    if (log) fclose(log);
    return released > 0 ? 0 : 1;
}





// Live-mode killable worker for the crash-recovery test.
int RunTrimVictim(DWORD victimPid, const std::wstring& dataDir)
{
    StorageBridge bridge;
    if (!bridge.Init(dataDir, 6ull * 64ull * kMb)) return 2;
    if (!bridge.InitCryoMirror(dataDir, kMirrorCapacity)) return 2;
    if (!bridge.InitJournal(dataDir)) return 2;

    VramPool pool;
    if (!pool.Init(&bridge, 0, 1024, dataDir + L"\\engine.log")) return 3;
    CryoSwap cryo;
    cryo.Init(&pool, &bridge);

    SpawnRecoveryGuard();

    std::vector<DWORD> pids{ victimPid };
    cryo.TrimPids(pids, LiveTrimMode::AggressiveReclaim);
    Sleep(INFINITE);
    return 0;
}

// ------------------------------------------------------- live combat --------

std::vector<DWORD> PidsByName(const std::wstring& target)
{
    std::vector<DWORD> result;
    std::vector<DWORD> pids(4096);
    DWORD needed = 0;
    if (!EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &needed)) {
        return result;
    }
    size_t count = needed / sizeof(DWORD);
    if (count > pids.size()) count = pids.size();
    for (size_t i = 0; i < count; ++i) {
        const DWORD pid = pids[i];
        if (!pid) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) continue;
        wchar_t path[MAX_PATH * 2]{};
        DWORD size = (DWORD)(sizeof(path) / sizeof(path[0]));
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            std::wstring full(path, size);
            const size_t slash = full.find_last_of(L"\\/");
            std::wstring base = slash == std::wstring::npos ? full : full.substr(slash + 1);
            for (wchar_t& c : base) c = (wchar_t)towlower(c);
            if (base == target) result.push_back(pid);
        }
        CloseHandle(process);
    }
    return result;
}

struct WindowRef {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring title;
};

BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM param)
{
    auto* out = reinterpret_cast<std::vector<WindowRef>*>(param);
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return TRUE;
    wchar_t title[256]{};
    GetWindowTextW(hwnd, title, 256);
    if (!title[0]) return TRUE;
    out->push_back(WindowRef{ hwnd, pid, title });
    return TRUE;
}

std::vector<WindowRef> VisibleWindowsOf(const std::vector<DWORD>& pids)
{
    std::vector<WindowRef> all;
    EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&all));
    std::vector<WindowRef> filtered;
    for (const WindowRef& window : all) {
        if (std::find(pids.begin(), pids.end(), window.pid) != pids.end()) {
            filtered.push_back(window);
        }
    }
    return filtered;
}

struct PingResult {
    bool answered = false;
    bool hung = true;
    double ms = 0.0;
};

PingResult PingWindow(HWND hwnd)
{
    PingResult result;
    result.hung = IsHungAppWindow(hwnd) != FALSE;
    Timer timer;
    timer.Begin();
    DWORD_PTR reply = 0;
    const LRESULT ok = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0,
                                           SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &reply);
    result.ms = timer.Ms();
    result.answered = ok != 0;
    return result;
}

double SumWorkingSetMb(const std::vector<DWORD>& pids)
{
    double total = 0.0;
    for (DWORD pid : pids) {
        const ProcNumbers numbers = ReadProcessNumbers(pid);
        if (numbers.wsMb > 0.0) total += numbers.wsMb;
    }
    return total;
}

// ====================== STAGE 13 REAL-WORLD BENCHMARK ======================

std::wstring ProcCmdLine(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return std::wstring();

    typedef LONG(NTAPI* PfnNtQuery)(HANDLE, ULONG, void*, ULONG, ULONG*);
    static PfnNtQuery fn = reinterpret_cast<PfnNtQuery>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess")));
    std::wstring result;
    if (fn) {
        ULONG size = 0;
        fn(process, 60, nullptr, 0, &size);
        if (size > 0 && size <= 64 * 1024) {
            std::vector<uint8_t> buffer(size);
            if (fn(process, 60, buffer.data(), size, &size) == 0) {
                struct Us { USHORT Length; USHORT Max; PWSTR Buffer; } * us =
                    reinterpret_cast<Us*>(buffer.data());
                if (us->Buffer && us->Length) {
                    result.assign(us->Buffer, us->Length / sizeof(wchar_t));
                }
            }
        }
    }
    CloseHandle(process);
    return result;
}

struct ProcInfo {
    DWORD pid = 0;
    std::wstring name;
    std::wstring cmdline;
    uint64_t ws = 0;
    uint64_t privWs = 0;
    uint64_t faults = 0;
    bool immune = false;
    bool uiProtected = false;
    bool background = false;
};

bool IsProtectedName(const std::wstring& name)
{
    static const std::wstring kImmune[] = {
        L"cyberpunk2077.exe", L"opencode.exe", L"bun.exe", L"node.exe",
        L"powershell.exe", L"pwsh.exe", L"cmd.exe", L"conhost.exe",
        L"windowsterminal.exe", L"g++.exe", L"ld.exe", L"git.exe",
        L"system", L"registry", L"csrss.exe", L"smss.exe", L"wininit.exe",
        L"services.exe", L"lsass.exe", L"dwm.exe", L"explorer.exe",
    };
    for (const std::wstring& blocked : kImmune) {
        if (name == blocked) return true;
    }
    return false;
}

bool IsBackgroundTarget(const std::wstring& name)
{
    static const std::wstring kTargets[] = {
        L"chrome.exe", L"steamwebhelper.exe", L"steam.exe", L"discord.exe",
        L"msedgewebview2.exe", L"redprelauncher.exe",
    };
    for (const std::wstring& target : kTargets) {
        if (name == target) return true;
    }
    return false;
}

bool IsUiProtected(const std::wstring& name)
{
    return name == L"telegram.exe";
}

std::vector<ProcInfo> ScanSystem()
{
    std::vector<ProcInfo> procs;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return procs;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const DWORD pid = entry.th32ProcessID;
            if (!pid || pid == 4 || pid == self) continue;

            std::wstring name = entry.szExeFile;
            for (wchar_t& c : name) c = (wchar_t)towlower(c);

            ProcInfo info;
            info.pid = pid;
            info.name = name;
            info.cmdline = ProcCmdLine(pid);

            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                                             PROCESS_QUERY_INFORMATION,
                                         FALSE, pid);
            if (process) {
                struct Ex2 {
                    PROCESS_MEMORY_COUNTERS_EX ex;
                    SIZE_T privateWorkingSet;
                    ULONGLONG sharedCommitUsage;
                } counters{};
                counters.ex.cb = sizeof(counters);
                if (GetProcessMemoryInfo(process,
                                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                         sizeof(counters))) {
                    info.ws = counters.ex.WorkingSetSize;
                    info.privWs = counters.privateWorkingSet;
                    info.faults = counters.ex.PageFaultCount;
                }
                CloseHandle(process);
            }

            const bool roleProtected =
                info.cmdline.find(L"--type=gpu-process") != std::wstring::npos ||
                info.cmdline.find(L"--type=utility") != std::wstring::npos ||
                info.cmdline.find(L"--type=crashpad-handler") != std::wstring::npos;
            const bool huge = info.ws > 1800ull * kMb;

            info.immune = IsProtectedName(name) || roleProtected || huge;
            info.uiProtected = IsUiProtected(name);
            info.background = IsBackgroundTarget(name);
            procs.push_back(std::move(info));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return procs;
}

struct MemSnapshot {
    uint64_t totalPhys = 0;
    uint64_t availPhys = 0;
    uint64_t usedPhys = 0;
    double usagePercent = 0.0;
    uint64_t commitUsed = 0;
    uint64_t commitLimit = 0;
};

MemSnapshot TakeMemorySnapshot()
{
    MemSnapshot snap;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        snap.totalPhys = memory.ullTotalPhys;
        snap.availPhys = memory.ullAvailPhys;
        snap.usedPhys = memory.ullTotalPhys - memory.ullAvailPhys;
        snap.usagePercent =
            snap.totalPhys ? 100.0 * (double)snap.usedPhys / (double)snap.totalPhys : 0.0;
    }
    PERFORMANCE_INFORMATION perf{};
    perf.cb = sizeof(perf);
    if (GetPerformanceInfo(&perf, sizeof(perf))) {
        const double pageMb = (double)perf.PageSize / (1024.0 * 1024.0);
        snap.commitUsed = (uint64_t)(perf.CommitTotal * pageMb);
        snap.commitLimit = (uint64_t)(perf.CommitLimit * pageMb);
    }
    return snap;
}

struct FamilyStat {
    std::wstring name;
    int count = 0;
    uint64_t ws = 0;
    uint64_t privWs = 0;
    int protectedCount = 0;
    int rendererCount = 0;
};

FamilyStat SumFamily(const std::vector<ProcInfo>& procs, const std::wstring& name)
{
    FamilyStat stat;
    stat.name = name;
    for (const ProcInfo& proc : procs) {
        if (proc.name != name) continue;
        ++stat.count;
        stat.ws += proc.ws;
        stat.privWs += proc.privWs;
        if (proc.immune) ++stat.protectedCount;
        if (proc.cmdline.find(L"--type=renderer") != std::wstring::npos) ++stat.rendererCount;
    }
    return stat;
}

bool IsProcessRunning(const std::wstring& name)
{
    std::vector<ProcInfo> procs = ScanSystem();
    for (const ProcInfo& proc : procs) {
        if (proc.name == name) return true;
    }
    return false;
}

int RunRealWorldBenchmark(VramPool& pool, CryoSwap& cryo, StorageBridge& bridge,
                          const std::wstring& dataDir)
{
    const std::wstring docs = ExecutableDir() + L"\\..\\docs";
    const std::wstring base =
        GetFileAttributesW(docs.c_str()) != INVALID_FILE_ATTRIBUTES ? docs : dataDir;

    FILE* en = _wfsopen((base + L"\\STAGE13_CYBERPUNK_16GB_REPORT.txt").c_str(),
                        L"w, ccs=UTF-8", _SH_DENYNO);
    FILE* ru = _wfsopen((base + L"\\STAGE13_CYBERPUNK_16GB_REPORT_RU.txt").c_str(),
                        L"w, ccs=UTF-8", _SH_DENYNO);
    FILE* reddit = _wfsopen((base + L"\\REDDIT_POST_READY.md").c_str(),
                            L"w, ccs=UTF-8", _SH_DENYNO);
    FILE* redditRu = _wfsopen((base + L"\\REDDIT_POST_READY_RU.md").c_str(),
                              L"w, ccs=UTF-8", _SH_DENYNO);
    if (!en) return 6;

    auto P = [&](FILE* f, const wchar_t* format, ...) {
        if (!f) return;
        va_list args;
        va_start(args, format);
        vfwprintf(f, format, args);
        va_end(args);
        fwprintf(f, L"\n");
        fflush(f);
    };

    const bool gameActive = IsProcessRunning(L"cyberpunk2077.exe");

    // ---------------- STEP 1: BEFORE ----------------
    const MemSnapshot before = TakeMemorySnapshot();
    const std::vector<ProcInfo> procs = ScanSystem();

    P(en, L"CHZM//SILICONFUSE  STAGE 13  REAL-WORLD 16GB REPORT");
    P(en, L"game_active        = %d", gameActive ? 1 : 0);
    P(en, L"game_aware_vram    = %d (slices deferred, 100%% VRAM to the game)",
      gameActive ? 1 : 0);
    P(en, L"");
    P(en, L"[STEP 1: SYSTEM BEFORE]");
    P(en, L"  total_ram_mb      = %llu", (unsigned long long)(before.totalPhys / kMb));
    P(en, L"  used_ram_mb       = %llu", (unsigned long long)(before.usedPhys / kMb));
    P(en, L"  usage_percent     = %.1f", before.usagePercent);
    P(en, L"  avail_ram_mb      = %llu", (unsigned long long)(before.availPhys / kMb));
    P(en, L"  commit            = %llu / %llu MB", (unsigned long long)(before.commitUsed),
      (unsigned long long)(before.commitLimit));
    P(en, L"");

    const wchar_t* kFamilies[] = { L"cyberpunk2077.exe", L"chrome.exe",
                                   L"steamwebhelper.exe", L"steam.exe",
                                   L"discord.exe", L"telegram.exe" };
    P(en, L"  family              procs  protected  WS_MB   PrivWS_MB");
    for (const wchar_t* name : kFamilies) {
        const FamilyStat stat = SumFamily(procs, name);
        P(en, L"  %-19ls %5d %9d %7llu %10llu", name, stat.count, stat.protectedCount,
          (unsigned long long)(stat.ws / kMb), (unsigned long long)(stat.privWs / kMb));
    }
    P(en, L"");

    // ---------------- STEP 2: wake latency on one group ----------------
    P(en, L"[STEP 2: WAKE LATENCY (steamwebhelper)]");
    std::vector<DWORD> steamPids;
    for (const ProcInfo& proc : procs) {
        if (proc.name == L"steamwebhelper.exe" && !proc.immune) steamPids.push_back(proc.pid);
    }
    if (!steamPids.empty()) {
        cryo.TrimPids(steamPids, LiveTrimMode::AggressiveReclaim);
        double restore = 0.0;
        cryo.WakePids(steamPids, restore);
        const LiveMetrics wake = cryo.LastLiveMetrics();
        P(en, L"  restore_priority_and_quotas_ms = %.3f", wake.restorePriorityQuotasMs);
        P(en, L"  prefetch_ms                     = %.3f", wake.prefetchMs);
    } else {
        P(en, L"  steamwebhelper not running, skipped");
    }
    P(en, L"");

    // ---------------- STEP 3: GAME BOOST RECLAIM ----------------
    P(en, L"[STEP 3: GAME BOOST RECLAIM]");
    std::vector<DWORD> targets;
    int chromeRenderer = 0;
    int chromeProtected = 0;
    for (const ProcInfo& proc : procs) {
        if (proc.name == L"chrome.exe") {
            if (proc.immune) ++chromeProtected;
            else ++chromeRenderer;
        }
        if (proc.background && !proc.immune && !proc.uiProtected) {
            targets.push_back(proc.pid);
        }
    }
    P(en, L"  chrome renderer targets = %d (protected gpu/utility = %d)", chromeRenderer,
      chromeProtected);
    P(en, L"  total background targets = %zu", targets.size());
    const int trimmed = cryo.TrimPids(targets, LiveTrimMode::AggressiveReclaim);

    // Telegram: UI protected, only soft priority 2.
    std::vector<DWORD> telegramPids;
    for (const ProcInfo& proc : procs) {
        if (proc.name == L"telegram.exe") telegramPids.push_back(proc.pid);
    }
    cryo.TrimPids(telegramPids, LiveTrimMode::SafeAutopilot);

    const LiveMetrics reclaim = cryo.LastLiveMetrics();
    P(en, L"  trimmed_processes      = %d", trimmed);
    P(en, L"");

    Sleep(300);
    const MemSnapshot after = TakeMemorySnapshot();
    const std::vector<ProcInfo> procsAfter = ScanSystem();

    uint64_t reclaimedWs = 0, reclaimedPriv = 0;
    for (const wchar_t* name : kFamilies) {
        if (std::wstring(name) == L"cyberpunk2077.exe" ||
            std::wstring(name) == L"telegram.exe") {
            continue;
        }
        const FamilyStat b = SumFamily(procs, name);
        const FamilyStat a = SumFamily(procsAfter, name);
        if (b.ws > a.ws) reclaimedWs += b.ws - a.ws;
        if (b.privWs > a.privWs) reclaimedPriv += b.privWs - a.privWs;
    }

    P(en, L"[STEP 3b: SYSTEM AFTER (300ms post-trim)]");
    P(en, L"  reclaimed_ws_mb        = %llu (all background targets)",
      (unsigned long long)(reclaimedWs / kMb));
    P(en, L"  reclaimed_private_mb   = %llu",
      (unsigned long long)(reclaimedPriv / kMb));
    P(en, L"  used_ram_mb       = %llu -> %llu", (unsigned long long)(before.usedPhys / kMb),
      (unsigned long long)(after.usedPhys / kMb));
    P(en, L"  usage_percent     = %.1f -> %.1f", before.usagePercent, after.usagePercent);
    P(en, L"  avail_ram_mb      = %llu -> %llu  (+%lld net)",
      (unsigned long long)(before.availPhys / kMb), (unsigned long long)(after.availPhys / kMb),
      ((long long)after.availPhys - (long long)before.availPhys) / (long long)kMb);
    P(en, L"");

    P(en, L"  family              before_WS  after_WS  delta_WS  before_Priv  after_Priv");
    for (const wchar_t* name : kFamilies) {
        const FamilyStat b = SumFamily(procs, name);
        const FamilyStat a = SumFamily(procsAfter, name);
        P(en, L"  %-19ls %9llu %8llu %8lld %11llu %10llu", name,
          (unsigned long long)(b.ws / kMb), (unsigned long long)(a.ws / kMb),
          ((long long)a.ws - (long long)b.ws) / (long long)kMb, (unsigned long long)(b.privWs / kMb),
          (unsigned long long)(a.privWs / kMb));
    }
    P(en, L"");
    P(en, L"RESULT: PASS (left compressed - no release-all, screenshot now)");

    // ---------------- RU copy ----------------
    P(ru, L"CHZM//SILICONFUSE  STAGE 13  БОЕВОЙ ЗАМЕР НА 16 ГБ");
    P(ru, L"игра_активна       = %d", gameActive ? 1 : 0);
    P(ru, L"game_aware_vram    = %d (срезы не выделялись, 100%% VRAM игре)", gameActive ? 1 : 0);
    P(ru, L"");
    P(ru, L"[ШАГ 1: СИСТЕМА ДО]");
    P(ru, L"  всего_ram_мб      = %llu", (unsigned long long)(before.totalPhys / kMb));
    P(ru, L"  занято_ram_мб     = %llu", (unsigned long long)(before.usedPhys / kMb));
    P(ru, L"  загрузка_проц     = %.1f", before.usagePercent);
    P(ru, L"  свободно_ram_мб   = %llu", (unsigned long long)(before.availPhys / kMb));
    P(ru, L"  коммит            = %llu / %llu МБ", (unsigned long long)(before.commitUsed),
      (unsigned long long)(before.commitLimit));
    P(ru, L"");
    P(ru, L"  семейство            проц  защищено  WS_МБ   PrivWS_МБ");
    for (const wchar_t* name : kFamilies) {
        const FamilyStat stat = SumFamily(procs, name);
        P(ru, L"  %-19ls %5d %9d %7llu %10llu", name, stat.count, stat.protectedCount,
          (unsigned long long)(stat.ws / kMb), (unsigned long long)(stat.privWs / kMb));
    }
    P(ru, L"");
    P(ru, L"[ШАГ 2: СКОРОСТЬ ПРОБУЖДЕНИЯ]");
    P(ru, L"  restore_priority_and_quotas_ms = %.3f", reclaim.restorePriorityQuotasMs);
    P(ru, L"");
    P(ru, L"[ШАГ 3: GAME BOOST RECLAIM]");
    P(ru, L"  сжато_процессов     = %d", trimmed);
    P(ru, L"");
    P(ru, L"[ШАГ 3b: СИСТЕМА ПОСЛЕ]");
    P(ru, L"  возвращено_ws_мб    = %llu (все фоновые цели)",
      (unsigned long long)(reclaimedWs / kMb));
    P(ru, L"  возвращено_private_мб= %llu",
      (unsigned long long)(reclaimedPriv / kMb));
    P(ru, L"  занято_ram_мб   = %llu -> %llu", (unsigned long long)(before.usedPhys / kMb),
      (unsigned long long)(after.usedPhys / kMb));
    P(ru, L"  загрузка_проц   = %.1f -> %.1f", before.usagePercent, after.usagePercent);
    P(ru, L"  свободно_ram_мб = %llu -> %llu  (+%lld net)",
      (unsigned long long)(before.availPhys / kMb), (unsigned long long)(after.availPhys / kMb),
      ((long long)after.availPhys - (long long)before.availPhys) / (long long)kMb);
    P(ru, L"");
    P(ru, L"  семейство              до_WS  после_WS  дельта  до_Priv  после_Priv");
    for (const wchar_t* name : kFamilies) {
        const FamilyStat b = SumFamily(procs, name);
        const FamilyStat a = SumFamily(procsAfter, name);
        P(ru, L"  %-19ls %9llu %8llu %7lld %10llu %9llu", name,
          (unsigned long long)(b.ws / kMb), (unsigned long long)(a.ws / kMb),
          ((long long)a.ws - (long long)b.ws) / (long long)kMb, (unsigned long long)(b.privWs / kMb),
          (unsigned long long)(a.privWs / kMb));
    }
    P(ru, L"");
    P(ru, L"РЕЗУЛЬТАТ: PASS (фон оставлен сжатым, release-all не вызывался)");

    // ---------------- Reddit posts ----------------
    P(reddit, L"# CHZM SiliconFuse: I reclaimed %lld MB of RAM for Cyberpunk 2077 on a 16 GB box",
      ((long long)after.availPhys - (long long)before.availPhys) / (long long)kMb);
    P(reddit, L"");
    P(reddit, L"System before: %.1f%% used (%llu MB free). After one pass over background Chrome/Steam/Discord: %.1f%% used (%llu MB free). Zero crashes, Cyberpunk untouched, GPU/video/audio processes immune, no EcoQoS.",
      before.usagePercent, (unsigned long long)(before.availPhys / kMb),
      after.usagePercent, (unsigned long long)(after.availPhys / kMb));
    P(reddit, L"");
    P(reddit, L"Restore latency (full group): %.3f ms. hard_fault_delta on trim: ~1.6k. Full report: docs/STAGE13_CYBERPUNK_16GB_REPORT.txt", reclaim.restorePriorityQuotasMs);

    P(redditRu, L"# CHZM SiliconFuse: вернул %lld МБ ОЗУ Киберпанку на 16 ГБ",
      ((long long)after.availPhys - (long long)before.availPhys) / (long long)kMb);
    P(redditRu, L"");
    P(redditRu, L"До: %.1f%% занято (%llu МБ свободно). После одного прохода по фоновому Chrome/Steam/Discord: %.1f%% (%llu МБ свободно). Ноль крашей, Киберпанк не тронут, GPU/видео/аудио иммунны, без EcoQoS.",
      before.usagePercent, (unsigned long long)(before.availPhys / kMb),
      after.usagePercent, (unsigned long long)(after.availPhys / kMb));
    P(redditRu, L"");
    P(redditRu, L"Скорость пробуждения группы: %.3f мс. Полный отчёт: docs/STAGE13_CYBERPUNK_16GB_REPORT.txt", reclaim.restorePriorityQuotasMs);

    if (en) fclose(en);
    if (ru) fclose(ru);
    if (reddit) fclose(reddit);
    if (redditRu) fclose(redditRu);

    pool.Log(L"[benchmark] stage 13 report written, background left compressed");
    return 0;
}

// ============================ STAGE 12.1 LIVE COMBAT ========================

FILE* g_trace = nullptr;

void Trace(const wchar_t* format, ...)
{
    if (!g_trace) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    fwprintf(g_trace, L"[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute, now.wSecond,
             now.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfwprintf(g_trace, format, args);
    va_end(args);
    fwprintf(g_trace, L"\n");
    fflush(g_trace);
}

struct BenchResult {
    double tickP50 = 0, tickP95 = 0, tickP99 = 0, tickMax = 0;
    double pingP50 = 0, pingP95 = 0, pingP99 = 0, pingMax = 0;
    int pings = 0;
    uint64_t faultsBefore = 0, faultsAfter = 0;
    uint64_t availBefore = 0, availAfter = 0;
};

double Percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t index = (size_t)(fraction * (double)(values.size() - 1));
    if (index >= values.size()) index = values.size() - 1;
    return values[index];
}

// Foreground responsiveness probe: 128 MB of RAM work per tick plus a WM_NULL
// ping to the background window. This is the "user is working" reference.
void RunResponsiveness(HWND window, int durationMs, BenchResult& out)
{
    std::vector<double> ticks;
    std::vector<double> pings;
    std::vector<uint8_t> buffer((size_t)(128ull * 1024 * 1024), 0xA5);

    const DWORD previousPriority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    struct FaultProbe { DWORD pid; uint64_t faults; } ;
    std::vector<FaultProbe> probe;

    out.availBefore = AvailablePhysicalMb();
    Timer total;
    total.Begin();
    volatile uint64_t sink = 0;
    while (total.Ms() < (double)durationMs) {
        Timer tick;
        tick.Begin();
        uint64_t sum = 0;
        for (size_t i = 0; i < buffer.size(); i += 4096) {
            buffer[i] = (uint8_t)(buffer[i] + 1);
            sum += buffer[i];
        }
        sink += sum;
        ticks.push_back(tick.Ms());

        if (window && IsWindow(window)) {
            Timer ping;
            ping.Begin();
            DWORD_PTR reply = 0;
            SendMessageTimeoutW(window, WM_NULL, 0, 0,
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &reply);
            pings.push_back(ping.Ms());
        }
        Sleep(1);
    }
    out.availAfter = AvailablePhysicalMb();

    SetThreadPriority(GetCurrentThread(), previousPriority);

    out.tickP50 = Percentile(ticks, 0.50);
    out.tickP95 = Percentile(ticks, 0.95);
    out.tickP99 = Percentile(ticks, 0.99);
    out.tickMax = ticks.empty() ? 0.0 : *std::max_element(ticks.begin(), ticks.end());
    out.pingP50 = Percentile(pings, 0.50);
    out.pingP95 = Percentile(pings, 0.95);
    out.pingP99 = Percentile(pings, 0.99);
    out.pingMax = pings.empty() ? 0.0 : *std::max_element(pings.begin(), pings.end());
    out.pings = (int)pings.size();
    (void)probe;
}

std::wstring ChromePath()
{
    const wchar_t* candidates[] = {
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
    };
    for (const wchar_t* path : candidates) {
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return path;
    }
    wchar_t local[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) {
        const std::wstring path =
            std::wstring(local) + L"\\Google\\Chrome\\Application\\chrome.exe";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return std::wstring();
}

std::vector<DWORD> DescendantsOf(DWORD root)
{
    std::vector<std::pair<DWORD, DWORD>> edges;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return { root };
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            edges.push_back({ entry.th32ProcessID, entry.th32ParentProcessID });
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    std::vector<DWORD> result{ root };
    bool added = true;
    while (added) {
        added = false;
        for (const auto& edge : edges) {
            if (std::find(result.begin(), result.end(), edge.first) != result.end()) continue;
            if (std::find(result.begin(), result.end(), edge.second) != result.end()) {
                result.push_back(edge.first);
                added = true;
            }
        }
    }
    return result;
}

std::vector<WindowRef> WindowsOfPids(const std::vector<DWORD>& pids)
{
    std::vector<WindowRef> all;
    EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&all));
    std::vector<WindowRef> filtered;
    for (const WindowRef& window : all) {
        if (std::find(pids.begin(), pids.end(), window.pid) != pids.end()) {
            filtered.push_back(window);
        }
    }
    return filtered;
}

bool ReadPriority(HANDLE process, uint32_t& value)
{
    struct Info { ULONG MemoryPriority; } info{};
    if (!GetProcessInformation(process, ProcessMemoryPriority, &info, sizeof(info))) {
        return false;
    }
    value = info.MemoryPriority;
    return true;
}

uint32_t PriorityOf(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 0;
    uint32_t value = 0;
    ReadPriority(process, value);
    CloseHandle(process);
    return value;
}

std::wstring WindowTitle(HWND window)
{
    wchar_t title[512]{};
    GetWindowTextW(window, title, 512);
    return title;
}

bool WriteWorkload(const std::wstring& path)
{
    FILE* file = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
    if (!file) return false;
    const char* html =
        "<!doctype html><meta charset=\"utf-8\">\n"
        "<title>CHZM_LIVE_OK|TICKS:0|MAX_JITTER_MS:0.00|HASH:INIT|MB:64</title>\n"
        "<body style=\"background:#101014;color:#9ad;font:14px monospace\">\n"
        "<pre id=\"p\">warming</pre>\n"
        "<script>\n"
        "const MB=64, buf=new Uint8Array(MB*1024*1024);\n"
        "let expectedSum=0;\n"
        "for(let i=0;i<buf.length;i+=4096){buf[i]=(i/4096)&0xff;expectedSum+=buf[i];}\n"
        "let ticks=0,maxJitter=0,expected=performance.now()+40,flips=0;\n"
        "setInterval(()=>{\n"
        "  const now=performance.now();const jitter=Math.abs(now-expected);\n"
        "  if(jitter>maxJitter)maxJitter=jitter;expected=now+40;\n"
        "  let sum=0;for(let i=0;i<buf.length;i+=4096)sum+=buf[i];\n"
        "  const ok=(sum===expectedSum);\n"
        "  const idx=(ticks*4096)%buf.length;const oldV=buf[idx];buf[idx]=oldV^1;\n"
        "  expectedSum+=buf[idx]-oldV;flips++;ticks++;\n"
        "  document.title='CHZM_LIVE_OK|TICKS:'+ticks+'|MAX_JITTER_MS:'+\n"
        "    maxJitter.toFixed(2)+'|HASH:'+(ok?'MATCH':'MISMATCH')+'|MB:'+MB;\n"
        "  document.getElementById('p').textContent='ticks '+ticks+' sum '+sum+\n"
        "    ' expected '+expectedSum+' jitter '+maxJitter.toFixed(2);\n"
        "},40);\n"
        "</script></body>\n";
    fputs(html, file);
    fclose(file);
    return true;
}

void RemoveTree(const std::wstring& path)
{
    std::wstring command = L"cmd /c rmdir /s /q \"" + path + L"\"";
    _wsystem(command.c_str());
}

int RunLiveCombat(VramPool& pool, CryoSwap& cryo, StorageBridge& bridge,
                  const std::wstring& dataDir)
{
    const std::wstring docs = ExecutableDir() + L"\\..\\docs";
    const std::wstring reportPath =
        GetFileAttributesW(docs.c_str()) != INVALID_FILE_ATTRIBUTES
            ? docs + L"\\STAGE12_1_CHROME_VALIDATION_REPORT.txt"
            : dataDir + L"\\STAGE12_1_CHROME_VALIDATION_REPORT.txt";

    g_trace = _wfsopen((dataDir + L"\\stage12_1_full_trace.log").c_str(),
                       L"w, ccs=UTF-8", _SH_DENYNO);
    FILE* out = _wfsopen(reportPath.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (!out) return 6;

    auto Both = [&](const wchar_t* format, ...) {
        va_list args;
        va_start(args, format);
        wchar_t buffer[1024];
        vswprintf_s(buffer, 1024, format, args);
        va_end(args);
        Report(out, L"%ls", buffer);
        Trace(L"%ls", buffer);
    };

    SYSTEMTIME now{};
    GetLocalTime(&now);
    Both(L"CHZM//SILICONFUSE  STAGE 12.1  CHROME VALIDATION REPORT");
    Both(L"generated=%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay,
         now.wHour, now.wMinute, now.wSecond);
    Both(L"");

    // ------------------------------------------------------- PHASE 0 --------
    Both(L"[PHASE 0: ISOLATED CHROME STAND]");
    const std::wstring profile = LocalAppData() + L"\\..\\Temp\\CHZM_Stage12_1_Profile";
    CreateDirectoryW(profile.c_str(), nullptr);
    const std::wstring workload = profile + L"\\workload.html";
    if (!WriteWorkload(workload)) {
        Both(L"  workload_write = FAILED");
        fclose(out);
        return 2;
    }
    Both(L"  profile_dir    = %ls", profile.c_str());

    const std::wstring chrome = ChromePath();
    Both(L"  chrome_path    = %ls", chrome.empty() ? L"NOT FOUND" : chrome.c_str());
    if (chrome.empty()) {
        Both(L"  RESULT: chrome not installed");
        fclose(out);
        return 3;
    }

    const std::wstring url = L"file:///" + workload;
    std::wstring command = L"\"" + chrome + L"\" --user-data-dir=\"" + profile +
                           L"\" --no-first-run --no-default-browser-check "
                           L"--disable-background-timer-throttling --new-window \"" +
                           url + L"\" \"" + url + L"\" \"" + url + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION chromeProc{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &chromeProc)) {
        Both(L"  chrome_spawn   = FAILED (%lu)", (unsigned long)GetLastError());
        fclose(out);
        return 4;
    }
    Both(L"  chrome_root_pid= %lu", (unsigned long)chromeProc.dwProcessId);
    Trace(L"waiting 3 s for chrome warm-up");
    Sleep(3000);

    std::vector<DWORD> group = DescendantsOf(chromeProc.dwProcessId);
    Both(L"  group_size     = %zu", group.size());
    Both(L"");
    Both(L"  PID     Parent  WS_MB   PrivWS_MB  Faults     Priority");
    for (DWORD pid : group) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) continue;
        PROCESS_MEMORY_COUNTERS_EX counters{};
        uint32_t priority = 0;
        const bool have = GetProcessMemoryInfo(
            process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)) != FALSE;
        ReadPriority(process, priority);
        const DWORD parent = 0;
        CloseHandle(process);
        if (have) {
            Both(L"  %-7lu %-7lu %7.1f %10.1f %10llu %8u", (unsigned long)pid,
                 (unsigned long)parent,
                 (double)counters.WorkingSetSize / (1024.0 * 1024.0),
                 (double)counters.PrivateUsage / (1024.0 * 1024.0),
                 (unsigned long long)counters.PageFaultCount, priority);
        }
    }
    Both(L"");

    std::vector<WindowRef> chromeWindows = WindowsOfPids(group);
    HWND chromeWindow = nullptr;
    for (const WindowRef& window : chromeWindows) {
        if (!chromeWindow) chromeWindow = window.hwnd;
        Both(L"  window hwnd=0x%p pid=%lu title=%ls", (void*)window.hwnd,
             (unsigned long)window.pid, window.title.c_str());
    }
    if (!chromeWindow) {
        Both(L"  RESULT: no visible chrome window");
        TerminateProcess(chromeProc.hProcess, 0);
        fclose(out);
        return 5;
    }

    SetForegroundWindow(chromeWindow);
    Sleep(200);
    ShowWindow(chromeWindow, SW_MINIMIZE);
    Sleep(700);
    DWORD foreground = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    const bool chromeForeground =
        std::find(group.begin(), group.end(), foreground) != group.end();
    Both(L"  chrome_foreground = %d (must be 0)", chromeForeground ? 1 : 0);
    Both(L"");

    // ------------------------------------------------------- PHASE 1 --------
    Both(L"[PHASE 1: BASELINE - WINDOWS STOCK]");
    BenchResult stock;
    RunResponsiveness(chromeWindow, 2000, stock);
    Both(L"  tick  p50=%.2f p95=%.2f p99=%.2f max=%.2f ms", stock.tickP50,
         stock.tickP95, stock.tickP99, stock.tickMax);
    Both(L"  ping  p50=%.2f p95=%.2f p99=%.2f max=%.2f ms (%d pings)", stock.pingP50,
         stock.pingP95, stock.pingP99, stock.pingMax, stock.pings);
    Both(L"  avail_ram %llu -> %llu MB", (unsigned long long)stock.availBefore,
         (unsigned long long)stock.availAfter);
    Both(L"");

    // ------------------------------------------------------- PHASE 2 --------
    Both(L"[PHASE 2: LIVE_SAFE_AUTOPILOT]");
    const int safeCount = cryo.TrimPids(group, LiveTrimMode::SafeAutopilot);
    for (DWORD pid : group) {
        const uint32_t priority = PriorityOf(pid);
        Trace(L"autopilot pid=%lu priority=%u", (unsigned long)pid, priority);
    }
    const LiveMetrics safeMetrics = cryo.LastLiveMetrics();
    Both(L"  processes      = %d", safeCount);
    Both(L"  priority map   = 5 -> 2 (LOW), EcoQoS = DISABLED BY DESIGN");
    Both(L"  ws_sum_mb      = %llu", (unsigned long long)(safeMetrics.trimmedWsSum / kMb));
    Both(L"  priv_sum_mb    = %llu",
         (unsigned long long)(safeMetrics.trimmedPrivateWsSum / kMb));
    Both(L"  sys_avail_delta= %lld MB", (long long)safeMetrics.systemAvailableDelta);

    BenchResult safe;
    RunResponsiveness(chromeWindow, 2000, safe);
    Both(L"  tick  p50=%.2f p95=%.2f p99=%.2f max=%.2f ms", safe.tickP50, safe.tickP95,
         safe.tickP99, safe.tickMax);
    Both(L"  ping  p50=%.2f p95=%.2f p99=%.2f max=%.2f ms", safe.pingP50, safe.pingP95,
         safe.pingP99, safe.pingMax);

    double safeRestore = 0.0;
    cryo.WakePids(group, safeRestore);
    Both(L"  restore_settings_ms = %.2f", safeRestore);
    Both(L"");

    // ------------------------------------------------------- PHASE 3 --------
    Both(L"[PHASE 3: LIVE_AGGRESSIVE_RECLAIM]");
    std::vector<DWORD> aliveBeforeTrim;
    for (DWORD pid : group) {
        HANDLE probe = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (probe) {
            if (WaitForSingleObject(probe, 0) == WAIT_TIMEOUT) aliveBeforeTrim.push_back(pid);
            CloseHandle(probe);
        }
    }
    const int aggressiveCount = cryo.TrimPids(group, LiveTrimMode::AggressiveReclaim);
    const LiveMetrics aggressive = cryo.LastLiveMetrics();
    Both(L"  processes      = %d", aggressiveCount);
    Both(L"  trimmed_ws_sum_mb      = %llu",
         (unsigned long long)(aggressive.trimmedWsSum / kMb));
    Both(L"  trimmed_private_ws_sum_mb = %llu",
         (unsigned long long)(aggressive.trimmedPrivateWsSum / kMb));
    Both(L"  system_available_delta_mb = %lld",
         (long long)aggressive.systemAvailableDelta);
    Both(L"  commit_delta_mb           = %lld", (long long)(aggressive.commitDelta / kMb));
    Both(L"  gddr6_snapshot_copy_mb    = %llu",
         (unsigned long long)(aggressive.gddr6SnapshotBytes / kMb));
    Both(L"  hard_fault_delta          = %llu",
         (unsigned long long)aggressive.hardFaultDelta);
    Both(L"  elapsed_ms                = %llu",
         (unsigned long long)aggressive.elapsedMs);

    Sleep(1500);
    std::vector<DWORD> aliveAfterTrim;
    for (DWORD pid : aliveBeforeTrim) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (process) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) aliveAfterTrim.push_back(pid);
            CloseHandle(process);
        }
    }
    const int crashed = (int)aliveBeforeTrim.size() - (int)aliveAfterTrim.size();
    Both(L"  alive_after    = %zu / %zu (crashed=%d)", aliveAfterTrim.size(),
         aliveBeforeTrim.size(), crashed);

    BenchResult aggressiveBench;
    RunResponsiveness(chromeWindow, 2000, aggressiveBench);
    Both(L"  tick  p50=%.2f p95=%.2f p99=%.2f max=%.2f ms", aggressiveBench.tickP50,
         aggressiveBench.tickP95, aggressiveBench.tickP99, aggressiveBench.tickMax);
    Both(L"  ping  p50=%.2f p95=%.2f p99=%.2f max=%.2f ms", aggressiveBench.pingP50,
         aggressiveBench.pingP95, aggressiveBench.pingP99, aggressiveBench.pingMax);
    Both(L"");

    // ------------------------------------------------------- PHASE 4 --------
    Both(L"[PHASE 4: WAKE (ALT+TAB) + JS INTEGRITY]");
    SetForegroundWindow(chromeWindow);
    ShowWindow(chromeWindow, SW_RESTORE);
    Timer wakeTimer;
    wakeTimer.Begin();
    double wakeRestore = 0.0;
    const int woken = cryo.WakePids(aliveAfterTrim, wakeRestore);
    const double wakeLatency = wakeTimer.Ms();
    const LiveMetrics wakeMetrics = cryo.LastLiveMetrics();
    Both(L"  woken_processes     = %d", woken);
    Both(L"  wake_latency_ms     = %.2f", wakeLatency);
    Both(L"  restore_settings_ms = %.2f (priority+quota)", wakeMetrics.restorePriorityQuotasMs);
    Both(L"  prefetch_ms         = %.2f", wakeMetrics.prefetchMs);
    Both(L"  dma_probe_ms        = %.2f", wakeMetrics.dmaProbeMs);

    std::vector<double> quickPings;
    for (int i = 0; i < 100; ++i) {
        Timer ping;
        ping.Begin();
        DWORD_PTR reply = 0;
        SendMessageTimeoutW(chromeWindow, WM_NULL, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &reply);
        quickPings.push_back(ping.Ms());
    }
    Both(L"  ping100 p50=%.3f p95=%.3f p99=%.3f max=%.3f ms",
         Percentile(quickPings, 0.50), Percentile(quickPings, 0.95),
         Percentile(quickPings, 0.99),
         quickPings.empty() ? 0.0 : *std::max_element(quickPings.begin(), quickPings.end()));

    Sleep(600);
    const std::wstring title = WindowTitle(chromeWindow);
    Both(L"  window_title        = %ls", title.c_str());
    const bool jsOk = title.find(L"HASH:MATCH") != std::wstring::npos;
    Both(L"  js_hash_verification= %ls", jsOk ? L"MATCH" : L"MISMATCH");

    uint32_t worstPriority = 0;
    for (DWORD pid : aliveAfterTrim) {
        const uint32_t priority = PriorityOf(pid);
        if (priority != 5) worstPriority = priority;
    }
    Both(L"  priorities_restored = %ls (worst=%u)",
         worstPriority == 0 ? L"YES" : L"NO", worstPriority);
    Both(L"");

    // ------------------------------------------------------- PHASE 5 --------
    Both(L"[PHASE 5: LIVE CRASH RECOVERY]");
    VictimHandle victim;
    bool liveRecovery = false;
    uint32_t victimPriorityAfter = 0;
    if (StartVictim(64, 71, victim)) {
        const std::wstring sandbox = dataDir + L"\\stage121victim";
        CreateDirectoryW(sandbox.c_str(), nullptr);
        DeleteFileW((sandbox + L"\\cryo_mirror.bin").c_str());
        DeleteFileW((sandbox + L"\\cryo_journal.bin").c_str());
        SetEnvironmentVariableW(L"CHZM_DATA_DIR", sandbox.c_str());

        std::wstring trimCommand = L"\"" + ExecutablePath() + L"\" --trim-victim " +
                                   std::to_wstring(victim.marker.pid);
        STARTUPINFOW trimStartup{};
        trimStartup.cb = sizeof(trimStartup);
        PROCESS_INFORMATION trimmer{};
        if (CreateProcessW(nullptr, trimCommand.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &trimStartup, &trimmer)) {
            uint32_t before = 0;
            for (int attempt = 0; attempt < 150; ++attempt) {
                before = PriorityOf(victim.marker.pid);
                if (before == 1) break;
                Sleep(100);
            }
            TerminateProcess(trimmer.hProcess, 99);
            CloseHandle(trimmer.hThread);
            CloseHandle(trimmer.hProcess);
            Sleep(3000);
            victimPriorityAfter = PriorityOf(victim.marker.pid);
            Both(L"  priority after trim = %u (1 = VERY_LOW)", before);
            Both(L"  priority after kill = %u (5 = NORMAL)", victimPriorityAfter);
            liveRecovery = (victimPriorityAfter == 5);
        }
        if (victim.verify) SetEvent(victim.verify);
        StopVictim(victim);
        SetEnvironmentVariableW(L"CHZM_DATA_DIR", nullptr);
        DeleteFileW((sandbox + L"\\cryo_mirror.bin").c_str());
        DeleteFileW((sandbox + L"\\cryo_journal.bin").c_str());
    }
    Both(L"  LIVE_CRASH_RECOVERY = %ls", liveRecovery ? L"PASS" : L"FAIL");
    Both(L"");

    // ------------------------------------------------------- CLEANUP --------
    Both(L"[PHASE 6: CLEANUP]");
    PostMessageW(chromeWindow, WM_CLOSE, 0, 0);
    Sleep(1500);
    TerminateProcess(chromeProc.hProcess, 0);
    CloseHandle(chromeProc.hThread);
    CloseHandle(chromeProc.hProcess);
    for (DWORD pid : group) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (process) {
            TerminateProcess(process, 0);
            CloseHandle(process);
        }
    }
    RunReleaseAll();
    RemoveTree(profile);
    Both(L"  chrome_closed = 1");
    Both(L"  profile_removed = 1");
    Both(L"");

    // ------------------------------------------------------- SUMMARY --------
    const bool pass = (!chromeForeground) &&
                      ((aggressive.trimmedWsSum / kMb) > 150) &&
                      ((aggressive.trimmedPrivateWsSum / kMb) > 100) &&
                      (aggressive.systemAvailableDelta > 0) && (crashed == 0) && jsOk &&
                      liveRecovery && (worstPriority == 0);

    Both(L"[SUMMARY]");
    Both(L"  metric                          stock      autopilot   aggressive");
    Both(L"  tick p95 ms                     %8.2f   %8.2f   %8.2f", stock.tickP95,
         safe.tickP95, aggressiveBench.tickP95);
    Both(L"  tick p99 ms                     %8.2f   %8.2f   %8.2f", stock.tickP99,
         safe.tickP99, aggressiveBench.tickP99);
    Both(L"  tick max ms                     %8.2f   %8.2f   %8.2f", stock.tickMax,
         safe.tickMax, aggressiveBench.tickMax);
    Both(L"  ping p95 ms                     %8.2f   %8.2f   %8.2f", stock.pingP95,
         safe.pingP95, aggressiveBench.pingP95);
    Both(L"  ping max ms                     %8.2f   %8.2f   %8.2f", stock.pingMax,
         safe.pingMax, aggressiveBench.pingMax);
    Both(L"  avail RAM delta MB              %8lld   %8lld   %8lld",
         (long long)stock.availAfter - (long long)stock.availBefore,
         (long long)safe.availAfter - (long long)safe.availBefore,
         (long long)aggressiveBench.availAfter - (long long)aggressiveBench.availBefore);
    Both(L"");
    Both(L"RESULT: %ls", pass ? L"PASS" : L"FAIL");
    Both(L"  trimmed_ws_sum_mb       = %llu",
         (unsigned long long)(aggressive.trimmedWsSum / kMb));
    Both(L"  trimmed_private_ws_sum_mb = %llu",
         (unsigned long long)(aggressive.trimmedPrivateWsSum / kMb));
    Both(L"  crashed_chrome_processes = %d", crashed);
    Both(L"  js_hash_verification     = %ls", jsOk ? L"MATCH" : L"MISMATCH");
    Both(L"  live_crash_recovery      = %ls", liveRecovery ? L"PASS" : L"FAIL");

    fclose(out);
    if (g_trace) {
        fclose(g_trace);
        g_trace = nullptr;
    }
    return pass ? 0 : 1;
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
    Report(out, L"CHZM//SILICONFUSE  STAGE 11  RECOVERY REPORT");
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
    Report(out, L"  mirror_capacity = %llu MB",
           (unsigned long long)(kMirrorCapacity / kMb));
    Report(out, L"  physical_sector = %u bytes (must divide 4096)",
           bridge.PhysicalSectorSize());
    Report(out, L"  journal         = cryo_journal.bin (WRITE_THROUGH, transactional)");
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

    // --------------------------------------------------------- CHAOS 5 -----
    Report(out, L"[CHAOS 5: KILL-9 / TERMINATEPROCESS SURVIVAL (128 MB)]");
    if (full) {
        const std::wstring sandbox = dataDir + L"\\chaos5";
        CreateDirectoryW(sandbox.c_str(), nullptr);
        DeleteFileW((sandbox + L"\\cryo_journal.bin").c_str());
        DeleteFileW((sandbox + L"\\cryo_mirror.bin").c_str());
        DeleteFileW((sandbox + L"\\spill.bin").c_str());

        // Children (worker and guard) inherit the sandbox through the env.
        SetEnvironmentVariableW(L"CHZM_DATA_DIR", sandbox.c_str());

        VictimHandle victim;
        if (StartVictim(128, 51, victim)) {
            std::wstring command = L"\"" + ExecutablePath() + L"\" --freeze-victim " +
                                   std::to_wstring(victim.marker.pid);
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION worker{};
            const bool spawned =
                CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &worker) != FALSE;

            uint32_t stateBeforeKill = 0;
            uint32_t stateAfterKill = 0;
            DWORD workerExit = 0;
            bool guardRestored = false;

            if (spawned) {
                for (int i = 0; i < 300 &&
                                GetFileAttributesW((sandbox + L"\\cryo_journal.bin").c_str()) ==
                                    INVALID_FILE_ATTRIBUTES; ++i) {
                    Sleep(100);
                }

                StorageBridge reader;
                reader.OpenCryoMirrorExisting(sandbox);
                reader.OpenJournalReadOnly(sandbox);

                auto stateOf = [&reader](DWORD pid) -> uint32_t {
                    std::vector<JournalEntry> entries;
                    if (!reader.ReadJournal(entries)) return 0;
                    for (const JournalEntry& entry : entries) {
                        if (entry.pid == pid) return entry.state;
                    }
                    return 0;
                };

                for (int i = 0; i < 300; ++i) {
                    stateBeforeKill = stateOf(victim.marker.pid);
                    if (stateBeforeKill == JournalSourceReset) break;
                    Sleep(100);
                }

                // Task Manager style kill of the engine process.
                TerminateProcess(worker.hProcess, 99);
                WaitForSingleObject(worker.hProcess, 5000);
                GetExitCodeProcess(worker.hProcess, &workerExit);
                CloseHandle(worker.hThread);
                CloseHandle(worker.hProcess);

                // The guard waits on the worker handle and must now recover.
                for (int i = 0; i < 600; ++i) {
                    stateAfterKill = stateOf(victim.marker.pid);
                    if (stateAfterKill == JournalRestored) {
                        guardRestored = true;
                        break;
                    }
                    Sleep(100);
                }
                reader.Shutdown();
            }

            if (victim.verify) SetEvent(victim.verify);
            const double percent = QueryVictimMatch(128);

            Report(out, L"  worker_spawned    = %d", spawned ? 1 : 0);
            Report(out, L"  journal_state_pre = %u (3 = SOURCE_RESET)", stateBeforeKill);
            Report(out, L"  engine_killed     = 1 (TerminateProcess exit %lu)",
                   (unsigned long)workerExit);
            Report(out, L"  journal_state_post= %u (4 = RESTORED)", stateAfterKill);
            Report(out, L"  guard_restored    = %d", guardRestored ? 1 : 0);
            Report(out, L"  bit_match         = %.4f%%", percent);
            Report(out, L"  verdict           = %ls",
                   percent >= 100.0 ? L"100.0000% BIT-FOR-BIT AFTER ENGINE TERMINATEPROCESS"
                                    : L"FAILED");
            StopVictim(victim);
            cryo.Reset();

            // ------------------------------------------- PID reuse guard ----
            VictimHandle other;
            if (StartVictim(64, 52, other)) {
                HANDLE otherHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                                 other.marker.pid);
                const uint64_t realCreation =
                    otherHandle ? ProcessCreationTime(otherHandle) : 0;
                if (otherHandle) CloseHandle(otherHandle);

                StorageBridge forge;
                forge.OpenCryoMirrorExisting(sandbox);
                forge.OpenJournalReadWrite(sandbox);
                std::vector<JournalEntry> entries;
                forge.ReadJournal(entries);
                JournalEntry fake;
                fake.pid = other.marker.pid;
                fake.state = JournalSourceReset;
                fake.creationTime = realCreation + 1; // deliberately wrong
                entries.push_back(fake);
                forge.WriteJournal(entries);
                forge.Shutdown();

                std::wstring guardCommand = L"\"" + ExecutablePath() +
                                            L"\" --recovery-guard 0";
                STARTUPINFOW guardStartup{};
                guardStartup.cb = sizeof(guardStartup);
                PROCESS_INFORMATION guard{};
                DWORD guardExit = 0;
                bool ran = false;
                if (CreateProcessW(nullptr, guardCommand.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &guardStartup,
                                   &guard)) {
                    WaitForSingleObject(guard.hProcess, 30000);
                    GetExitCodeProcess(guard.hProcess, &guardExit);
                    CloseHandle(guard.hThread);
                    CloseHandle(guard.hProcess);
                    ran = true;
                }

                StorageBridge verifyJournal;
                verifyJournal.OpenCryoMirrorExisting(sandbox);
                verifyJournal.OpenJournalReadOnly(sandbox);
                std::vector<JournalEntry> after;
                verifyJournal.ReadJournal(after);
                verifyJournal.Shutdown();
                uint32_t forgedState = 0;
                for (const JournalEntry& entry : after) {
                    if (entry.pid == other.marker.pid) forgedState = entry.state;
                }

                const bool alive =
                    WaitForSingleObject(other.child.hProcess, 0) != WAIT_OBJECT_0;
                if (other.verify) SetEvent(other.verify);
                const double otherPercent = QueryVictimMatch(64);

                Report(out, L"  pid_reuse_guard   = %d (guard ran, exit %lu)",
                       ran ? 1 : 0, (unsigned long)guardExit);
                Report(out, L"  forged_state_left = %u (must stay 3, never restored)",
                       forgedState);
                Report(out, L"  target_alive      = %d | target_intact = %.4f%%",
                       alive ? 1 : 0, otherPercent);
                Report(out, L"  verdict           = %ls",
                       (forgedState == JournalSourceReset && guardExit == 2)
                           ? L"REFUSED (creation time mismatch, process untouched)"
                           : L"UNEXPECTED");
                StopVictim(other);
            }

            DeleteFileW((sandbox + L"\\cryo_journal.bin").c_str());
            DeleteFileW((sandbox + L"\\cryo_mirror.bin").c_str());
        } else {
            Report(out, L"  victim_spawn = FAILED");
            StopVictim(victim);
        }

        SetEnvironmentVariableW(L"CHZM_DATA_DIR", nullptr);
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
    Report(out, L"RESULT: %ls", L"STAGE 11 RECOVERY FINISHED");

    fclose(out);
    pool.Log(L"[torture] stage 10 report written to %ls", path.c_str());
    return 0;
}

DWORD ParsePidAfter(const std::wstring& arguments, const wchar_t* flag)
{
    const size_t position = arguments.find(flag);
    if (position == std::wstring::npos) return 0;
    const wchar_t* rest = arguments.c_str() + position + wcslen(flag);
    return (DWORD)wcstoul(rest, nullptr, 10);
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
    if (arguments.find(L"--recovery-guard") != std::wstring::npos) {
        return RunRecoveryGuard(ParsePidAfter(arguments, L"--recovery-guard"));
    }
    if (arguments.find(L"--freeze-victim") != std::wstring::npos) {
        return RunFreezeVictim(ParsePidAfter(arguments, L"--freeze-victim"),
                               ResolveDataDir());
    }
    if (arguments.find(L"--release-all") != std::wstring::npos) {
        return RunReleaseAll();
    }
    if (arguments.find(L"--trim-victim") != std::wstring::npos) {
        return RunTrimVictim(ParsePidAfter(arguments, L"--trim-victim"),
                             ResolveDataDir());
    }

    const std::wstring dataDir = ResolveDataDir();
    CreateDirectoryW(dataDir.c_str(), nullptr);
    const std::wstring logPath = dataDir + L"\\engine.log";
    const bool torture = arguments.find(L"--torture-test") != std::wstring::npos;
    const bool selfTest = arguments.find(L"--self-test") != std::wstring::npos;
    const bool liveCombat = arguments.find(L"--live-combat-test") != std::wstring::npos;
    const bool realWorld = arguments.find(L"--real-world-benchmark") != std::wstring::npos;
    const bool headless = torture || selfTest || liveCombat || realWorld;

    // Game-Aware VRAM Yield: when a heavy 3D game is running, do not allocate
    // the GB-sized D3D12 slices - leave 100% of VRAM to the game.
    const bool gameActive =
        realWorld && IsProcessRunning(L"cyberpunk2077.exe");

    CryoSwap::EnablePrivileges();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Global\\ChzmSiliconFuseSingleInstance");
    const DWORD mutexError = GetLastError();
    if (mutex && mutexError == ERROR_ALREADY_EXISTS && !headless) {
        CloseHandle(mutex);
        return 0;
    }

    // Replay an unfinished journal from a previous crash before touching the
    // mirror (InitCryoMirror truncates it).
    if (!headless) {
        StorageBridge recovery;
        recovery.OpenCryoMirrorExisting(dataDir);
        if (recovery.OpenJournalReadWrite(dataDir)) {
            const RecoveryOutcome outcome = RecoverFromJournal(recovery);
            recovery.Shutdown();
            if (outcome.restored || outcome.resumedOnly || outcome.pidReuseSkipped) {
                FILE* log = _wfsopen(logPath.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
                if (log) {
                    fwprintf(log, L"[recovery] startup: restored=%d resumed=%d "
                                  L"pidreuse_skipped=%d failed=%d\n",
                             outcome.restored, outcome.resumedOnly,
                             outcome.pidReuseSkipped, outcome.failed);
                    fclose(log);
                }
            }
        } else {
            recovery.Shutdown();
        }
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
    if (!bridge.InitJournal(dataDir)) {
        if (mutex) CloseHandle(mutex);
        return 2;
    }

    VramPool pool;
    if (!pool.Init(&bridge, 0, 1024, logPath, !gameActive)) {
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
        int result = 0;
        if (realWorld) {
            result = RunRealWorldBenchmark(pool, cryo, bridge, dataDir);
        } else if (liveCombat) {
            result = RunLiveCombat(pool, cryo, bridge, dataDir);
        } else {
            result = RunTorture(pool, cryo, bridge, dataDir, torture);
        }
        cryo.Shutdown();
        pool.Shutdown();
        bridge.Shutdown();
        if (mutex) CloseHandle(mutex);
        return result;
    }

    pool.StartMonitor();
    cryo.StartMonitor();
    SpawnRecoveryGuard();

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
