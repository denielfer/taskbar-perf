#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct TaskbarInfo {
    HWND taskbarHwnd = nullptr;
    HMONITOR monitor = nullptr;
    std::wstring monitorId;
    std::wstring deviceName;
    std::wstring friendlyName;
    RECT taskbarRect{};
    RECT monitorRect{};
    RECT workRect{};
    RECT trayNotifyRect{};
    bool hasTrayNotifyRect = false;
    UINT dpi = 96;
    bool isPrimary = false;
    bool isVisible = false;
    bool isFullscreenCovered = false;
};

bool MatchesMonitorId(const TaskbarInfo& tb, const std::wstring& monitorId);

// Os nomes/IDs dos monitores (QueryDisplayConfig) ficam em cache; invalidar em WM_DISPLAYCHANGE.
void InvalidateDisplayMetadataCache();

// detectFullscreen = false pula a deteccao de tela cheia (isFullscreenCovered fica false).
std::vector<TaskbarInfo> EnumerateAllDisplays(
    HWND ownFlyoutHwnd,
    const std::vector<HWND>& ownBarHwnds,
    bool detectFullscreen = true);

std::vector<TaskbarInfo> EnumerateTaskbars(
    bool allMonitors,
    const std::wstring& selectedMonitorId,
    HWND ownFlyoutHwnd,
    const std::vector<HWND>& ownBarHwnds,
    bool detectFullscreen = true);
