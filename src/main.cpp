#include <windows.h>
#include <shlobj.h>

#include <string>

#include "engine/PagingManager.h"
#include "engine/StorageBridge.h"
#include "engine/VramPool.h"
#include "gui/MainWindow.h"

namespace {

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

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int showCommand)
{
    const std::wstring dataDir = LocalAppData();
    CreateDirectoryW(dataDir.c_str(), nullptr);
    const std::wstring logPath = dataDir + L"\\engine.log";
    const bool selfTest = commandLine && wcsstr(commandLine, L"--self-test") != nullptr;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Global\\ChzmSiliconFuseSingleInstance");
    const DWORD mutexError = GetLastError();
    if (mutex && mutexError == ERROR_ALREADY_EXISTS && !selfTest) {
        CloseHandle(mutex);
        return 0;
    }

    // 4 slices x 64 MB dirty window = 256 MB host staging.
    StorageBridge bridge;
    if (!bridge.Init(dataDir, 4ull * 64ull * 1024ull * 1024ull)) {
        if (mutex) CloseHandle(mutex);
        return 2;
    }

    VramPool pool;
    if (!pool.Init(&bridge, 4096, 1024, logPath)) {
        if (mutex) CloseHandle(mutex);
        return 3;
    }

    const PagingStatus paging = ProbePagingEnvironment();
    pool.Log(L"[paging] volume=%d pagefile=%d ntstatus=0x%08lX",
             paging.volumeReady ? 1 : 0, paging.pagefileReady ? 1 : 0,
             (unsigned long)paging.ntstatus);
    pool.Log(L"[paging] %ls", paging.detail.c_str());

    if (!selfTest) {
        pool.StartMonitor();
    }

    const PoolStats stats = pool.GetStats();
    pool.Log(L"[pool] resident=%llu MB slices=%d/%d budget=%llu MB usage=%llu MB "
             L"nvml_free=%llu MB",
             (unsigned long long)(stats.residentBytes / (1024ull * 1024ull)),
             stats.residentSlices, stats.sliceCount,
             (unsigned long long)(stats.budgetBytes / (1024ull * 1024ull)),
             (unsigned long long)(stats.usageBytes / (1024ull * 1024ull)),
             (unsigned long long)(stats.nvmlFreeBytes / (1024ull * 1024ull)));

    if (selfTest) {
        pool.Shutdown();
        bridge.Shutdown();
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    MainWindow window;
    if (!window.Create(instance, &pool, ExecutablePath())) {
        pool.Shutdown();
        bridge.Shutdown();
        if (mutex) CloseHandle(mutex);
        return 4;
    }
    window.Show(showCommand);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    pool.Shutdown();
    bridge.Shutdown();
    if (mutex) CloseHandle(mutex);
    return 0;
}
