#pragma once

#include "config.h"
#include "d2d_layered.h"
#include "metrics.h"
#include "taskbar_tracker.h"
#include "theme.h"
#include <windows.h>
#include <cstdint>
#include <string>

class FlyoutWindow {
public:
    static constexpr UINT WM_FLYOUT_OPEN_MENU = WM_APP + 20;
    static constexpr UINT WM_FLYOUT_OPEN_TASKMGR = WM_APP + 21;
    static constexpr UINT WM_FLYOUT_TOGGLE_PIN = WM_APP + 22;

    static constexpr float kWidthDip = 326.0f;

    FlyoutWindow() = default;
    ~FlyoutWindow();

    FlyoutWindow(const FlyoutWindow&) = delete;
    FlyoutWindow& operator=(const FlyoutWindow&) = delete;

    static bool RegisterWindowClass(HINSTANCE hInst);

    bool Create(HINSTANCE hInst, HWND controllerHwnd);
    void Destroy();

    HWND Hwnd() const { return hwnd_; }
    bool IsVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    bool WasJustDeactivated() const;
    std::wstring TakePendingTogglePinId();

    void ShowAnchored(
        D2DResources& res,
        const TaskbarInfo& tb,
        const RECT& anchorRect,
        const SystemSnapshot& snap,
        const AppConfig& cfg,
        const ThemeColors& colors);

    void UpdateIfVisible(
        D2DResources& res,
        const SystemSnapshot& snap,
        const AppConfig& cfg,
        const ThemeColors& colors);

    void Hide();

    static float ComputeContentHeightDip(const SystemSnapshot& snap);
    static float ComputeDesiredHeightDip(const SystemSnapshot& snap, float maxHeightDip);
    static float MaxHeightForWorkArea(const RECT& workRect, UINT dpi);

    // Desenha e fecha o EndDraw; retorna false se nada foi desenhado.
    static bool RenderToSurface(
        D2DResources& res,
        D2DSurface& surf,
        float widthDip,
        float heightDip,
        UINT dpi,
        const SystemSnapshot& snap,
        const AppConfig& cfg,
        const ThemeColors& colors,
        float scrollDip,
        int hoveredCard,
        int hoveredPinCard,
        int selectedCard,
        int hoverHeaderBtn,
        float& outContentHeightDip);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void ApplyDwmFrameStyle(bool isLight);
    void RedrawNow();
    void ClampSelection(const SystemSnapshot& snap);
    bool FitHeightToContent(const SystemSnapshot& snap);

    HWND hwnd_ = nullptr;
    HWND controllerHwnd_ = nullptr;
    D2DSurface surface_;

    UINT currentDpi_ = 96;
    float currentWidthDip_ = kWidthDip;
    float currentHeightDip_ = 400.0f;
    float contentHeightDip_ = 400.0f;
    float scrollDip_ = 0.0f;
    float maxHeightDip_ = 580.0f;
    bool anchoredAbove_ = true;

    int hoveredCard_ = -1;
    int hoveredPinCard_ = -1;
    int selectedCard_ = 0;
    int hoverHeaderBtn_ = 0; // 0=none, 1=taskmgr, 2=menu
    bool trackingMouse_ = false;
    bool leftDown_ = false;
    bool rightDown_ = false;
    bool anchoring_ = false;
    uint64_t lastDeactivateTick_ = 0;
    std::wstring pendingTogglePinId_;

    D2DResources* lastRes_ = nullptr;
    const SystemSnapshot* lastSnap_ = nullptr;
    const AppConfig* lastCfg_ = nullptr;
    ThemeColors lastColors_{};
    bool appliedThemeKnown_ = false;
    bool appliedLightTheme_ = false;
};
