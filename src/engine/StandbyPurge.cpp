#include "StandbyPurge.h"

#include <windows.h>

namespace {

// SYSTEM_INFORMATION_CLASS numbers are not in the public headers.
constexpr int kSystemMemoryListInformation = 80;
constexpr int kMemoryPurgeLowPriorityStandbyList = 5;

typedef LONG(NTAPI* PfnNtSetSystemInformation)(int, void*, unsigned long);

PfnNtSetSystemInformation Resolve()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
    if (!ntdll) return nullptr;
    return reinterpret_cast<PfnNtSetSystemInformation>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtSetSystemInformation")));
}

} // namespace

uint64_t AvailablePhysicalMb()
{
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) return 0;
    return memory.ullAvailPhys / (1024ull * 1024ull);
}

bool PurgeLowPriorityStandbyList()
{
    const PfnNtSetSystemInformation fn = Resolve();
    if (!fn) return false;
    int command = kMemoryPurgeLowPriorityStandbyList;
    const LONG status = fn(kSystemMemoryListInformation, &command, sizeof(command));
    return status == 0; // STATUS_SUCCESS
}
