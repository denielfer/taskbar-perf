#pragma once

#include "config.h"
#include "d2d_layered.h"
#include "metrics.h"
#include "taskbar_tracker.h"
#include "theme.h"
#include <windows.h>

class TaskbarBarWindow {
public:
    static constexpr UINT WM_BAR_LEFT_CLICK = WM_APP + 10;
    static constexpr UINT WM_BAR_RIGHT_CLICK = WM_APP + 11;

    TaskbarBarWindow() = default;
    ~TaskbarBarWindow();

    TaskbarBarWindow(const TaskbarBarWindow&) = delete;
    TaskbarBarWindow& operator=(const TaskbarBarWindow&) = delete;

    static bool RegisterWindowClass(HINSTANCE hInst);

    bool Create(HINSTANCE hInst, HWND controllerHwnd, HMONITOR monitor, HWND taskbarHwnd);
    void Destroy();

    HWND Hwnd() const { return hwnd_; }
    HMONITOR Monitor() const { return monitor_; }
    HWND TaskbarHwnd() const { return taskbarHwnd_; }
    const RECT& CurrentBarRect() const { return barRect_; }

    void UpdateAndRender(
        D2DResources& res,
        const TaskbarInfo& tb,
        const SystemSnapshot& snap,
        const AppConfig& cfg,
        const ThemeColors& colors);

    void Hide();
    void EnsureAboveTaskbar();

    static void ComputeLayoutDip(
        const AppConfig& cfg,
        const SystemSnapshot& snap,
        float maxWidthDip,
        float taskbarHeightDip,
        float& outWidthDip,
        float& outHeightDip);

    // Desenha e fecha o EndDraw; retorna false se nada foi desenhado.
    static bool RenderToSurface(
        D2DResources& res,
        D2DSurface& surf,
        float widthDip,
        float heightDip,
        UINT dpi,
        const SystemSnapshot& snap,
        const AppConfig& cfg,
        bool hovered,
        const ThemeColors& colors);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void RedrawHover(bool hovered);

    HWND hwnd_ = nullptr;
    HWND controllerHwnd_ = nullptr;
    HMONITOR monitor_ = nullptr;
    HWND taskbarHwnd_ = nullptr;
    D2DSurface surface_;
    RECT barRect_{};
    UINT currentDpi_ = 96;
    bool isHovered_ = false;
    bool trackingMouse_ = false;
    bool leftDown_ = false;
    bool rightDown_ = false;

    const SystemSnapshot* lastSnap_ = nullptr;
    const AppConfig* lastCfg_ = nullptr;
    D2DResources* lastRes_ = nullptr;
    ThemeColors lastColors_{};
    float lastWidthDip_ = 0.0f;
    float lastHeightDip_ = 0.0f;
};
