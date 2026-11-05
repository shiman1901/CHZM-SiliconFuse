#include "PagingManager.h"

#include <windows.h>

PagingStatus ProbePagingEnvironment()
{
    PagingStatus status;

    // Do not call NtCreatePagingFile. There is no SiliconFuse volume, and
    // pointing the API at C: would replace the live system pagefile.
    status.attempted = false;
    status.volumeReady = false;
    status.pagefileReady = false;
    status.ntstatus = 0xC0000001L; // STATUS_UNSUCCESSFUL: nothing was attempted.
    status.detail =
        L"not attempted: no SiliconFuse block device (requires a signed kernel "
        L"driver), no volume to format, and NtCreatePagingFile has no flags or "
        L"HybridPriority parameter. Existing C: pagefile left untouched.";

    return status;
}
