#include "taskbar_bar.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kBarClassName[] = L"TaskbarPerfBarWnd";

struct ResolvedBarTile {
    float baseWidthDip = 78.0f;
    D2D1_COLOR_F color{};
    D2D1_COLOR_F secondaryColor{};
    const RingBuffer<60>* series = nullptr;
    const RingBuffer<60>* secondary = nullptr;
    double maxScale = 100.0;
    std::wstring label;
    std::wstring value;
    std::wstring value2;
};

std::vector<ResolvedBarTile> ResolvePinnedBarTiles(
    const AppConfig& cfg,
    const SystemSnapshot& snap,
    const ThemeColors& colors) {
    std::vector<ResolvedBarTile> tiles;
    wchar_t buf[64]{};

    auto makeCpuTile = [&]() {
        ResolvedBarTile t;
        t.baseWidthDip = 80.0f;
        t.color = colors.cpu;
        t.series = &snap.cpu.history;
        t.maxScale = 100.0;
        t.label = L"CPU";
        swprintf_s(buf, L"%.0f%%", snap.cpu.utilizationPct);
        t.value = buf;
        return t;
    };

    if (cfg.IsPinned(L"cpu")) {
        tiles.push_back(makeCpuTile());
    }

    if (cfg.IsPinned(L"ram")) {
        ResolvedBarTile t;
        t.baseWidthDip = 98.0f;
        t.color = colors.ram;
        t.series = &snap.ram.history;
        t.maxScale = 100.0;
        t.label = L"RAM";
        t.value = FormatDecimalComma(snap.ram.usedGb, 1) + L" GB";
        tiles.push_back(std::move(t));
    }

    for (const auto& g : snap.gpus) {
        if (!cfg.IsPinned(PinIdForGpu(g))) continue;

        ResolvedBarTile t;
        t.baseWidthDip = 102.0f;
        t.color = colors.gpu;
        t.series = &g.history;
        t.maxScale = 100.0;
        t.label = (snap.gpus.size() > 1) ? (L"GPU " + std::to_wstring(g.index)) : L"GPU";
        if (g.tempC > 0.0) {
            swprintf_s(buf, L"%.0f%% %.0f\u00B0", g.utilizationPct, g.tempC);
        } else {
            swprintf_s(buf, L"%.0f%%", g.utilizationPct);
        }
        t.value = buf;
        t.value2 = (g.vramUsedGb >= 0.0) ? FormatDecimalComma(g.vramUsedGb, 1) + L" GB" : L"-- GB";
        tiles.push_back(std::move(t));
    }

    for (const auto& d : snap.disks) {
        if (!cfg.IsPinned(PinIdForDisk(d))) continue;

        ResolvedBarTile t;
        t.baseWidthDip = 116.0f;
        t.color = colors.disk;
        t.series = &d.history;
        t.maxScale = 100.0;
        if (!d.driveLetters.empty() && d.driveLetters.find(L' ') == std::wstring::npos) {
            t.label = L"Disco " + d.driveLetters;
        } else {
            t.label = L"Disco " + std::to_wstring(d.index);
        }
        t.value = L"L: " + FormatDiskSpeedMBs(d.readMBs);
        t.value2 = L"E: " + FormatDiskSpeedMBs(d.writeMBs);
        tiles.push_back(std::move(t));
    }

    for (const auto& n : snap.nets) {
        if (!cfg.IsPinned(PinIdForNet(n))) continue;

        ResolvedBarTile t;
        t.baseWidthDip = 116.0f;
        t.color = colors.netRecv;
        t.secondaryColor = colors.netSend;
        t.series = &n.recvHistory;
        t.secondary = &n.sendHistory;
        t.maxScale = n.maxScaleBps;
        t.label = n.name;
        t.value = L"\u2193 " + FormatDetailedBps(n.recvBps);
        t.value2 = L"\u2191 " + FormatDetailedBps(n.sendBps);
        tiles.push_back(std::move(t));
    }

    if (tiles.empty()) {
        tiles.push_back(makeCpuTile());
    }
    return tiles;
}

} // namespace

TaskbarBarWindow::~TaskbarBarWindow() {
    Destroy();
}

bool TaskbarBarWindow::RegisterWindowClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kBarClassName;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool TaskbarBarWindow::Create(HINSTANCE hInst, HWND controllerHwnd, HMONITOR monitor, HWND taskbarHwnd) {
    controllerHwnd_ = controllerHwnd;
    monitor_ = monitor;
    taskbarHwnd_ = taskbarHwnd;

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kBarClassName,
        L"TaskbarPerfBar",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr,
        nullptr,
        hInst,
        this);

    return hwnd_ != nullptr;
}

void TaskbarBarWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);  // WM_NCDESTROY zera hwnd_ e o ponteiro da janela
        hwnd_ = nullptr;
    }
    surface_.Release();
}

void TaskbarBarWindow::Hide() {
    isHovered_ = false;
    trackingMouse_ = false;
    leftDown_ = false;
    rightDown_ = false;
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void TaskbarBarWindow::EnsureAboveTaskbar() {
    if (!hwnd_ || !taskbarHwnd_ || !IsWindowVisible(hwnd_)) {
        return;
    }
    HWND cur = hwnd_;
    for (int i = 0; i < 64 && cur != nullptr; ++i) {
        cur = GetWindow(cur, GW_HWNDPREV);
        if (cur == taskbarHwnd_) {
            SetWindowPos(
                hwnd_,
                HWND_TOPMOST,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            break;
        }
    }
}

void TaskbarBarWindow::ComputeLayoutDip(
    const AppConfig& cfg,
    const SystemSnapshot& snap,
    float maxWidthDip,
    float taskbarHeightDip,
    float& outWidthDip,
    float& outHeightDip) {
    const ThemeColors dummyColors{};
    const auto tiles = ResolvePinnedBarTiles(cfg, snap, dummyColors);
    constexpr float kPadX = 8.0f;
    constexpr float kGap = 6.0f;

    float sum = kPadX * 2.0f + kGap * static_cast<float>(tiles.size() - 1);
    for (const auto& t : tiles) {
        sum += t.baseWidthDip;
    }

    if (maxWidthDip > 80.0f && sum > maxWidthDip) {
        sum = maxWidthDip;
    }

    outWidthDip = std::max(72.0f, sum);
    outHeightDip = std::clamp(taskbarHeightDip - 6.0f, 32.0f, 44.0f);
}

bool TaskbarBarWindow::RenderToSurface(
    D2DResources& res,
    D2DSurface& surf,
    float widthDip,
    float heightDip,
    UINT dpi,
    const SystemSnapshot& snap,
    const AppConfig& cfg,
    bool hovered,
    const ThemeColors& colors) {
    const int physW = std::max(1, MulDiv(static_cast<int>(widthDip + 0.5f), static_cast<int>(dpi), 96));
    const int physH = std::max(1, MulDiv(static_cast<int>(heightDip + 0.5f), static_cast<int>(dpi), 96));

    if (!surf.Resize(res, physW, physH, dpi)) {
        return false;
    }
    if (!surf.BeginDraw(res, D2D1::ColorF(0, 0, 0, 0))) {
        return false;
    }

    ID2D1DCRenderTarget* rt = surf.RT();
    const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, widthDip - 0.5f, heightDip - 0.5f),
        6.0f, 6.0f);

    if (auto* bgBrush = surf.Brush(hovered ? colors.barHoverBg : colors.barHitPillBg)) {
        rt->FillRoundedRectangle(pill, bgBrush);
    }

    const auto tiles = ResolvePinnedBarTiles(cfg, snap, colors);
    constexpr float kPadX = 8.0f;
    constexpr float kGap = 6.0f;

    float rawTileSum = 0.0f;
    for (const auto& t : tiles) rawTileSum += t.baseWidthDip;
    const float availTileSpace = std::max(40.0f, widthDip - kPadX * 2.0f - kGap * static_cast<float>(tiles.size() - 1));
    const float scale = (rawTileSum > 0.0f) ? std::min(1.0f, availTileSpace / rawTileSum) : 1.0f;

    const float sparkY = 3.0f;
    const float sparkH = std::max(20.0f, heightDip - 6.0f);

    float x = kPadX;
    for (const auto& tile : tiles) {
        const float tw = tile.baseWidthDip * scale;
        // O grafico nunca ocupa mais que ~45% do bloco, senao invade o vizinho quando a barra encolhe.
        const float sparkW = std::min(std::clamp(sparkH * scale, 20.0f, 38.0f), std::max(10.0f, tw * 0.45f));
        const D2D1_RECT_F sparkRect = D2D1::RectF(x, sparkY, x + sparkW, sparkY + sparkH);

        SparklineStyle st{};
        st.boxBg = colors.barBoxBg;
        st.boxBorder = colors.barBoxBorder;
        st.strokeWidth = 1.25f;
        st.fillAlpha = 0.28f;
        st.drawGrid = false;
        st.lineColor = tile.color;
        if (tile.secondary) {
            st.secondary = tile.secondary;
            st.secondaryColor = tile.secondaryColor;
            st.secondaryStrokeStyle = res.DashedStroke();
        }
        if (tile.series) {
            DrawSparkline(res, surf, sparkRect, *tile.series, tile.maxScale, st);
        }

        const float textLeft = x + sparkW + 5.0f;
        const float textRight = x + tw;
        if (textRight > textLeft + 10.0f) {
            if (tile.value2.empty()) {
                const float midY = sparkY + sparkH * 0.5f;
                const D2D1_RECT_F labelRect = D2D1::RectF(textLeft, sparkY + 2.0f, textRight, midY + 1.0f);
                const D2D1_RECT_F valueRect = D2D1::RectF(textLeft, midY - 1.0f, textRight, sparkY + sparkH - 2.0f);
                if (auto* bMuted = surf.Brush(colors.barTextMuted)) {
                    rt->DrawText(tile.label.c_str(), static_cast<UINT32>(tile.label.size()), res.FmtBarLabel(), labelRect, bMuted);
                }
                if (auto* bPrim = surf.Brush(colors.barTextPrimary)) {
                    rt->DrawText(tile.value.c_str(), static_cast<UINT32>(tile.value.size()), res.FmtBarValue(), valueRect, bPrim);
                }
            } else {
                const float top = 1.5f;
                const float bot = heightDip - 1.5f;
                const float availH = std::max(24.0f, bot - top);
                const float h1 = availH * 0.31f;
                const float h2 = availH * 0.345f;
                const D2D1_RECT_F r1 = D2D1::RectF(textLeft, top, textRight, top + h1);
                const D2D1_RECT_F r2 = D2D1::RectF(textLeft, top + h1 - 0.5f, textRight, top + h1 + h2);
                const D2D1_RECT_F r3 = D2D1::RectF(textLeft, top + h1 + h2 - 0.5f, textRight, bot);
                if (auto* bMuted = surf.Brush(colors.barTextMuted)) {
                    rt->DrawText(tile.label.c_str(), static_cast<UINT32>(tile.label.size()), res.FmtBarSmallLabel(), r1, bMuted);
                }
                if (auto* bPrim = surf.Brush(colors.barTextPrimary)) {
                    rt->DrawText(tile.value.c_str(), static_cast<UINT32>(tile.value.size()), res.FmtBarSmallValue(), r2, bPrim);
                    rt->DrawText(tile.value2.c_str(), static_cast<UINT32>(tile.value2.size()), res.FmtBarSmallValue(), r3, bPrim);
                }
            }
        }

        x += tw + kGap;
    }
    return surf.EndDraw();
}

void TaskbarBarWindow::UpdateAndRender(
    D2DResources& res,
    const TaskbarInfo& tb,
    const SystemSnapshot& snap,
    const AppConfig& cfg,
    const ThemeColors& colors) {
    if (!hwnd_) return;

    monitor_ = tb.monitor;
    taskbarHwnd_ = tb.taskbarHwnd;
    currentDpi_ = tb.dpi;
    lastRes_ = &res;
    lastSnap_ = &snap;
    lastCfg_ = &cfg;
    lastColors_ = colors;

    const int tbPhysW = tb.taskbarRect.right - tb.taskbarRect.left;
    const int tbPhysH = tb.taskbarRect.bottom - tb.taskbarRect.top;
    if (!tb.isVisible || tbPhysW <= 64 || tbPhysH <= 16) {
        Hide();
        return;
    }

    const float tbDipW = static_cast<float>(tbPhysW) * 96.0f / static_cast<float>(tb.dpi);
    const float tbDipH = static_cast<float>(tbPhysH) * 96.0f / static_cast<float>(tb.dpi);
    const float maxDipW = tbDipW * 0.48f;

    float widthDip = 0.0f;
    float heightDip = 0.0f;
    ComputeLayoutDip(cfg, snap, maxDipW, tbDipH, widthDip, heightDip);
    lastWidthDip_ = widthDip;
    lastHeightDip_ = heightDip;

    // RenderToSurface ja fecha o EndDraw: nenhuma mensagem enviada daqui em diante
    // encontra um BeginDraw aberto.
    if (!RenderToSurface(res, surface_, widthDip, heightDip, tb.dpi, snap, cfg, isHovered_, colors)) {
        return;
    }

    const int barPhysW = surface_.Width();
    const int barPhysH = surface_.Height();
    const int marginPhys = MulDiv(8, static_cast<int>(tb.dpi), 96);

    int dstX = tb.taskbarRect.left + marginPhys;
    if (cfg.alignRight) {
        if (tb.hasTrayNotifyRect) {
            dstX = tb.trayNotifyRect.left - barPhysW - marginPhys;
        } else {
            dstX = tb.taskbarRect.right - barPhysW - MulDiv(110, static_cast<int>(tb.dpi), 96);
        }
        dstX = std::max<int>(tb.taskbarRect.left + marginPhys, dstX);
    }
    const int dstY = tb.taskbarRect.top + (tbPhysH - barPhysH) / 2;

    barRect_ = RECT{dstX, dstY, dstX + barPhysW, dstY + barPhysH};

    // UpdateLayeredWindow ja move/redimensiona; so mostra depois que ha conteudo
    // (evita um quadro vazio). A janela ja nasce WS_EX_TOPMOST, entao nao e preciso
    // reafirmar HWND_TOPMOST a cada tick: so sobe se a barra do Windows ficou por cima.
    if (!surface_.PresentLayered(hwnd_, POINT{dstX, dstY})) {
        return;
    }
    if (!IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
    EnsureAboveTaskbar();
}

void TaskbarBarWindow::RedrawHover(bool hovered) {
    if (!lastRes_ || !lastSnap_ || !lastCfg_ || lastWidthDip_ <= 0.0f) {
        return;
    }
    if (RenderToSurface(*lastRes_, surface_, lastWidthDip_, lastHeightDip_, currentDpi_, *lastSnap_, *lastCfg_, hovered, lastColors_)) {
        surface_.PresentLayered(hwnd_, POINT{barRect_.left, barRect_.top});
    }
}

LRESULT CALLBACK TaskbarBarWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<TaskbarBarWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
        return DefWindowProcW(hwnd, msg, wParam, lParam);  // grava o titulo da janela
    }

    auto* self = reinterpret_cast<TaskbarBarWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self) self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TaskbarBarWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEMOVE:
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            trackingMouse_ = true;
        }
        if (!isHovered_) {
            isHovered_ = true;
            RedrawHover(true);
        }
        return 0;

    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        leftDown_ = false;
        rightDown_ = false;
        if (isHovered_) {
            isHovered_ = false;
            RedrawHover(false);
        }
        return 0;

    // So conta como clique se o botao foi pressionado sobre a barra (soltar um
    // arraste vindo de outra janela nao abre o painel).
    case WM_LBUTTONDOWN:
        leftDown_ = true;
        return 0;

    case WM_RBUTTONDOWN:
        rightDown_ = true;
        return 0;

    case WM_LBUTTONUP:
        if (leftDown_ && controllerHwnd_) {
            PostMessageW(controllerHwnd_, WM_BAR_LEFT_CLICK, reinterpret_cast<WPARAM>(hwnd_), 0);
        }
        leftDown_ = false;
        return 0;

    case WM_RBUTTONUP:
        if (rightDown_ && controllerHwnd_) {
            POINT pt{};
            GetCursorPos(&pt);
            PostMessageW(controllerHwnd_, WM_BAR_RIGHT_CLICK, reinterpret_cast<WPARAM>(hwnd_), MAKELPARAM(pt.x, pt.y));
        }
        rightDown_ = false;
        return 0;

    case WM_NCHITTEST:
        return HTCLIENT;
    }

    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}
