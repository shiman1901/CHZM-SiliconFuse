#pragma once

#include <cstdint>

// CommitGuard: the pool must never eat the system commit reserve that WDDM
// needs for its own allocations. Everything here is a cheap counter read.
uint64_t FreeCommitMb();
uint64_t TotalCommitLimitMb();

// A new 1 GB slice is only created while at least this much commit is free.
constexpr uint64_t kCommitGuardFloorMb = 6144;
// Below this the pool trims slices on its own to protect the WDDM reserve.
constexpr uint64_t kCommitPressureFloorMb = 3072;
