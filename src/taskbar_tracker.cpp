#include "taskbar_tracker.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace {

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(
        a.c_str(), static_cast<int>(a.size()),
        b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring BoundedWideString(const wchar_t* buf, size_t cap) {
    std::wstring s(buf, wcsnlen(buf, cap));
    s.erase(
        std::remove_if(s.begin(), s.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }),
        s.end());
    return s;
}

struct DisplayMetaEntry {
    std::wstring gdiDeviceName;
    std::wstring monitorDevicePath;
    std::wstring friendlyName;
};

std::vector<DisplayMetaEntry> QueryDisplayMetadata() {
    std::vector<DisplayMetaEntry> entries;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;

    LONG status = ERROR_SUCCESS;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (status != ERROR_SUCCESS || pathCount == 0) {
            return entries;
        }
        paths.resize(pathCount);
        modes.resize(modeCount);
        status = QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (status != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
    }
    if (status != ERROR_SUCCESS) {
        return entries;
    }

    for (UINT32 i = 0; i < pathCount; ++i) {
        const auto& p = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName{};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = p.sourceInfo.adapterId;
        srcName.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS) {
            continue;
        }
        const std::wstring gdi = BoundedWideString(
            srcName.viewGdiDeviceName, ARRAYSIZE(srcName.viewGdiDeviceName));
        if (gdi.empty()) {
            continue;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME tgtName{};
        tgtName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgtName.header.size = sizeof(tgtName);
        tgtName.header.adapterId = p.targetInfo.adapterId;
        tgtName.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgtName.header) != ERROR_SUCCESS) {
            continue;
        }

        DisplayMetaEntry entry;
        entry.gdiDeviceName = gdi;
        entry.monitorDevicePath = BoundedWideString(
            tgtName.monitorDevicePath, ARRAYSIZE(tgtName.monitorDevicePath));
        entry.friendlyName = BoundedWideString(
            tgtName.monitorFriendlyDeviceName, ARRAYSIZE(tgtName.monitorFriendlyDeviceName));

        const bool alreadyHasGdi = std::any_of(entries.begin(), entries.end(), [&](const DisplayMetaEntry& e) {
            return EqualsIgnoreCase(e.gdiDeviceName, gdi);
        });
        if (!alreadyHasGdi) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

std::wstring FallbackFriendlyName(const wchar_t* deviceName) {
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    if (EnumDisplayDevicesW(deviceName, 0, &adapter, 0)) {
        std::wstring s = BoundedWideString(adapter.DeviceString, ARRAYSIZE(adapter.DeviceString));
        if (!s.empty()) {
            return s;
        }
    }
    std::wstring dev(deviceName);
    if (dev.rfind(L"\\\\.\\", 0) == 0) {
        return dev.substr(4);
    }
    return dev;
}

struct TaskbarWindowEntry {
    HMONITOR monitor = nullptr;
    HWND hwnd = nullptr;
    bool isPrimary = false;
};

struct MonitorEnumCtx {
    std::vector<HMONITOR> monitors;
};

BOOL CALLBACK CollectMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* ctx = reinterpret_cast<MonitorEnumCtx*>(lParam);
    if (ctx && hMon) {
        ctx->monitors.push_back(hMon);
    }
    return TRUE;
}

bool IsIgnoredForegroundClass(HWND hwnd) {
    wchar_t cls[128]{};
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) == 0) {
        return true;
    }
    return wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           wcscmp(cls, L"Progman") == 0 ||
           wcscmp(cls, L"WorkerW") == 0;
}

// Estado global calculado uma vez por enumeracao.
struct ForegroundState {
    HWND fg = nullptr;
    HMONITOR fgMonitor = nullptr;
    bool systemFullscreen = false;  // D3D exclusivo ou modo apresentacao
};

ForegroundState QueryForegroundState() {
    ForegroundState st;
    st.fg = GetForegroundWindow();
    if (st.fg) {
        st.fgMonitor = MonitorFromWindow(st.fg, MONITOR_DEFAULTTONULL);
    }
    QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&quns))) {
        st.systemFullscreen = (quns == QUNS_RUNNING_D3D_FULL_SCREEN || quns == QUNS_PRESENTATION_MODE);
    }
    return st;
}

bool IsMonitorFullscreenCovered(
    HMONITOR mon,
    const MONITORINFO& mi,
    bool isPrimary,
    const ForegroundState& fgState,
    HWND ownFlyoutHwnd,
    const std::vector<HWND>& ownBarHwnds) {
    // SHQueryUserNotificationState e global: atribui o tela cheia ao monitor da janela
    // em primeiro plano (ou ao principal, se nao der para saber), nao a todos.
    if (fgState.systemFullscreen) {
        if (fgState.fgMonitor ? (fgState.fgMonitor == mon) : isPrimary) {
            return true;
        }
    }

    HWND fg = fgState.fg;
    if (!fg || !IsWindowVisible(fg) || IsIconic(fg)) {
        return false;
    }
    if (fg == ownFlyoutHwnd ||
        std::find(ownBarHwnds.begin(), ownBarHwnds.end(), fg) != ownBarHwnds.end()) {
        return false;
    }
    if (IsIgnoredForegroundClass(fg)) {
        return false;
    }
    if (fgState.fgMonitor != mon) {
        return false;
    }

    // Limites visiveis (sem as bordas invisiveis de redimensionamento que o
    // GetWindowRect inclui no Windows 10/11).
    RECT fgRect{};
    if (FAILED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &fgRect, sizeof(fgRect))) &&
        !GetWindowRect(fg, &fgRect)) {
        return false;
    }

    const bool coversMonitor =
        fgRect.left <= mi.rcMonitor.left + 2 &&
        fgRect.top <= mi.rcMonitor.top + 2 &&
        fgRect.right >= mi.rcMonitor.right - 2 &&
        fgRect.bottom >= mi.rcMonitor.bottom - 2;

    if (!coversMonitor) {
        return false;
    }

    const LONG style = GetWindowLongW(fg, GWL_STYLE);
    const bool hasCaption = (style & WS_CAPTION) == WS_CAPTION;
    if (hasCaption && IsZoomed(fg)) {
        // Janela maximizada comum (mesmo com a barra em ocultacao automatica) nao e tela cheia.
        return false;
    }
    const bool taskbarReservesWorkArea =
        (mi.rcWork.bottom < mi.rcMonitor.bottom) ||
        (mi.rcWork.top > mi.rcMonitor.top) ||
        (mi.rcWork.left > mi.rcMonitor.left) ||
        (mi.rcWork.right < mi.rcMonitor.right);

    // A regular maximized window stops at rcWork; true fullscreen covers rcMonitor (or lacks caption).
    return !hasCaption || taskbarReservesWorkArea;
}

TaskbarInfo BuildDisplayTaskbarInfo(
    HMONITOR hMon,
    const TaskbarWindowEntry* tbWin,
    const std::vector<DisplayMetaEntry>& metas,
    const ForegroundState& fgState,
    HWND ownFlyoutHwnd,
    const std::vector<HWND>& ownBarHwnds) {
    TaskbarInfo info;
    info.monitor = hMon;

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) {
        return info;  // monitor sumiu durante a enumeracao: isVisible fica false
    }
    info.monitorRect = mi.rcMonitor;
    info.workRect = mi.rcWork;
    info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    info.deviceName = BoundedWideString(mi.szDevice, ARRAYSIZE(mi.szDevice));

    for (const auto& m : metas) {
        if (EqualsIgnoreCase(m.gdiDeviceName, info.deviceName)) {
            info.monitorId = m.monitorDevicePath;
            info.friendlyName = m.friendlyName;
            break;
        }
    }
    if (info.monitorId.empty()) {
        info.monitorId = info.deviceName;
    }
    if (info.friendlyName.empty()) {
        info.friendlyName = FallbackFriendlyName(info.deviceName.c_str());
    }

    UINT dpiX = 96;
    UINT dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0) {
        info.dpi = dpiX;
    }

    if (tbWin && tbWin->hwnd) {
        info.taskbarHwnd = tbWin->hwnd;
        if (tbWin->isPrimary) {
            info.isPrimary = true;
        }
        GetWindowRect(tbWin->hwnd, &info.taskbarRect);

        RECT inter{};
        const bool intersects = IntersectRect(&inter, &info.taskbarRect, &info.monitorRect) != FALSE;
        const int interW = inter.right - inter.left;
        const int interH = inter.bottom - inter.top;
        info.isVisible = IsWindowVisible(tbWin->hwnd) && intersects && interW >= 16 && interH >= 16;

        HWND trayNotify = nullptr;
        if (tbWin->isPrimary) {
            trayNotify = FindWindowExW(tbWin->hwnd, nullptr, L"TrayNotifyWnd", nullptr);
        } else {
            trayNotify = FindWindowExW(tbWin->hwnd, nullptr, L"ClockButton", nullptr);
            if (!trayNotify) {
                trayNotify = FindWindowExW(tbWin->hwnd, nullptr, L"TrayNotifyWnd", nullptr);
            }
        }
        if (trayNotify && IsWindowVisible(trayNotify)) {
            RECT tr{};
            if (GetWindowRect(trayNotify, &tr) &&
                tr.left > info.taskbarRect.left &&
                tr.right > tr.left &&
                tr.right <= info.taskbarRect.right + 4) {
                info.trayNotifyRect = tr;
                info.hasTrayNotifyRect = true;
            }
        }
    } else {
        // Display sem barra do Windows (ex.: "Mostrar barra em todos os monitores" desligado):
        // ancora numa faixa sintetica na borda inferior do monitor.
        const int barH = std::max(16, MulDiv(48, static_cast<int>(info.dpi), 96));
        info.taskbarRect = RECT{
            info.monitorRect.left,
            std::max(info.monitorRect.top, info.monitorRect.bottom - barH),
            info.monitorRect.right,
            info.monitorRect.bottom
        };
        info.isVisible = true;
    }

    MONITORINFO baseMi{};
    baseMi.cbSize = sizeof(baseMi);
    baseMi.rcMonitor = mi.rcMonitor;
    baseMi.rcWork = mi.rcWork;
    baseMi.dwFlags = mi.dwFlags;
    info.isFullscreenCovered = IsMonitorFullscreenCovered(
        info.monitor, baseMi, info.isPrimary, fgState, ownFlyoutHwnd, ownBarHwnds);

    return info;
}

} // namespace

bool MatchesMonitorId(const TaskbarInfo& tb, const std::wstring& monitorId) {
    if (monitorId.empty()) {
        return false;
    }
    if (!tb.monitorId.empty() && EqualsIgnoreCase(tb.monitorId, monitorId)) {
        return true;
    }
    return !tb.deviceName.empty() && EqualsIgnoreCase(tb.deviceName, monitorId);
}

std::vector<TaskbarInfo> EnumerateAllDisplays(
    HWND ownFlyoutHwnd,
    const std::vector<HWND>& ownBarHwnds) {
    const ForegroundState fgState = QueryForegroundState();
    const std::vector<DisplayMetaEntry> metas = QueryDisplayMetadata();

    std::vector<TaskbarWindowEntry> tbWins;
    if (HWND primary = FindWindowExW(nullptr, nullptr, L"Shell_TrayWnd", nullptr)) {
        tbWins.push_back({MonitorFromWindow(primary, MONITOR_DEFAULTTONEAREST), primary, true});
    }
    HWND sec = nullptr;
    while ((sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        tbWins.push_back({MonitorFromWindow(sec, MONITOR_DEFAULTTONEAREST), sec, false});
    }

    MonitorEnumCtx enumCtx;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorProc, reinterpret_cast<LPARAM>(&enumCtx));

    std::vector<TaskbarInfo> result;
    result.reserve(enumCtx.monitors.size());
    for (HMONITOR hMon : enumCtx.monitors) {
        const TaskbarWindowEntry* matchTb = nullptr;
        for (const auto& entry : tbWins) {
            if (entry.monitor == hMon) {
                matchTb = &entry;
                break;
            }
        }
        TaskbarInfo info = BuildDisplayTaskbarInfo(hMon, matchTb, metas, fgState, ownFlyoutHwnd, ownBarHwnds);
        if (!info.deviceName.empty()) {
            result.push_back(std::move(info));
        }
    }

    std::sort(result.begin(), result.end(), [](const TaskbarInfo& a, const TaskbarInfo& b) {
        if (a.isPrimary != b.isPrimary) return a.isPrimary > b.isPrimary;
        if (a.monitorRect.left != b.monitorRect.left) return a.monitorRect.left < b.monitorRect.left;
        if (a.monitorRect.top != b.monitorRect.top) return a.monitorRect.top < b.monitorRect.top;
        return a.deviceName < b.deviceName;
    });
    return result;
}

std::vector<TaskbarInfo> EnumerateTaskbars(
    bool allMonitors,
    const std::wstring& selectedMonitorId,
    HWND ownFlyoutHwnd,
    const std::vector<HWND>& ownBarHwnds) {
    std::vector<TaskbarInfo> displays = EnumerateAllDisplays(ownFlyoutHwnd, ownBarHwnds);
    std::vector<TaskbarInfo> result;

    if (!selectedMonitorId.empty()) {
        for (auto& d : displays) {
            if (MatchesMonitorId(d, selectedMonitorId)) {
                result.push_back(std::move(d));
                break;
            }
        }
        return result;
    }

    if (allMonitors) {
        for (auto& d : displays) {
            if (d.taskbarHwnd != nullptr) {
                result.push_back(std::move(d));
            }
        }
        if (result.empty()) {
            return displays;
        }
        return result;
    }

    for (auto& d : displays) {
        if (d.isPrimary) {
            result.push_back(std::move(d));
            return result;
        }
    }
    for (auto& d : displays) {
        if (d.taskbarHwnd != nullptr) {
            result.push_back(std::move(d));
            return result;
        }
    }
    return result;
}
