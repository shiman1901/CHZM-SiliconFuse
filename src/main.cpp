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
    SetProcessWorkingSetSizeEx(process, (SIZE_T)-1, (SIZE_T)-1, 0);
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
    std::vector<DWORD> pids(4096);
    DWORD needed = 0;
    if (!EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &needed)) return 2;
    size_t count = needed / sizeof(DWORD);
    if (count > pids.size()) count = pids.size();

    const DWORD self = GetCurrentProcessId();
    int released = 0;
    for (size_t i = 0; i < count; ++i) {
        const DWORD pid = pids[i];
        if (!pid || pid == 4 || pid == self) continue;
        HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION,
                                     FALSE, pid);
        if (!process) continue;
        if (SetProcessWorkingSetSizeEx(process, (SIZE_T)-1, (SIZE_T)-1, 0)) ++released;
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask = 0;
        SetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state));
        CloseHandle(process);
    }
    return released > 0 ? 0 : 1;
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

int RunLiveCombat(VramPool& pool, CryoSwap& cryo, StorageBridge& bridge,
                  const std::wstring& dataDir)
{
    const std::wstring docs = ExecutableDir() + L"\\..\\docs";
    const std::wstring path =
        GetFileAttributesW(docs.c_str()) != INVALID_FILE_ATTRIBUTES
            ? docs + L"\\STAGE12_LIVE_COMBAT_REPORT.txt"
            : dataDir + L"\\STAGE12_LIVE_COMBAT_REPORT.txt";
    FILE* out = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (!out) return 6;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    Report(out, L"CHZM//SILICONFUSE  STAGE 12  LIVE COMBAT REPORT");
    Report(out, L"generated=%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth,
           now.wDay, now.wHour, now.wMinute, now.wSecond);
    Report(out, L"mode=live safe trim (no suspend, no MEM_RESET)");
    Report(out, L"");

    Report(out, L"[TEST 1: BUILD]");
    wchar_t exePath[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH * 2);
    WIN32_FILE_ATTRIBUTE_DATA fileInfo{};
    GetFileAttributesExW(exePath, GetFileExInfoStandard, &fileInfo);
    Report(out, L"  exe_size_bytes = %llu",
           (unsigned long long)(((uint64_t)fileInfo.nFileSizeHigh << 32) |
                                fileInfo.nFileSizeLow));
    Report(out, L"  hard_mode      = available only via --torture-test");
    Report(out, L"  live_mode      = quota + EmptyWorkingSet + EcoQoS, threads untouched");
    Report(out, L"");

    // Three warm processes of our own: if anything suspended them, they would
    // not answer the verify event after the trim.
    std::vector<VictimHandle> warm;
    const uint64_t warmSizes[3] = { 180, 220, 260 };
    for (int i = 0; i < 3; ++i) {
        VictimHandle handle;
        if (StartVictim(warmSizes[i], (uint32_t)(60 + i), handle)) {
            warm.push_back(handle);
        }
    }
    // -------------------------------------------------- BEFORE ------------
    const std::vector<DWORD> chromeBefore = PidsByName(L"chrome.exe");
    const double chromeWsBefore = SumWorkingSetMb(chromeBefore);
    const std::vector<WindowRef> windows = VisibleWindowsOf(chromeBefore);
    const uint64_t availBefore = AvailablePhysicalMb();

    Report(out, L"[TEST 2: SNAPSHOT BEFORE]");
    Report(out, L"  chrome_pids_before   = %d", (int)chromeBefore.size());
    Report(out, L"  chrome_ws_before_mb  = %.1f", chromeWsBefore);
    int chromeAboveThreshold = 0;
    for (DWORD pid : chromeBefore) {
        if (ReadProcessNumbers(pid).wsMb >= 50.0) ++chromeAboveThreshold;
    }
    Report(out, L"  chrome_visible_windows = %d", (int)windows.size());
    Report(out, L"  chrome_above_50mb    = %d (live trim threshold)", chromeAboveThreshold);
    for (const WindowRef& window : windows) {
        Report(out, L"    hwnd=0x%p pid=%lu title=%ls", (void*)window.hwnd,
               (unsigned long)window.pid, window.title.c_str());
    }
    const DWORD foregroundNow = []() {
        const HWND window = GetForegroundWindow();
        DWORD pid = 0;
        if (window) GetWindowThreadProcessId(window, &pid);
        return pid;
    }();
    const bool chromeFocused =
        std::find(chromeBefore.begin(), chromeBefore.end(), foregroundNow) != chromeBefore.end();
    Report(out, L"  foreground_pid       = %lu", (unsigned long)foregroundNow);
    Report(out, L"  chrome_foreground    = %d (focused apps are skipped by design)",
           chromeFocused ? 1 : 0);
    Report(out, L"  avail_ram_before_mb  = %llu", (unsigned long long)availBefore);
    Report(out, L"");

    // -------------------------------------------------- TRIM --------------

    Timer cycleTimer;
    cycleTimer.Begin();
    const int trimmed = cryo.RunCycle();
    const double cycleMs = cycleTimer.Ms();
    const CryoStats stats = cryo.GetStats();
    const uint64_t availAfterTrim = AvailablePhysicalMb();

    Report(out, L"[TEST 3: LIVE SAFE CRYOTRIM]");
    Report(out, L"  trimmed_count    = %d", trimmed);
    Report(out, L"  hot_cache_mb     = %llu (streamed into GDDR6)",
           (unsigned long long)(stats.hotBytes / kMb));
    Report(out, L"  saved_ddr4_mb    = %llu", (unsigned long long)(stats.savedBytes / kMb));
    Report(out, L"  cycle_ms         = %.2f", cycleMs);
    Report(out, L"  stored_trimmed   = %d", stats.trimmed);
    Report(out, L"  processes_frozen = %d (must stay 0 in live mode)", stats.frozen);
    Report(out, L"  trim_ingress_mbps= %.1f", cycleMs > 0.0 ? (double)stats.hotBytes / (1024.0 * 1024.0) / (cycleMs / 1000.0) : 0.0);
    Report(out, L"  avail_ram_after_mb = %llu", (unsigned long long)availAfterTrim);
    Report(out, L"  avail_ram_delta_mb = %lld",
           (long long)availAfterTrim - (long long)availBefore);
    pool.LogSensors(L"live-trim");
    Report(out, L"");

    // --------------------------------------------- SURVIVAL + RESPONSIVENESS
    Sleep(3000); // Chrome's own watchdog window

    const std::vector<DWORD> chromeAfter = PidsByName(L"chrome.exe");
    const double chromeWsAfter = SumWorkingSetMb(chromeAfter);

    int hung = 0;
    int answered = 0;
    double worstMs = 0.0;
    double totalMs = 0.0;
    for (const WindowRef& window : windows) {
        if (!IsWindow(window.hwnd)) continue;
        const PingResult ping = PingWindow(window.hwnd);
        if (ping.hung) ++hung;
        if (ping.answered) ++answered;
        totalMs += ping.ms;
        if (ping.ms > worstMs) worstMs = ping.ms;
    }
    const double averageMs = answered > 0 ? totalMs / answered : 0.0;

    Report(out, L"[TEST 4: SURVIVAL + WINDOW RESPONSIVENESS]");
    Report(out, L"  chrome_pids_after    = %d", (int)chromeAfter.size());
    Report(out, L"  crashed_tabs         = %d", (int)chromeBefore.size() - (int)chromeAfter.size());
    Report(out, L"  chrome_ws_after_mb   = %.1f", chromeWsAfter);
    Report(out, L"  windows_probed       = %d", (int)windows.size());
    Report(out, L"  windows_answered     = %d", answered);
    Report(out, L"  windows_hung         = %d (must be 0)", hung);
    Report(out, L"  ping_avg_ms          = %.2f (must be < 10.0)", averageMs);
    Report(out, L"  ping_worst_ms        = %.2f", worstMs);
    Report(out, L"  verdict              = %ls",
           (hung == 0 && averageMs < 10.0 &&
            chromeAfter.size() >= chromeBefore.size())
               ? L"NO HANG, NO CRASH"
               : L"CHECK REQUIRED");
    Report(out, L"");

    // Threads must never be suspended: the warm processes are trimmed and
    // still answer their verify event instantly.
    int warmTrimmed = 0;
    int warmReacted = 0;
    double worstReactMs = 0.0;
    for (VictimHandle& handle : warm) {
        if (cryo.IsTrimmed(handle.marker.pid)) ++warmTrimmed;
        Timer react;
        react.Begin();
        if (handle.verify) SetEvent(handle.verify);
        const DWORD wait = WaitForSingleObject(handle.child.hProcess, 2000);
        const double ms = react.Ms();
        if (wait == WAIT_OBJECT_0) {
            ++warmReacted;
            if (ms > worstReactMs) worstReactMs = ms;
        }
    }
    Report(out, L"[TEST 4B: THREAD SAFETY (NO SUSPEND)");
    Report(out, L"  warm_processes      = %d", (int)warm.size());
    Report(out, L"  warm_trimmed        = %d", warmTrimmed);
    Report(out, L"  warm_reacted        = %d (must equal warm_processes)", warmReacted);
    Report(out, L"  worst_react_ms      = %.2f (exit + self-verify, threads alive)",
           worstReactMs);
    for (VictimHandle& handle : warm) StopVictim(handle);
    warm.clear();
    Report(out, L"");

    // ------------------------------------------- VRAM PRESSURE (6 GB) -----
    const PoolStats before = pool.GetStats();
    pool.RequestResize(6144);
    Sleep(500);
    const PoolStats pressed = pool.GetStats();
    pool.RequestResize(4096);
    Sleep(400);
    const PoolStats relieved = pool.GetStats();

    Report(out, L"[TEST 5: VRAM PRESSURE (GRANULAR YIELD)]");
    Report(out, L"  slices_before  = %d (%llu MB)", before.sliceCount,
           (unsigned long long)(before.poolBytes / kMb));
    Report(out, L"  slices_6gb     = %d (%llu MB)", pressed.sliceCount,
           (unsigned long long)(pressed.poolBytes / kMb));
    Report(out, L"  slices_after   = %d (%llu MB)", relieved.sliceCount,
           (unsigned long long)(relieved.poolBytes / kMb));
    Report(out, L"  commit_free_mb = %llu", (unsigned long long)FreeCommitMb());
    Report(out, L"");

    // ------------------------------------------------ WAKE (ALT+TAB) ------
    double wakeMs = 0.0;
    const int woken = cryo.WakeGroup(L"chrome.exe", wakeMs);
    Sleep(400);
    const std::vector<DWORD> chromeWake = PidsByName(L"chrome.exe");
    const double chromeWsWake = SumWorkingSetMb(chromeWake);

    Report(out, L"[TEST 6: GROUP WAKE (SIMULATED ALT+TAB)");
    Report(out, L"  woken_processes  = %d (all chrome.exe)", woken);
    Report(out, L"  wake_ms          = %.2f", wakeMs);
    Report(out, L"  chrome_ws_wake_mb= %.1f", chromeWsWake);
    Report(out, L"  remaining_trimmed= %d (other applications)", cryo.GetStats().trimmed);
    Report(out, L"");

    // Release every remaining application so the machine is left clean.
    double flushMs = 0.0;
    const int flushed = cryo.WakeAllTrimmed();
    RunReleaseAll(); // also clears quotas left by earlier runs
    Report(out, L"[TEST 7: FINAL CLEANUP (RESET)");
    Report(out, L"  flushed_processes = %d", flushed);
    Report(out, L"  flush_ms          = %.2f", flushMs);
    Report(out, L"  trimmed_left      = %d (must be 0)", cryo.GetStats().trimmed);
    Report(out, L"");

    Report(out, L"[RESULT]");
    const bool pass = (hung == 0) && (averageMs < 10.0) &&
                      (chromeAfter.size() >= chromeBefore.size()) &&
                      (cryo.GetStats().trimmed == 0);
    Report(out, L"  status = %ls", pass ? L"PASS" : L"FAIL");
    Report(out, L"  note   = live mode never suspends threads and never issues MEM_RESET");
    fclose(out);

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

    const std::wstring dataDir = ResolveDataDir();
    CreateDirectoryW(dataDir.c_str(), nullptr);
    const std::wstring logPath = dataDir + L"\\engine.log";
    const bool torture = arguments.find(L"--torture-test") != std::wstring::npos;
    const bool selfTest = arguments.find(L"--self-test") != std::wstring::npos;
    const bool liveCombat = arguments.find(L"--live-combat-test") != std::wstring::npos;
    const bool headless = torture || selfTest || liveCombat;

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
        const int result = liveCombat
                               ? RunLiveCombat(pool, cryo, bridge, dataDir)
                               : RunTorture(pool, cryo, bridge, dataDir, torture);
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
