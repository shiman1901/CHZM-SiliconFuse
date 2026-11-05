#pragma once

#include <string>

// The pagefile attachment described in the brief is not implemented, because
// the required prerequisites do not exist in this build and the API contract
// in the brief is incorrect:
//   * there is no block-device driver, so there is no volume to format;
//   * formatting a real volume is destructive and is never attempted;
//   * NtCreatePagingFile(UNICODE_STRING*, LARGE_INTEGER*, LARGE_INTEGER*,
//     ULONG Priority) has no `flags` argument and no HybridPriority=7 value.
//
// This module reports the real state instead of fabricating STATUS_SUCCESS.
struct PagingStatus {
    bool attempted = false;
    bool volumeReady = false;
    bool pagefileReady = false;
    long ntstatus = 0;
    std::wstring detail;
};

PagingStatus ProbePagingEnvironment();
