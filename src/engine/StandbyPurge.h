#pragma once

#include <cstdint>

// Available physical memory in MB (MEMORYSTATUSEX::ullAvailPhys).
uint64_t AvailablePhysicalMb();

// Drops the low priority standby list through
// ntdll!NtSetSystemInformation(SystemMemoryListInformation = 80,
//                              MemoryPurgeLowPriorityStandbyList = 5).
// Requires SE_PROF_SINGLE_PROCESS (admin). Returns false when the call is not
// available or the token does not carry the privilege.
bool PurgeLowPriorityStandbyList();
