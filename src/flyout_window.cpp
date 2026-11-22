#include "flyout_window.h"

#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

namespace {

constexpr wchar_t kFlyoutClassName[] = L"TaskbarPerfFlyoutWnd";
constexpr float kHeaderH = 40.0f;
constexpr float kPadX = 8.0f;
constexpr float kPadY = 8.0f;
constexpr float kCardH = 62.0f;
constexpr float kCardGap = 4.0f;

// Header button hit rects in DIPs (width = 326)
const D2D1_RECT_F kTaskMgrBtnRect = D2D1::RectF(196.0f, 8.0f, 286.0f, 32.0f);
const D2D1_RECT_F kMenuBtnRect = D2D1::RectF(290.0f, 8.0f, 318.0f, 32.0f);

std::wstring FmtComma(double val, int decimals) {
    return FormatDecimalComma(val, decimals);
}

void DrawLiteral(ID2D1RenderTarget* rt, std::wstring_view text, IDWriteTextFormat* fmt,
                 const D2D1_RECT_F& rect, ID2D1Brush* brush) {
    rt->DrawText(text.data(), static_cast<UINT32>(text.size()), fmt, rect, brush);
}

bool PtInDipRect(float x, float y, const D2D1_RECT_F& r) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

D2D1_RECT_F ComputeCardPinRect(const D2D1_RECT_F& cardRect) {
    return D2D1::RectF(cardRect.right - 28.0f, cardRect.top + 6.0f, cardRect.right - 6.0f, cardRect.top + 26.0f);
}

size_t CountCards(const SystemSnapshot& snap) {
    return 2 + snap.disks.size() + snap.nets.size() + snap.gpus.size();
}

struct FlyoutCardItem {
    std::wstring id;
    bool pinned = false;
    D2D1_COLOR_F color{};
    D2D1_COLOR_F secondaryColor{};
    const RingBuffer<60>* series = nullptr;
    const RingBuffer<60>* secondary = nullptr;
    double maxScale = 100.0;
    std::wstring line1;
    std::wstring line2;
    std::wstring line3;
};

std::vector<FlyoutCardItem> BuildCardItems(
    const SystemSnapshot& snap,
    const AppConfig& cfg,
    const ThemeColors& colors) {
    std::vector<FlyoutCardItem> items;
    items.reserve(CountCards(snap));

    // CPU
    {
        FlyoutCardItem it;
        it.id = L"cpu";
        it.pinned = cfg.IsPinned(it.id);
        it.color = colors.cpu;
        it.series = &snap.cpu.history;
        it.maxScale = 100.0;
        it.line1 = L"CPU";
        it.line2 = FmtComma(snap.cpu.utilizationPct, 0) + L"%  " + FmtComma(snap.cpu.speedGhz, 2) + L" GHz";
        items.push_back(std::move(it));
    }

    // RAM
    {
        FlyoutCardItem it;
        it.id = L"ram";
        it.pinned = cfg.IsPinned(it.id);
        it.color = colors.ram;
        it.series = &snap.ram.history;
        it.maxScale = 100.0;
        it.line1 = L"Mem\u00F3ria";
        it.line2 = FmtComma(snap.ram.usedGb, 1) + L"/" + FmtComma(snap.ram.totalGb, 1) +
                   L" GB (" + FmtComma(snap.ram.usedPct, 0) + L"%)";
        items.push_back(std::move(it));
    }

    // Disks
    for (const auto& d : snap.disks) {
        FlyoutCardItem it;
        it.id = PinIdForDisk(d);
        it.pinned = cfg.IsPinned(it.id);
        it.color = colors.disk;
        it.series = &d.history;
        it.maxScale = 100.0;
        it.line1 = d.instanceName;
        it.line2 = d.mediaType;
        if (d.readMBs >= 0.1 || d.writeMBs >= 0.1) {
            it.line3 = FmtComma(d.activePct, 0) + L"%  L:" + FmtComma(d.readMBs, 1) +
                       L" E:" + FmtComma(d.writeMBs, 1) + L" MB/s";
        } else {
            it.line3 = FmtComma(d.activePct, 0) + L"%";
        }
        items.push_back(std::move(it));
    }

    // Networks
    for (const auto& n : snap.nets) {
        FlyoutCardItem it;
        it.id = PinIdForNet(n);
        it.pinned = cfg.IsPinned(it.id);
        it.color = colors.netRecv;
        it.secondaryColor = colors.netSend;
        it.series = &n.recvHistory;
        it.secondary = &n.sendHistory;
        it.maxScale = n.maxScaleBps;
        it.line1 = n.name;
        it.line2 = n.description;
        it.line3 = L"E: " + FormatDetailedBps(n.sendBps) + L"  R: " + FormatDetailedBps(n.recvBps);
        items.push_back(std::move(it));
    }

    // GPUs
    for (const auto& g : snap.gpus) {
        FlyoutCardItem it;
        it.id = PinIdForGpu(g);
        it.pinned = cfg.IsPinned(it.id);
        it.color = colors.gpu;
        it.series = &g.history;
        it.maxScale = 100.0;
        it.line1 = L"GPU " + std::to_wstring(g.index);
        it.line2 = g.name;
        std::wstring l3 = FmtComma(g.utilizationPct, 0) + L"%";
        if (g.tempC > 0.0) {
            l3 += L" (" + FmtComma(g.tempC, 0) + L" \u00B0C)";
        }
        if (g.vramTotalGb > 0.0) {
            if (g.vramUsedGb >= 0.0) {
                l3 += L"  " + FmtComma(g.vramUsedGb, 1) + L"/" + FmtComma(g.vramTotalGb, 1) + L" GB";
            } else {
                l3 += L"  VRAM " + FmtComma(g.vramTotalGb, 1) + L" GB";
            }
        }
        it.line3 = std::move(l3);
        items.push_back(std::move(it));
    }

    return items;
}

std::wstring GetCardIdAtIndex(const SystemSnapshot& snap, int idx) {
    if (idx < 0) return L"";
    if (idx == 0) return L"cpu";
    if (idx == 1) return L"ram";
    size_t cur = 2;
    for (const auto& d : snap.disks) {
        if (static_cast<int>(cur) == idx) return PinIdForDisk(d);
        ++cur;
    }
    for (const auto& n : snap.nets) {
        if (static_cast<int>(cur) == idx) return PinIdForNet(n);
        ++cur;
    }
    for (const auto& g : snap.gpus) {
        if (static_cast<int>(cur) == idx) return PinIdForGpu(g);
        ++cur;
    }
    return L"";
}

void DrawPinIcon(
    D2DResources& res,
    D2DSurface& surf,
    D2D1_RECT_F rect,
    D2D1_COLOR_F accent,
    D2D1_COLOR_F muted,
    bool pinned,
    bool cardHovered,
    bool pinHovered) {
    ID2D1DCRenderTarget* rt = surf.RT();
    ID2D1PathGeometry* pinGeo = res.PinGeometry();
    if (!rt || !pinGeo) return;

    const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(rect, 4.0f, 4.0f);
    if (pinned) {
        D2D1_COLOR_F pillBg = accent;
        pillBg.a = pinHovered ? 0.34f : 0.20f;
        if (auto* b = surf.Brush(pillBg)) {
            rt->FillRoundedRectangle(pill, b);
        }
        D2D1_COLOR_F pillBorder = accent;
        pillBorder.a = 0.45f;
        if (auto* bb = surf.Brush(pillBorder)) {
            rt->DrawRoundedRectangle(pill, bb, 1.0f);
        }
    } else if (pinHovered) {
        D2D1_COLOR_F pillBg = muted;
        pillBg.a = 0.16f;
        if (auto* b = surf.Brush(pillBg)) {
            rt->FillRoundedRectangle(pill, b);
        }
    }

    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;

    D2D1_COLOR_F iconCol = pinned ? accent : muted;
    if (!pinned) {
        iconCol.a = pinHovered ? 0.95f : (cardHovered ? 0.65f : 0.30f);
    }

    // Geometria criada uma vez na origem; aqui so posiciona (e inclina se nao fixado).
    D2D1_MATRIX_3X2_F xf = D2D1::Matrix3x2F::Translation(cx, cy);
    if (!pinned) {
        xf = D2D1::Matrix3x2F::Rotation(38.0f) * xf;
    }
    rt->SetTransform(xf);
    if (auto* b = surf.Brush(iconCol)) {
        if (pinned) {
            rt->FillGeometry(pinGeo, b);
        } else {
            rt->DrawGeometry(pinGeo, b, 1.15f);
        }
        rt->DrawLine(D2D1::Point2F(0.0f, 1.2f), D2D1::Point2F(0.0f, 5.6f), b, 1.3f);
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

} // namespace

FlyoutWindow::~FlyoutWindow() {
    Destroy();
}

bool FlyoutWindow::RegisterWindowClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kFlyoutClassName;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool FlyoutWindow::Create(HINSTANCE hInst, HWND controllerHwnd) {
    controllerHwnd_ = controllerHwnd;
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kFlyoutClassName,
        L"Desempenho",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 326, 400,
        nullptr,
        nullptr,
        hInst,
        this);

    if (hwnd_) {
        ApplyDwmFrameStyle(false);
    }
    return hwnd_ != nullptr;
}

void FlyoutWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);  // WM_NCDESTROY zera hwnd_ e o ponteiro da janela
        hwnd_ = nullptr;
    }
    surface_.Release();
}

bool FlyoutWindow::WasJustDeactivated() const {
    return (GetTickCount64() - lastDeactivateTick_) < 220ULL;
}

std::wstring FlyoutWindow::TakePendingTogglePinId() {
    std::wstring out = std::move(pendingTogglePinId_);
    pendingTogglePinId_.clear();
    return out;
}

void FlyoutWindow::Hide() {
    leftDown_ = false;
    rightDown_ = false;
    trackingMouse_ = false;
    hoverHeaderBtn_ = 0;
    hoveredCard_ = -1;
    hoveredPinCard_ = -1;
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void FlyoutWindow::ApplyDwmFrameStyle(bool isLight) {
    if (!hwnd_) return;
    if (appliedThemeKnown_ && appliedLightTheme_ == isLight) {
        return;
    }
    appliedThemeKnown_ = true;
    appliedLightTheme_ = isLight;

    const DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    const BOOL darkMode = isLight ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    const COLORREF borderColor = isLight ? RGB(205, 205, 205) : RGB(62, 62, 66);
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
}

float FlyoutWindow::ComputeContentHeightDip(const SystemSnapshot& snap) {
    const size_t n = CountCards(snap);
    const float cardsH = static_cast<float>(n) * kCardH +
                         static_cast<float>(n > 1 ? n - 1 : 0) * kCardGap;
    return kHeaderH + kPadY * 2.0f + cardsH;
}

float FlyoutWindow::ComputeDesiredHeightDip(const SystemSnapshot& snap, float maxHeightDip) {
    return std::min(ComputeContentHeightDip(snap), std::max(220.0f, maxHeightDip));
}

float FlyoutWindow::MaxHeightForWorkArea(const RECT& workRect, UINT dpi) {
    const float workDipH = static_cast<float>(workRect.bottom - workRect.top) * 96.0f / static_cast<float>(dpi ? dpi : 96);
    return (workDipH > 200.0f) ? (workDipH * 0.78f) : 580.0f;
}

bool FlyoutWindow::RenderToSurface(
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
    float& outContentHeightDip) {
    const int physW = std::max(1, MulDiv(static_cast<int>(widthDip + 0.5f), static_cast<int>(dpi), 96));
    const int physH = std::max(1, MulDiv(static_cast<int>(heightDip + 0.5f), static_cast<int>(dpi), 96));

    if (!surf.Resize(res, physW, physH, dpi)) {
        return false;
    }
    if (!surf.BeginDraw(res, colors.flyoutBg)) {
        return false;
    }

    ID2D1DCRenderTarget* rt = surf.RT();
    const auto items = BuildCardItems(snap, cfg, colors);
    const size_t n = items.size();
    const float cardsH = static_cast<float>(n) * kCardH +
                         static_cast<float>(n > 1 ? n - 1 : 0) * kCardGap;
    outContentHeightDip = kHeaderH + kPadY * 2.0f + cardsH;

    // 1. Scrollable card list clipped below header
    const D2D1_RECT_F bodyClip = D2D1::RectF(0.0f, kHeaderH, widthDip, heightDip);
    rt->PushAxisAlignedClip(bodyClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float y = kHeaderH + kPadY - scrollDip;
    for (size_t i = 0; i < n; ++i) {
        const auto& item = items[i];
        const float cardTop = y;
        const float cardBottom = y + kCardH;
        y += kCardH + kCardGap;

        if (cardBottom < kHeaderH || cardTop > heightDip) {
            continue;
        }

        const D2D1_RECT_F cardRect = D2D1::RectF(kPadX, cardTop, widthDip - kPadX, cardBottom);
        const D2D1_ROUNDED_RECT cardRR = D2D1::RoundedRect(cardRect, 6.0f, 6.0f);
        const bool isCardHovered = (static_cast<int>(i) == hoveredCard);
        const bool isPinHovered = (static_cast<int>(i) == hoveredPinCard);

        D2D1_COLOR_F bgCol = colors.flyoutCardBg;
        if (static_cast<int>(i) == selectedCard) {
            bgCol = colors.flyoutCardSelectedBg;
        } else if (isCardHovered) {
            bgCol = colors.flyoutCardHoverBg;
        }
        if (auto* b = surf.Brush(bgCol)) {
            rt->FillRoundedRectangle(cardRR, b);
        }
        if (auto* bb = surf.Brush(colors.flyoutCardBorder)) {
            rt->DrawRoundedRectangle(cardRR, bb, 1.0f);
        }

        const D2D1_RECT_F sparkRect = D2D1::RectF(
            cardRect.left + 8.0f,
            cardRect.top + 9.0f,
            cardRect.left + 72.0f,
            cardRect.bottom - 9.0f);

        SparklineStyle st{};
        st.lineColor = item.color;
        st.boxBg = colors.flyoutBoxBg;
        st.boxBorder = colors.flyoutBoxBorder;
        st.gridColor = colors.flyoutGridLine;
        st.drawGrid = true;
        st.strokeWidth = 1.35f;
        st.fillAlpha = 0.25f;
        if (item.secondary) {
            st.secondary = item.secondary;
            st.secondaryColor = item.secondaryColor;
            st.secondaryStrokeStyle = res.DashedStroke();
        }
        if (item.series) {
            DrawSparkline(res, surf, sparkRect, *item.series, item.maxScale, st);
        }

        const float txLeft = sparkRect.right + 10.0f;
        const float txTitleRight = cardRect.right - 32.0f;
        const float txSubRight = cardRect.right - 8.0f;

        if (item.line3.empty()) {
            const D2D1_RECT_F r1 = D2D1::RectF(txLeft, cardRect.top + 10.0f, txTitleRight, cardRect.top + 31.0f);
            const D2D1_RECT_F r2 = D2D1::RectF(txLeft, cardRect.top + 31.0f, txSubRight, cardRect.bottom - 10.0f);
            if (auto* bp = surf.Brush(colors.flyoutTextPrimary)) {
                rt->DrawText(item.line1.c_str(), static_cast<UINT32>(item.line1.size()), res.FmtCardTitle(), r1, bp);
            }
            if (auto* bs = surf.Brush(colors.flyoutTextSecondary)) {
                rt->DrawText(item.line2.c_str(), static_cast<UINT32>(item.line2.size()), res.FmtCardSub(), r2, bs);
            }
        } else {
            const D2D1_RECT_F r1 = D2D1::RectF(txLeft, cardRect.top + 5.0f, txTitleRight, cardRect.top + 23.0f);
            const D2D1_RECT_F r2 = D2D1::RectF(txLeft, cardRect.top + 22.0f, txSubRight, cardRect.top + 39.0f);
            const D2D1_RECT_F r3 = D2D1::RectF(txLeft, cardRect.top + 38.0f, txSubRight, cardRect.bottom - 5.0f);
            if (auto* bp = surf.Brush(colors.flyoutTextPrimary)) {
                rt->DrawText(item.line1.c_str(), static_cast<UINT32>(item.line1.size()), res.FmtCardTitle(), r1, bp);
            }
            if (auto* bs = surf.Brush(colors.flyoutTextSecondary)) {
                rt->DrawText(item.line2.c_str(), static_cast<UINT32>(item.line2.size()), res.FmtCardSub(), r2, bs);
                rt->DrawText(item.line3.c_str(), static_cast<UINT32>(item.line3.size()), res.FmtCardSub(), r3, bs);
            }
        }

        const D2D1_RECT_F pinRect = ComputeCardPinRect(cardRect);
        DrawPinIcon(res, surf, pinRect, item.color, colors.flyoutTextSecondary, item.pinned, isCardHovered, isPinHovered);
    }

    // Subtle scrollbar thumb when overflowing
    if (outContentHeightDip > heightDip + 1.0f) {
        const float viewH = heightDip - kHeaderH;
        const float maxScroll = outContentHeightDip - heightDip;
        const float thumbH = std::max(24.0f, viewH * (heightDip / outContentHeightDip));
        const float thumbTravel = std::max(0.0f, viewH - thumbH - 8.0f);
        const float ratio = (maxScroll > 0.0f) ? std::clamp(scrollDip / maxScroll, 0.0f, 1.0f) : 0.0f;
        const float thumbY = kHeaderH + 4.0f + ratio * thumbTravel;
        if (auto* sb = surf.Brush(colors.flyoutScrollThumb)) {
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(widthDip - 5.0f, thumbY, widthDip - 2.0f, thumbY + thumbH), 1.5f, 1.5f),
                sb);
        }
    }

    rt->PopAxisAlignedClip();

    // 2. Header bar
    const D2D1_RECT_F titleRect = D2D1::RectF(14.0f, 4.0f, 190.0f, kHeaderH - 4.0f);
    if (auto* tp = surf.Brush(colors.flyoutTextPrimary)) {
        DrawLiteral(rt, L"Desempenho", res.FmtHeaderTitle(), titleRect, tp);
    }

    if (hoverHeaderBtn == 1) {
        if (auto* hb = surf.Brush(colors.flyoutButtonHover)) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(kTaskMgrBtnRect, 4.0f, 4.0f), hb);
        }
    }
    if (auto* ts = surf.Brush(colors.flyoutTextSecondary)) {
        DrawLiteral(rt, L"Gerenciador", res.FmtHeaderBtn(), kTaskMgrBtnRect, ts);
    }

    if (hoverHeaderBtn == 2) {
        if (auto* hb = surf.Brush(colors.flyoutButtonHover)) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(kMenuBtnRect, 4.0f, 4.0f), hb);
        }
    }
    if (auto* tp = surf.Brush(colors.flyoutTextPrimary)) {
        DrawLiteral(rt, L"\u22EE", res.FmtHeaderBtn(), kMenuBtnRect, tp);
    }

    if (auto* div = surf.Brush(colors.flyoutHeaderBorder)) {
        rt->DrawLine(
            D2D1::Point2F(0.0f, kHeaderH - 0.5f),
            D2D1::Point2F(widthDip, kHeaderH - 0.5f),
            div,
            1.0f);
    }

    return surf.EndDraw();
}

void FlyoutWindow::ShowAnchored(
    D2DResources& res,
    const TaskbarInfo& tb,
    const RECT& anchorRect,
    const SystemSnapshot& snap,
    const AppConfig& cfg,
    const ThemeColors& colors) {
    if (!hwnd_) return;

    lastRes_ = &res;
    lastSnap_ = &snap;
    lastCfg_ = &cfg;
    lastColors_ = colors;
    currentDpi_ = tb.dpi;
    ApplyDwmFrameStyle(colors.isLight);

    maxHeightDip_ = MaxHeightForWorkArea(tb.workRect, tb.dpi);

    currentWidthDip_ = kWidthDip;
    currentHeightDip_ = ComputeDesiredHeightDip(snap, maxHeightDip_);

    const int physW = MulDiv(static_cast<int>(currentWidthDip_ + 0.5f), static_cast<int>(tb.dpi), 96);
    const int physH = MulDiv(static_cast<int>(currentHeightDip_ + 0.5f), static_cast<int>(tb.dpi), 96);
    const int gapPhys = MulDiv(10, static_cast<int>(tb.dpi), 96);

    int x = anchorRect.left;
    const int maxRight = tb.workRect.right - gapPhys;
    if (x + physW > maxRight) {
        x = maxRight - physW;
    }
    x = std::max<int>(tb.workRect.left + gapPhys, x);

    int y = tb.taskbarRect.top - physH - gapPhys;
    anchoredAbove_ = true;
    if (y < tb.workRect.top + gapPhys) {
        anchoredAbove_ = false;
        y = tb.taskbarRect.bottom + gapPhys;
        if (y + physH > tb.workRect.bottom - gapPhys) {
            y = std::max<int>(tb.workRect.top + gapPhys, tb.workRect.bottom - physH - gapPhys);
        }
    }

    scrollDip_ = 0.0f;
    leftDown_ = false;
    rightDown_ = false;
    trackingMouse_ = false;
    hoverHeaderBtn_ = 0;
    hoveredCard_ = -1;
    hoveredPinCard_ = -1;
    ClampSelection(snap);
    if (!RenderToSurface(
            res, surface_, currentWidthDip_, currentHeightDip_, currentDpi_,
            snap, cfg, colors, scrollDip_, hoveredCard_, hoveredPinCard_, selectedCard_, hoverHeaderBtn_, contentHeightDip_)) {
        return;
    }

    anchoring_ = true;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, physW, physH, SWP_SHOWWINDOW);
    anchoring_ = false;
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FlyoutWindow::ClampSelection(const SystemSnapshot& snap) {
    const int count = static_cast<int>(CountCards(snap));
    selectedCard_ = std::clamp(selectedCard_, 0, std::max(0, count - 1));
    if (hoveredCard_ >= count) hoveredCard_ = -1;
    if (hoveredPinCard_ >= count) hoveredPinCard_ = -1;
}

// Discos/redes que aparecem ou somem com o painel aberto mudam a altura desejada:
// redesenha no tamanho novo e so entao redimensiona, mantendo fixa a borda voltada
// para a barra de tarefas. Retorna true se ja redesenhou.
bool FlyoutWindow::FitHeightToContent(const SystemSnapshot& snap) {
    const float desired = ComputeDesiredHeightDip(snap, maxHeightDip_);
    if (std::fabs(desired - currentHeightDip_) < 0.5f) {
        return false;
    }
    RECT wr{};
    if (!GetWindowRect(hwnd_, &wr)) {
        return false;
    }
    currentHeightDip_ = desired;
    RedrawNow();
    const int physH = MulDiv(static_cast<int>(currentHeightDip_ + 0.5f), static_cast<int>(currentDpi_), 96);
    const int y = anchoredAbove_ ? (wr.bottom - physH) : wr.top;
    SetWindowPos(hwnd_, nullptr, wr.left, y, wr.right - wr.left, physH, SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

void FlyoutWindow::UpdateIfVisible(
    D2DResources& res,
    const SystemSnapshot& snap,
    const AppConfig& cfg,
    const ThemeColors& colors) {
    if (!IsVisible()) return;
    lastRes_ = &res;
    lastSnap_ = &snap;
    lastCfg_ = &cfg;
    lastColors_ = colors;
    ApplyDwmFrameStyle(colors.isLight);
    if (!FitHeightToContent(snap)) {
        RedrawNow();
    }
}

// Desenha na superficie e pede um WM_PAINT, que so copia o resultado (um render por atualizacao).
void FlyoutWindow::RedrawNow() {
    if (!hwnd_ || !lastRes_ || !lastSnap_ || !lastCfg_) return;

    ClampSelection(*lastSnap_);
    // Altura do conteudo do snapshot ATUAL (nao a do quadro anterior): se um disco/rede
    // sumiu, o scroll ja e limitado certo neste quadro.
    contentHeightDip_ = ComputeContentHeightDip(*lastSnap_);
    const float maxScroll = std::max(0.0f, contentHeightDip_ - currentHeightDip_);
    scrollDip_ = std::clamp(scrollDip_, 0.0f, maxScroll);

    if (RenderToSurface(
            *lastRes_, surface_, currentWidthDip_, currentHeightDip_, currentDpi_,
            *lastSnap_, *lastCfg_, lastColors_, scrollDip_,
            hoveredCard_, hoveredPinCard_, selectedCard_, hoverHeaderBtn_, contentHeightDip_)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

LRESULT CALLBACK FlyoutWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<FlyoutWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
        return DefWindowProcW(hwnd, msg, wParam, lParam);  // grava o titulo da janela
    }

    auto* self = reinterpret_cast<FlyoutWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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

LRESULT FlyoutWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    auto ptToDip = [&](LPARAM lp, float& dx, float& dy) {
        const int px = static_cast<short>(LOWORD(lp));
        const int py = static_cast<short>(HIWORD(lp));
        dx = static_cast<float>(px) * 96.0f / static_cast<float>(currentDpi_);
        dy = static_cast<float>(py) * 96.0f / static_cast<float>(currentDpi_);
    };

    auto cardRectAt = [&](int idx) -> D2D1_RECT_F {
        const float top = kHeaderH + kPadY - scrollDip_ + static_cast<float>(idx) * (kCardH + kCardGap);
        return D2D1::RectF(kPadX, top, currentWidthDip_ - kPadX, top + kCardH);
    };

    auto hitCardIndex = [&](float dx, float dy) -> int {
        if (dy < kHeaderH || dx < kPadX || dx > currentWidthDip_ - kPadX || !lastSnap_) {
            return -1;
        }
        const float relY = dy - (kHeaderH + kPadY - scrollDip_);
        if (relY < 0.0f) return -1;
        const float pitch = kCardH + kCardGap;
        const int idx = static_cast<int>(relY / pitch);
        const float within = relY - static_cast<float>(idx) * pitch;
        if (within > kCardH) return -1;
        if (idx < 0 || static_cast<size_t>(idx) >= CountCards(*lastSnap_)) return -1;
        return idx;
    };

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        // A superficie ja foi desenhada em RedrawNow/ShowAnchored: aqui so copia.
        if (!surface_.HasContent() && lastRes_ && lastSnap_ && lastCfg_) {
            RenderToSurface(
                *lastRes_, surface_, currentWidthDip_, currentHeightDip_, currentDpi_,
                *lastSnap_, *lastCfg_, lastColors_, scrollDip_,
                hoveredCard_, hoveredPinCard_, selectedCard_, hoverHeaderBtn_, contentHeightDip_);
        }
        surface_.BlitTo(hdc);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            lastDeactivateTick_ = GetTickCount64();
            Hide();
            return 0;
        }
        // Ativado: garante o foco de teclado (sem ele o Esc chega como WM_SYSKEYDOWN).
        SetFocus(hwnd_);
        return 0;

    case WM_DPICHANGED: {
        currentDpi_ = std::max<UINT>(HIWORD(wParam), 1);
        if (anchoring_) {
            return 0;
        }
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (!suggested) {
            return 0;
        }
        // Limite de altura do novo monitor, e tamanho calculado igual ao do DIB (MulDiv),
        // para nao sobrar faixa sem pintar por arredondamento.
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST), &mi)) {
            maxHeightDip_ = MaxHeightForWorkArea(mi.rcWork, currentDpi_);
        }
        if (lastSnap_) {
            currentHeightDip_ = ComputeDesiredHeightDip(*lastSnap_, maxHeightDip_);
        }
        const int physW = MulDiv(static_cast<int>(currentWidthDip_ + 0.5f), static_cast<int>(currentDpi_), 96);
        const int physH = MulDiv(static_cast<int>(currentHeightDip_ + 0.5f), static_cast<int>(currentDpi_), 96);
        RedrawNow();
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, physW, physH,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            Hide();
            return 0;
        }
        break;

    case WM_MOUSEWHEEL: {
        const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const float maxScroll = std::max(0.0f, contentHeightDip_ - currentHeightDip_);
        if (maxScroll > 0.0f) {
            scrollDip_ = std::clamp(scrollDip_ - (static_cast<float>(delta) / 120.0f) * 38.0f, 0.0f, maxScroll);
            RedrawNow();
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            trackingMouse_ = true;
        }
        float dx = 0.0f, dy = 0.0f;
        ptToDip(lParam, dx, dy);
        int newBtn = 0;
        if (PtInDipRect(dx, dy, kTaskMgrBtnRect)) newBtn = 1;
        else if (PtInDipRect(dx, dy, kMenuBtnRect)) newBtn = 2;

        const int newCard = (newBtn == 0) ? hitCardIndex(dx, dy) : -1;
        int newPinCard = -1;
        if (newCard >= 0) {
            const D2D1_RECT_F pinR = ComputeCardPinRect(cardRectAt(newCard));
            if (PtInDipRect(dx, dy, pinR)) {
                newPinCard = newCard;
            }
        }
        if (newBtn != hoverHeaderBtn_ || newCard != hoveredCard_ || newPinCard != hoveredPinCard_) {
            hoverHeaderBtn_ = newBtn;
            hoveredCard_ = newCard;
            hoveredPinCard_ = newPinCard;
            RedrawNow();
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        leftDown_ = false;
        rightDown_ = false;
        if (hoverHeaderBtn_ != 0 || hoveredCard_ != -1 || hoveredPinCard_ != -1) {
            hoverHeaderBtn_ = 0;
            hoveredCard_ = -1;
            hoveredPinCard_ = -1;
            RedrawNow();
        }
        return 0;

    // So conta como clique se o botao foi pressionado dentro do painel.
    case WM_LBUTTONDOWN:
        leftDown_ = true;
        return 0;

    case WM_LBUTTONUP: {
        if (!leftDown_) {
            return 0;
        }
        leftDown_ = false;
        float dx = 0.0f, dy = 0.0f;
        ptToDip(lParam, dx, dy);
        if (PtInDipRect(dx, dy, kTaskMgrBtnRect)) {
            Hide();
            if (controllerHwnd_) PostMessageW(controllerHwnd_, WM_FLYOUT_OPEN_TASKMGR, 0, 0);
            return 0;
        }
        if (PtInDipRect(dx, dy, kMenuBtnRect)) {
            if (controllerHwnd_) {
                POINT pt{};
                GetCursorPos(&pt);
                PostMessageW(controllerHwnd_, WM_FLYOUT_OPEN_MENU, 0, MAKELPARAM(pt.x, pt.y));
            }
            return 0;
        }
        const int card = hitCardIndex(dx, dy);
        if (card >= 0 && lastSnap_) {
            const D2D1_RECT_F pinR = ComputeCardPinRect(cardRectAt(card));
            if (PtInDipRect(dx, dy, pinR)) {
                pendingTogglePinId_ = GetCardIdAtIndex(*lastSnap_, card);
                if (controllerHwnd_ && !pendingTogglePinId_.empty()) {
                    PostMessageW(controllerHwnd_, WM_FLYOUT_TOGGLE_PIN, 0, 0);
                }
                return 0;
            }
            selectedCard_ = card;
            RedrawNow();
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        leftDown_ = true;  // o WM_LBUTTONUP seguinte faz parte do mesmo clique
        float dx = 0.0f, dy = 0.0f;
        ptToDip(lParam, dx, dy);
        const int card = hitCardIndex(dx, dy);
        if (card >= 0 && controllerHwnd_) {
            const D2D1_RECT_F pinR = ComputeCardPinRect(cardRectAt(card));
            if (!PtInDipRect(dx, dy, pinR)) {
                Hide();
                PostMessageW(controllerHwnd_, WM_FLYOUT_OPEN_TASKMGR, 0, 0);
            }
        }
        return 0;
    }

    case WM_RBUTTONDOWN:
        rightDown_ = true;
        return 0;

    case WM_RBUTTONUP:
        if (!rightDown_) {
            return 0;
        }
        rightDown_ = false;
        if (controllerHwnd_) {
            POINT pt{};
            GetCursorPos(&pt);
            PostMessageW(controllerHwnd_, WM_FLYOUT_OPEN_MENU, 0, MAKELPARAM(pt.x, pt.y));
        }
        return 0;
    }

    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}
