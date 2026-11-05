#include "MainWindow.h"
#include "engine/CryoSwap.h"
#include "engine/VramPool.h"

#include <windowsx.h>
#include <psapi.h>
#include <dwmapi.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr wchar_t kClassName[] = L"CHZMSiliconFuseWindow";
constexpr wchar_t kWindowTitle[] = L"CHZM//SILICONFUSE";
constexpr UINT kTrayMessage = WM_APP + 1;

constexpr int kClientW = 740;
constexpr int kClientH = 440;

constexpr COLORREF kBaseTop = RGB(16, 16, 20);
constexpr COLORREF kBaseBottom = RGB(6, 6, 8);
constexpr COLORREF kPanel = RGB(20, 20, 26);
constexpr COLORREF kPanelBorder = RGB(44, 44, 54);
constexpr COLORREF kBlue = RGB(0, 96, 255);
constexpr COLORREF kBlueDim = RGB(6, 26, 70);
constexpr COLORREF kViolet = RGB(180, 44, 255);
constexpr COLORREF kVioletDim = RGB(34, 12, 54);
constexpr COLORREF kAmber = RGB(255, 152, 32);
constexpr COLORREF kAmberDim = RGB(54, 30, 4);
constexpr COLORREF kText = RGB(236, 236, 240);
constexpr COLORREF kDim = RGB(138, 138, 148);
constexpr COLORREF kRule = RGB(38, 38, 46);

void FillSolid(HDC dc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void DrawLabel(HDC dc, HFONT font, const std::wstring& text, const RECT& rect,
               COLORREF color, UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER)
{
    HFONT old = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT r = rect;
    DrawTextW(dc, text.c_str(), (int)text.size(), &r, format);
    SelectObject(dc, old);
}

void DrawPanel(HDC dc, const RECT& rect, COLORREF fill, COLORREF border, int radius)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, brush);
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawRule(HDC dc, int x1, int y, int x2, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN old = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, x1, y, nullptr);
    LineTo(dc, x2, y);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void DrawPowerGlyph(HDC dc, int cx, int cy, int radius, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN old = (HPEN)SelectObject(dc, pen);
    Arc(dc, cx - radius, cy - radius, cx + radius, cy + radius,
        cx + radius - 2, cy - 3, cx - radius + 2, cy - 3);
    MoveToEx(dc, cx, cy - radius - 1, nullptr);
    LineTo(dc, cx, cy + 1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

std::wstring FormatMb(uint64_t bytes)
{
    wchar_t buf[64];
    swprintf_s(buf, L"%.0f MB", (double)bytes / (1024.0 * 1024.0));
    return buf;
}

std::wstring FormatGb1(uint64_t bytes)
{
    wchar_t buf[64];
    swprintf_s(buf, L"%.1f", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

COLORREF Mix(COLORREF a, COLORREF b, int percent)
{
    const int ia = 100 - percent;
    return RGB((GetRValue(a) * ia + GetRValue(b) * percent) / 100,
               (GetGValue(a) * ia + GetGValue(b) * percent) / 100,
               (GetBValue(a) * ia + GetBValue(b) * percent) / 100);
}

void FillGradient(HDC dc, int width, int height)
{
    TRIVERTEX vertices[2]{};
    vertices[0].x = 0;
    vertices[0].y = 0;
    vertices[0].Red = (COLOR16)(GetRValue(kBaseTop) << 8);
    vertices[0].Green = (COLOR16)(GetGValue(kBaseTop) << 8);
    vertices[0].Blue = (COLOR16)(GetBValue(kBaseTop) << 8);
    vertices[0].Alpha = 0xFF00;

    vertices[1].x = width;
    vertices[1].y = height;
    vertices[1].Red = (COLOR16)(GetRValue(kBaseBottom) << 8);
    vertices[1].Green = (COLOR16)(GetGValue(kBaseBottom) << 8);
    vertices[1].Blue = (COLOR16)(GetBValue(kBaseBottom) << 8);
    vertices[1].Alpha = 0xFF00;

    GRADIENT_RECT rect{ 0, 1 };
    GradientFill(dc, vertices, 2, &rect, 1, GRADIENT_FILL_RECT_V);
}

} // namespace

MainWindow::MainWindow() = default;

MainWindow::~MainWindow()
{
    if (fontTitle_) DeleteObject(fontTitle_);
    if (fontLabel_) DeleteObject(fontLabel_);
    if (fontMono_) DeleteObject(fontMono_);
    if (iconSmall_) DestroyIcon(iconSmall_);
    if (iconLarge_) DestroyIcon(iconLarge_);
}

bool MainWindow::Create(HINSTANCE instance, VramPool* pool, CryoSwap* cryo,
                        const std::wstring& exePath)
{
    instance_ = instance;
    pool_ = pool;
    cryo_ = cryo;
    exePath_ = exePath;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;

    // Embedded icon group 1 (src/app.rc). LoadImage keeps our own handles so
    // the shell also shows them for the tray and the taskbar.
    iconLarge_ = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXICON),
                                  GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    iconSmall_ = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.hIcon = iconLarge_;
    wc.hIconSm = iconSmall_;

    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT frame{ 0, 0, kClientW, kClientH };
    AdjustWindowRectEx(&frame, style, FALSE, 0);

    hwnd_ = CreateWindowExW(0, kClassName, kWindowTitle, style,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            frame.right - frame.left, frame.bottom - frame.top,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    EnableGlass();

    fontTitle_ = CreateFontW(-27, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    fontLabel_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    fontMono_ = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");

    autostart_ = AutostartEnabled();
    AddTrayIcon();
    SetTimer(hwnd_, 1, 500, nullptr);
    return true;
}

void MainWindow::EnableGlass()
{
    // No DwmExtendFrameIntoClientArea / backdrop here: on a GDI surface the
    // DWM glass path composites the client with the backdrop and washes the
    // accent colours to grey. The title bar is kept dark, the client is
    // rendered opaque by GDI with the intended palette.
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
}

void MainWindow::Show(int cmdShow)
{
    ShowWindow(hwnd_, cmdShow);
    UpdateWindow(hwnd_);
}

void MainWindow::AddTrayIcon()
{
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = iconSmall_ ? iconSmall_ : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"CHZM//SILICONFUSE");
    Shell_NotifyIconW(NIM_ADD, &data);
    trayAdded_ = true;
}

void MainWindow::RemoveTrayIcon()
{
    if (!trayAdded_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayAdded_ = false;
}

bool MainWindow::AutostartEnabled() const
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    wchar_t value[MAX_PATH * 2]{};
    DWORD size = sizeof(value);
    const bool present =
        RegQueryValueExW(key, L"ChzmSiliconFuse", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(value), &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return present;
}

void MainWindow::ToggleAutostart()
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
        != ERROR_SUCCESS)
        return;

    if (autostart_) {
        RegDeleteValueW(key, L"ChzmSiliconFuse");
        autostart_ = false;
    } else {
        const std::wstring quoted = L"\"" + exePath_ + L"\"";
        RegSetValueExW(key, L"ChzmSiliconFuse", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted.c_str()),
                       (DWORD)((quoted.size() + 1) * sizeof(wchar_t)));
        autostart_ = true;
    }
    RegCloseKey(key);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::TogglePower()
{
    if (!pool_) return;
    if (pool_->IsEvacuated() || pool_->GetStats().sliceCount == 0) {
        pool_->RequestRestoreAll();
    } else {
        pool_->RequestEvictAll();
    }
}

void MainWindow::OnTimer()
{
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RunCryoNow()
{
    if (!cryo_) return;
    cryo_->SetEnabled(true);
    cryo_->RunCycle();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ResetCryo()
{
    if (!cryo_) return;
    cryo_->Reset();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring MainWindow::PoolCellText() const
{
    if (!pool_) return L"--";
    const uint32_t mb = poolAuto_ ? pool_->AutoTargetMb() : poolMb_;
    return FormatGb1((uint64_t)mb * 1024ull * 1024ull) + L" GB" +
           (poolAuto_ ? L" [AUTO]" : L" [FIXED]");
}

// Manual steps may never exceed half of the adapter's VRAM. The capacity is
// rounded to the marketed size first (a 12 GB card reports ~11.7 GB to DXGI),
// so a 12 GB GPU can still reach the 6 GB step.
uint32_t MainWindow::MaxPoolMb() const
{
    if (!pool_) return 1024;
    const uint64_t vramMb = pool_->AdapterVramBytes() / (1024ull * 1024ull);
    const uint64_t vramGb = (vramMb + 512ull) / 1024ull;
    uint64_t cap = vramGb * 512ull;
    if (cap > 6144) cap = 6144;
    return (uint32_t)cap;
}

// AUTO -> 1.0 -> 2.0 -> 4.0 -> 6.0 -> AUTO, skipping steps above the cap.
void MainWindow::CyclePool()
{
    if (!pool_) return;

    static const uint32_t kSteps[] = { 1024, 2048, 4096, 6144 };
    const uint32_t maxMb = MaxPoolMb();
    const uint32_t current = poolAuto_ ? 0 : poolMb_;

    uint32_t next = 0;
    if (current == 0) {
        for (uint32_t mb : kSteps) {
            if (mb <= maxMb) { next = mb; break; }
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            if (kSteps[i] != current) continue;
            for (int j = i + 1; j < 4; ++j) {
                if (kSteps[j] <= maxMb) { next = kSteps[j]; break; }
            }
            break;
        }
    }

    if (next == 0) {
        poolAuto_ = true;
        poolMb_ = (uint32_t)pool_->AutoTargetMb();
        pool_->RequestResize(pool_->AutoTargetMb());
    } else {
        poolAuto_ = false;
        poolMb_ = next;
        pool_->RequestResize(next);
    }
    pool_->Log(L"[ui] pool step -> %u MB (auto=%d, cap=%u MB)",
               poolMb_, poolAuto_ ? 1 : 0, maxMb);
    InvalidateRect(hwnd_, &poolRect_, FALSE);
}

void MainWindow::Paint()
{
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right;
    const int height = client.bottom;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HBITMAP old = (HBITMAP)SelectObject(mem, bitmap);

    FillGradient(mem, width, height);
    DrawScene(mem, width, height);

    BitBlt(dc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

void MainWindow::DrawScene(HDC dc, int width, int height)
{
    // Hairline frame and a soft top highlight give the flat gradient depth.
    HPEN edge = CreatePen(PS_SOLID, 1, Mix(kPanelBorder, kBaseTop, 35));
    HPEN oldEdge = (HPEN)SelectObject(dc, edge);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, width, height);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldEdge);
    DeleteObject(edge);

    HPEN highlight = CreatePen(PS_SOLID, 1, RGB(44, 44, 56));
    HPEN oldHighlight = (HPEN)SelectObject(dc, highlight);
    MoveToEx(dc, 1, 1, nullptr);
    LineTo(dc, width - 1, 1);
    SelectObject(dc, oldHighlight);
    DeleteObject(highlight);

    PoolStats stats = pool_ ? pool_->GetStats() : PoolStats{};
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);

    PERFORMANCE_INFORMATION perf{};
    perf.cb = sizeof(perf);
    GetPerformanceInfo(&perf, sizeof(perf));
    const double pageMb = (double)perf.PageSize / (1024.0 * 1024.0);
    const double commitUsedGb = perf.CommitTotal * pageMb / 1024.0;
    const double commitLimitGb = perf.CommitLimit * pageMb / 1024.0;

    const bool noPool = stats.sliceCount == 0;
    const bool evacuated = stats.residentSlices == 0 && stats.sliceCount > 0;
    const COLORREF accent = evacuated ? kAmber : kViolet;
    const COLORREF accentDim = evacuated ? kAmberDim : kVioletDim;

    const int pad = 28;
    const int contentW = width - pad * 2;

    DrawLabel(dc, fontTitle_, L"CHZM//SILICONFUSE",
              RECT{ pad, 14, width - 180, 50 }, kText);
    DrawLabel(dc, fontMono_, L"v0.3  PHASE 3",
              RECT{ width - 170, 20, width - pad, 44 }, kDim,
              DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    wchar_t dma[96];
    swprintf_s(dma, L"PCIe DMA: %.1f MB/s", stats.lastDmaMBps);
    DrawLabel(dc, fontMono_, dma,
              RECT{ pad, 56, width - pad, 78 }, kDim);

    const int barX = pad;
    const int barY = 86;
    const int barH = 48;
    const int vramW = (int)(contentW * 0.28);
    const int ramW = contentW - vramW - 4;

    const double ramRatio = memory.ullTotalPhys
        ? (double)(memory.ullTotalPhys - memory.ullAvailPhys) / (double)memory.ullTotalPhys
        : 0.0;
    const double vramRatio = stats.poolBytes
        ? (double)stats.residentBytes / (double)stats.poolBytes : 0.0;

    RECT ramBg{ barX, barY, barX + ramW, barY + barH };
    RECT vramBg{ barX + ramW + 4, barY, barX + contentW, barY + barH };
    FillSolid(dc, ramBg, kBlueDim);
    FillSolid(dc, vramBg, accentDim);

    RECT ramFill = ramBg;
    ramFill.right = ramFill.left + (LONG)(ramW * ramRatio);
    RECT vramFill = vramBg;
    vramFill.right = vramFill.left + (LONG)(vramW * (noPool ? 0.0 : vramRatio));
    FillSolid(dc, ramFill, kBlue);
    FillSolid(dc, vramFill, accent);

    DrawLabel(dc, fontMono_, L"R A M",
              RECT{ ramBg.left + 14, barY + 7, ramBg.right, barY + 25 },
              RGB(200, 214, 255));
    DrawLabel(dc, fontMono_, L"VRAM",
              RECT{ vramBg.left + 12, barY + 7, vramBg.right, barY + 25 },
              RGB(242, 214, 255));

    wchar_t ramText[64];
    swprintf_s(ramText, L"%.1f / %.1f GB",
               (double)(memory.ullTotalPhys - memory.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0),
               (double)memory.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
    DrawLabel(dc, fontMono_, ramText,
              RECT{ ramBg.left + 14, barY + 25, ramBg.right, barY + 43 }, kText);

    wchar_t vramText[64];
    if (noPool) {
        swprintf_s(vramText, L"NOT ALLOCATED");
    } else {
        swprintf_s(vramText, L"%.1f / %.1f GB",
                   (double)stats.residentBytes / (1024.0 * 1024.0 * 1024.0),
                   (double)stats.poolBytes / (1024.0 * 1024.0 * 1024.0));
    }
    DrawLabel(dc, fontMono_, vramText,
              RECT{ vramBg.left + 12, barY + 25, vramBg.right, barY + 43 }, kText);

    const int bracketY = barY + barH + 12;
    HPEN pen = CreatePen(PS_SOLID, 1, kRule);
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, barX, bracketY, nullptr);
    LineTo(dc, barX, bracketY + 7);
    LineTo(dc, barX + contentW, bracketY + 7);
    LineTo(dc, barX + contentW, bracketY);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    const double ramGb = (double)memory.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    const double poolGb = (double)stats.poolBytes / (1024.0 * 1024.0 * 1024.0);
    wchar_t tier[96];
    swprintf_s(tier, L"%.0f GB  UNIFIED SILICON TIER", ramGb + poolGb);
    DrawLabel(dc, fontMono_, tier,
              RECT{ barX, bracketY + 11, barX + contentW, bracketY + 30 }, kDim);

    wchar_t commit[96];
    swprintf_s(commit, L"SYS COMMIT: %.1f / %.1f GB", commitUsedGb, commitLimitGb);
    DrawLabel(dc, fontMono_, commit,
              RECT{ barX, bracketY + 34, barX + contentW, bracketY + 54 }, kText);

    std::wstring status;
    COLORREF statusColor = kText;
    if (noPool) {
        status = L"GPU BUSY  -  POOL NOT ALLOCATED";
        statusColor = kAmber;
    } else if (stats.busy) {
        status = evacuated ? L"RESTORING (VRAM)" : L"EVACUATING (SSD)";
        statusColor = kAmber;
    } else if (evacuated) {
        status = L"EVACUATED (SSD)";
        statusColor = kAmber;
    } else {
        status = L"ACTIVE (VRAM)";
        statusColor = kViolet;
    }

    wchar_t metrics[192];
    swprintf_s(metrics, L"%ls   |   SLICES %d/%d   |   EVICT %.1f ms   |   RESTORE %.1f ms",
               status.c_str(), stats.residentSlices, stats.sliceCount,
               stats.lastEvictMs, stats.lastRestoreMs);
    DrawLabel(dc, fontMono_, metrics,
              RECT{ barX, bracketY + 58, barX + contentW, bracketY + 78 }, statusColor);

    const int statsY = 252;
    DrawRule(dc, pad, statsY - 14, width - pad, kRule);

    struct Block { const wchar_t* label; std::wstring value; COLORREF color; };
    const Block blocks[4] = {
        { L"POOL", PoolCellText(), poolAuto_ ? kText : kViolet },
        { L"PAGES", std::to_wstring(stats.residentSlices) + L" / " + std::to_wstring(stats.sliceCount), kText },
        { L"BUDGET", FormatMb(stats.budgetBytes), kText },
        { L"NVML FREE", FormatMb(stats.nvmlFreeBytes), kText },
    };
    const int blockW = contentW / 4;

    // The POOL cell is the only interactive metric: hover highlights it and a
    // click cycles the target size.
    poolRect_ = RECT{ pad - 10, statsY - 10, pad + blockW - 6, statsY + 44 };
    if (poolHover_) {
        DrawPanel(dc, poolRect_, Mix(kPanel, kBaseTop, 30),
                  Mix(kPanelBorder, kViolet, 55), 10);
    }

    for (int i = 0; i < 4; ++i) {
        const int x = pad + blockW * i;
        DrawLabel(dc, fontLabel_, blocks[i].label,
                  RECT{ x, statsY, x + blockW - 10, statsY + 18 }, kDim);
        DrawLabel(dc, fontMono_, blocks[i].value,
                  RECT{ x, statsY + 18, x + blockW - 10, statsY + 40 },
                  blocks[i].color);
    }

    DrawRule(dc, pad, 312, width - pad, kRule);

    // CryoSwap strip: live DDR4 savings, last wake latency, cache depth.
    const int cryoY = 320;
    const int pillH = 24;
    cryoResetRect_ = RECT{ width - pad - 74, cryoY, width - pad, cryoY + pillH };
    cryoRunRect_ = RECT{ cryoResetRect_.left - 92, cryoY,
                         cryoResetRect_.left - 8, cryoY + pillH };

    CryoStats cryoStats{};
    if (cryo_) cryoStats = cryo_->GetStats();

    wchar_t cryoText[160];
    swprintf_s(cryoText, L"CRYO SAVED: %.1f GB DDR4  |  WAKE %.1f MS  |  FROZEN %d",
               (double)cryoStats.savedBytes / (1024.0 * 1024.0 * 1024.0),
               cryoStats.lastWakeMs, cryoStats.frozen);
    DrawLabel(dc, fontMono_, cryoText,
              RECT{ pad, cryoY, cryoRunRect_.left - 12, cryoY + pillH }, kDim);

    DrawPanel(dc, cryoRunRect_, kPanel, Mix(kPanelBorder, kViolet, 45), 10);
    DrawLabel(dc, fontMono_, L"RUN CRYO", cryoRunRect_, kViolet,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    DrawPanel(dc, cryoResetRect_, kPanel, kPanelBorder, 10);
    DrawLabel(dc, fontMono_, L"RESET", cryoResetRect_, kDim,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    const int controlY = 356;
    const int controlH = 44;

    autostartRect_ = RECT{ pad, controlY, pad + 196, controlY + controlH };
    const std::wstring autostartText =
        std::wstring(L"AUTOSTART: [ ") + (autostart_ ? L"ON" : L"OFF") + L" ]";
    DrawLabel(dc, fontMono_, autostartText, autostartRect_,
              autostart_ ? kViolet : kDim);

    const int capsuleW = 230;
    capsuleRect_ = RECT{ (width - capsuleW) / 2, controlY,
                         (width - capsuleW) / 2 + capsuleW, controlY + controlH };

    RECT glowOuter = capsuleRect_;
    InflateRect(&glowOuter, 3, 3);
    DrawPanel(dc, glowOuter, kBaseBottom, Mix(accentDim, kBaseBottom, 30), 24);
    RECT glowInner = capsuleRect_;
    InflateRect(&glowInner, 1, 1);
    DrawPanel(dc, glowInner, kBaseBottom, Mix(accentDim, accent, 45), 22);

    DrawPanel(dc, capsuleRect_, Mix(accentDim, kBaseBottom, 20), accent, 22);

    const std::wstring capsuleText = evacuated ? L"RESTORE TO VRAM" : L"EVACUATE TO SSD";
    SIZE textSize{};
    HFONT oldFont = (HFONT)SelectObject(dc, fontMono_);
    GetTextExtentPoint32W(dc, capsuleText.c_str(), (int)capsuleText.size(), &textSize);
    SelectObject(dc, oldFont);

    const int glyphW = 18;
    const int gap = 10;
    const int totalW = glyphW + gap + textSize.cx;
    const int startX = capsuleRect_.left + (capsuleW - totalW) / 2;
    const int centerY = (capsuleRect_.top + capsuleRect_.bottom) / 2;

    DrawPowerGlyph(dc, startX + glyphW / 2, centerY, 7, accent);
    DrawLabel(dc, fontMono_, capsuleText,
              RECT{ startX + glyphW + gap, capsuleRect_.top,
                    capsuleRect_.right, capsuleRect_.bottom },
              accent, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    minimizeRect_ = RECT{ width - pad - 170, controlY,
                          width - pad, controlY + controlH };
    DrawPanel(dc, minimizeRect_, kPanel, kPanelBorder, 12);
    DrawLabel(dc, fontMono_, L"MINIMIZE TO TRAY", minimizeRect_, kDim,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->Dispatch(msg, wParam, lParam);
}

LRESULT MainWindow::Dispatch(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT:
        Paint();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        OnTimer();
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInRect(&poolRect_, pt)) {
            CyclePool();
            return 0;
        }
        if (PtInRect(&cryoRunRect_, pt)) {
            RunCryoNow();
            return 0;
        }
        if (PtInRect(&cryoResetRect_, pt)) {
            ResetCryo();
            return 0;
        }
        if (PtInRect(&capsuleRect_, pt)) {
            TogglePower();
            return 0;
        }
        if (PtInRect(&autostartRect_, pt)) {
            ToggleAutostart();
            return 0;
        }
        if (PtInRect(&minimizeRect_, pt)) {
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const bool hover = PtInRect(&poolRect_, pt) != 0;
        if (hover != poolHover_) {
            poolHover_ = hover;
            InvalidateRect(hwnd_, &poolRect_, FALSE);
            if (hover) {
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
                TrackMouseEvent(&track);
            }
        }
        SetCursor(LoadCursorW(nullptr, hover ? IDC_HAND : IDC_ARROW));
        return 0;
    }

    case WM_MOUSELEAVE:
        if (poolHover_) {
            poolHover_ = false;
            InvalidateRect(hwnd_, &poolRect_, FALSE);
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd_, SW_HIDE);
        return 0;

    case kTrayMessage:
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
        } else if (lParam == WM_RBUTTONUP) {
            POINT pt{};
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Open");
            AppendMenuW(menu, MF_STRING, 2, L"Exit");
            SetForegroundWindow(hwnd_);
            const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                           pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) {
                ShowWindow(hwnd_, SW_SHOW);
                SetForegroundWindow(hwnd_);
            } else if (cmd == 2) {
                DestroyWindow(hwnd_);
            }
        }
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        // Fail-safe: never leave a frozen process behind on logoff/shutdown.
        if (wParam && cryo_) cryo_->RestoreAllFrozen();
        if (wParam) ExitProcess(0);
        return 0;

    case WM_DESTROY:
        if (cryo_) cryo_->RestoreAllFrozen();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}
