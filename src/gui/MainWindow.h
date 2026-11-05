#pragma once

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <string>

class VramPool;

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE instance, VramPool* pool, const std::wstring& exePath);
    void Show(int cmdShow);
    HWND Handle() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT Dispatch(UINT msg, WPARAM wParam, LPARAM lParam);

    void EnableGlass();
    void Paint();
    void DrawScene(HDC dc, int width, int height);
    void OnTimer();
    void TogglePower();
    void ToggleAutostart();
    bool AutostartEnabled() const;
    void AddTrayIcon();
    void RemoveTrayIcon();

    void CyclePool();
    uint32_t MaxPoolMb() const;
    std::wstring PoolCellText() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    VramPool* pool_ = nullptr;
    std::wstring exePath_;

    HFONT fontTitle_ = nullptr;
    HFONT fontLabel_ = nullptr;
    HFONT fontMono_ = nullptr;

    bool autostart_ = false;
    bool trayAdded_ = false;
    bool poolAuto_ = true;
    bool poolHover_ = false;
    uint32_t poolMb_ = 0;

    HICON iconLarge_ = nullptr;
    HICON iconSmall_ = nullptr;

    RECT capsuleRect_{};
    RECT minimizeRect_{};
    RECT autostartRect_{};
    RECT poolRect_{};
};
