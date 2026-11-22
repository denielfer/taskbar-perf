#include "d2d_layered.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

bool D2DResources::Initialize() {
    if (!d2dFactory_) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()))) {
            return false;
        }
    }
    if (!dwriteFactory_) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) {
            return false;
        }
    }
    if (!dashedStroke_) {
        const float dashes[] = {2.0f, 2.0f};
        D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_FLAT,
            D2D1_CAP_STYLE_FLAT,
            D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND,
            1.0f,
            D2D1_DASH_STYLE_CUSTOM,
            0.0f);
        d2dFactory_->CreateStrokeStyle(&props, dashes, ARRAYSIZE(dashes), dashedStroke_.GetAddressOf());
    }
    if (!pinGeometry_) {
        // Alfinete vetorial centrado na origem (cabeca + corpo); a haste e uma linha a parte.
        if (SUCCEEDED(d2dFactory_->CreatePathGeometry(pinGeometry_.GetAddressOf())) && pinGeometry_) {
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(pinGeometry_->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure(D2D1::Point2F(-3.2f, -5.0f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(3.2f, -5.0f));
                sink->AddLine(D2D1::Point2F(2.3f, -1.2f));
                sink->AddLine(D2D1::Point2F(4.4f, 1.2f));
                sink->AddLine(D2D1::Point2F(-4.4f, 1.2f));
                sink->AddLine(D2D1::Point2F(-2.3f, -1.2f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                if (FAILED(sink->Close())) {
                    pinGeometry_.Reset();
                }
            } else {
                pinGeometry_.Reset();
            }
        }
    }

    if (!CreateTextFormat(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true, fmtHeaderTitle_)) return false;
    if (!CreateTextFormat(11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, fmtHeaderBtn_)) return false;
    if (!CreateTextFormat(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true, fmtCardTitle_)) return false;
    if (!CreateTextFormat(11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true, fmtCardSub_)) return false;
    if (!CreateTextFormat(10.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true, fmtBarLabel_)) return false;
    if (!CreateTextFormat(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true, fmtBarValue_)) return false;
    if (!CreateTextFormat(9.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true, fmtBarSmallLabel_)) return false;
    if (!CreateTextFormat(10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true, fmtBarSmallValue_)) return false;

    return true;
}

bool D2DResources::CreateTextFormat(
    float sizeDip,
    DWRITE_FONT_WEIGHT weight,
    DWRITE_TEXT_ALIGNMENT align,
    bool ellipsis,
    Microsoft::WRL::ComPtr<IDWriteTextFormat>& out) {
    out.Reset();
    HRESULT hr = dwriteFactory_->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        sizeDip,
        L"pt-BR",
        out.GetAddressOf());
    if (FAILED(hr) || !out) {
        return false;
    }
    out->SetTextAlignment(align);
    out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis) {
        Microsoft::WRL::ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(out.Get(), sign.GetAddressOf()))) {
            DWRITE_TRIMMING trim{};
            trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            out->SetTrimming(&trim, sign.Get());
        }
    }
    return true;
}

ID2D1Device* D2DResources::Device() {
    if (d2dDevice_) {
        return d2dDevice_.Get();
    }
    if (deviceFailed_ || !d2dFactory_) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory1;
    if (FAILED(d2dFactory_.As(&factory1))) {
        deviceFailed_ = true;
        return nullptr;
    }
    // WARP = rasterizador em CPU, sem depender de driver nem acordar a GPU. Sem as flags
    // abaixo ele espalha o trabalho num pool de threads (varias trocas de contexto por quadro).
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                       D3D11_CREATE_DEVICE_SINGLETHREADED |
                       D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<ID2D1Device> dev;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                                 D3D11_SDK_VERSION, d3d.GetAddressOf(), nullptr, nullptr)) ||
        FAILED(d3d.As(&dxgi)) ||
        FAILED(factory1->CreateDevice(dxgi.Get(), dev.GetAddressOf())) || !dev) {
        deviceFailed_ = true;  // fica no DC render target em software
        return nullptr;
    }
    d3dDevice_ = std::move(d3d);
    d2dDevice_ = std::move(dev);
    return d2dDevice_.Get();
}

void D2DResources::ResetDevice() {
    d2dDevice_.Reset();
    d3dDevice_.Reset();
    ++deviceGen_;  // superficies com contexto do dispositivo antigo recriam o seu
}

void D2DResources::DrawTextCached(
    ID2D1RenderTarget* rt,
    const std::wstring& text,
    IDWriteTextFormat* fmt,
    const D2D1_RECT_F& rect,
    ID2D1Brush* brush) {
    if (!rt || !fmt || !brush || !dwriteFactory_) return;
    const float w = std::max(0.0f, rect.right - rect.left);
    const float h = std::max(0.0f, rect.bottom - rect.top);

    // Chave: ponteiro do formato + tamanho da caixa (em centesimos de DIP) + texto.
    wchar_t prefix[64]{};
    swprintf_s(prefix, L"%p|%d|%d|", static_cast<void*>(fmt),
               static_cast<int>(std::lround(w * 100.0f)), static_cast<int>(std::lround(h * 100.0f)));
    std::wstring key(prefix);
    key += text;

    const uint64_t now = ++textUseCounter_;
    auto it = textLayouts_.find(key);
    if (it == textLayouts_.end()) {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()), fmt, w, h, layout.GetAddressOf())) || !layout) {
            return;
        }
        // Poda: descarta o que nao foi usado nas ultimas ~256 chamadas (alguns quadros).
        constexpr uint64_t kKeepUses = 256;
        if (textLayouts_.size() >= kKeepUses) {
            for (auto e = textLayouts_.begin(); e != textLayouts_.end();) {
                e = (now - e->second.lastUse > kKeepUses) ? textLayouts_.erase(e) : std::next(e);
            }
        }
        it = textLayouts_.emplace(std::move(key), TextLayoutEntry{std::move(layout), now}).first;
    }
    it->second.lastUse = now;
    rt->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), it->second.layout.Get(), brush);
}

void D2DResources::Shutdown() {
    textLayouts_.clear();
    d2dDevice_.Reset();
    d3dDevice_.Reset();
    fmtHeaderTitle_.Reset();
    fmtHeaderBtn_.Reset();
    fmtCardTitle_.Reset();
    fmtCardSub_.Reset();
    fmtBarLabel_.Reset();
    fmtBarValue_.Reset();
    fmtBarSmallLabel_.Reset();
    fmtBarSmallValue_.Reset();
    dashedStroke_.Reset();
    pinGeometry_.Reset();
    dwriteFactory_.Reset();
    d2dFactory_.Reset();
}

D2DSurface::~D2DSurface() {
    Release();
}

void D2DSurface::DropRenderTarget() {
    brush_.Reset();
    staging_.Reset();
    target_.Reset();
    dc_.Reset();
    dcGen_ = 0;
    bitmapsW_ = 0;
    bitmapsH_ = 0;
    bitmapsDpi_ = 0;
    dcrt_.Reset();
    dcrtBound_ = false;
    active_ = nullptr;
}

void D2DSurface::Release() {
    DropRenderTarget();
    res_ = nullptr;
    if (memDC_) {
        if (oldBmp_) {
            SelectObject(memDC_, oldBmp_);
            oldBmp_ = nullptr;
        }
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        DeleteDC(memDC_);
        memDC_ = nullptr;
    } else if (dib_) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    bits_ = nullptr;
    width_ = 0;
    height_ = 0;
    drawing_ = false;
    hasContent_ = false;
}

bool D2DSurface::EnsureRenderTarget(D2DResources& res) {
    if (!res.Factory()) {
        return false;
    }
    if (ID2D1Device* device = res.Device()) {
        if (dc_ && dcGen_ == res.DeviceGeneration()) {
            return true;
        }
        DropRenderTarget();
        if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc_.GetAddressOf())) || !dc_) {
            dc_.Reset();
            return false;
        }
        dcGen_ = res.DeviceGeneration();
        dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        active_ = dc_.Get();
        return true;
    }

    if (dcrt_) {
        return true;
    }
    DropRenderTarget();
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f,
        96.0f,
        D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT);

    if (FAILED(res.Factory()->CreateDCRenderTarget(&props, dcrt_.GetAddressOf())) || !dcrt_) {
        dcrt_.Reset();
        return false;
    }
    dcrt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dcrtBound_ = false;
    active_ = dcrt_.Get();
    return true;
}

bool D2DSurface::EnsureDeviceBitmaps() {
    if (target_ && staging_ && bitmapsW_ == width_ && bitmapsH_ == height_ && bitmapsDpi_ == dpi_) {
        return true;
    }
    dc_->SetTarget(nullptr);
    target_.Reset();
    staging_.Reset();
    bitmapsW_ = 0;
    bitmapsH_ = 0;
    bitmapsDpi_ = 0;

    const D2D1_PIXEL_FORMAT fmt = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const float dpi = static_cast<float>(dpi_);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(width_), static_cast<UINT32>(height_));
    const D2D1_BITMAP_PROPERTIES1 targetProps = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, fmt, dpi, dpi);
    const D2D1_BITMAP_PROPERTIES1 stagingProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, fmt, dpi, dpi);
    if (FAILED(dc_->CreateBitmap(size, nullptr, 0, &targetProps, target_.GetAddressOf())) ||
        FAILED(dc_->CreateBitmap(size, nullptr, 0, &stagingProps, staging_.GetAddressOf()))) {
        target_.Reset();
        staging_.Reset();
        return false;
    }
    dc_->SetTarget(target_.Get());
    bitmapsW_ = width_;
    bitmapsH_ = height_;
    bitmapsDpi_ = dpi_;
    return true;
}

bool D2DSurface::Resize(D2DResources& res, int physW, int physH, UINT dpi) {
    if (drawing_) {
        return false;  // nunca redimensiona com um BeginDraw aberto
    }
    physW = std::max(1, physW);
    physH = std::max(1, physH);
    dpi_ = (dpi > 0) ? dpi : 96;

    if (!memDC_) {
        memDC_ = CreateCompatibleDC(nullptr);
        if (!memDC_) return false;
    }

    if (!EnsureRenderTarget(res)) {
        return false;
    }

    if (physW == width_ && physH == height_ && dib_ != nullptr) {
        return true;
    }

    if (oldBmp_) {
        SelectObject(memDC_, oldBmp_);
        oldBmp_ = nullptr;
    }
    if (dib_) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    bits_ = nullptr;
    width_ = 0;
    height_ = 0;
    hasContent_ = false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = physW;
    bmi.bmiHeader.biHeight = -physH; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    dib_ = CreateDIBSection(memDC_, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!dib_ || !bits_) {
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        bits_ = nullptr;
        return false;
    }

    oldBmp_ = SelectObject(memDC_, dib_);
    if (!oldBmp_ || oldBmp_ == HGDI_ERROR) {
        oldBmp_ = nullptr;
        DeleteObject(dib_);
        dib_ = nullptr;
        bits_ = nullptr;
        return false;
    }
    width_ = physW;
    height_ = physH;
    dcrtBound_ = false;  // DIB novo: o DCRT (fallback) precisa religar
    return true;
}

ID2D1SolidColorBrush* D2DSurface::Brush(D2D1_COLOR_F color) {
    if (!active_) return nullptr;
    if (!brush_) {
        if (FAILED(active_->CreateSolidColorBrush(color, brush_.GetAddressOf()))) {
            return nullptr;
        }
    } else {
        brush_->SetColor(color);
    }
    return brush_.Get();
}

bool D2DSurface::BeginDraw(D2DResources& res, D2D1_COLOR_F clearColor) {
    if (drawing_ || !memDC_ || !dib_ || !EnsureRenderTarget(res)) {
        return false;
    }
    res_ = &res;
    if (dc_) {
        if (!EnsureDeviceBitmaps()) {
            DropRenderTarget();
            return false;
        }
    } else if (!dcrtBound_) {
        // BindDC e uma parte grande do custo do quadro: so religa quando o DIB ou o alvo mudam.
        RECT rc{0, 0, width_, height_};
        if (FAILED(dcrt_->BindDC(memDC_, &rc))) {
            DropRenderTarget();
            if (!EnsureRenderTarget(res) || !dcrt_ || FAILED(dcrt_->BindDC(memDC_, &rc))) {
                DropRenderTarget();
                return false;
            }
        }
        dcrtBound_ = true;
    }
    active_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    active_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    active_->BeginDraw();
    active_->Clear(clearColor);
    drawing_ = true;
    hasContent_ = false;
    return true;
}

bool D2DSurface::EndDraw() {
    if (!active_ || !drawing_) return false;
    drawing_ = false;
    hasContent_ = false;
    const HRESULT hr = active_->EndDraw();
    if (FAILED(hr)) {
        if (dc_ && hr == D2DERR_RECREATE_TARGET && res_) {
            res_->ResetDevice();
        }
        DropRenderTarget();
        return false;
    }

    if (dc_) {
        // Copia o quadro do bitmap do WARP para o DIB usado por UpdateLayeredWindow/BitBlt.
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(staging_->CopyFromBitmap(nullptr, target_.Get(), nullptr)) ||
            FAILED(staging_->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
            DropRenderTarget();
            return false;
        }
        GdiFlush();  // nenhuma operacao GDI pendente no DIB antes de escrever nele
        const size_t rowBytes = static_cast<size_t>(width_) * 4;
        auto* dst = static_cast<uint8_t*>(bits_);
        for (int y = 0; y < height_; ++y) {
            std::memcpy(dst + rowBytes * static_cast<size_t>(y),
                        mapped.bits + static_cast<size_t>(mapped.pitch) * static_cast<size_t>(y),
                        rowBytes);
        }
        staging_->Unmap();
    }
    hasContent_ = true;
    return true;
}

bool D2DSurface::PresentLayered(HWND hwnd, POINT ptDst) const {
    if (!hasContent_ || !hwnd) {
        return false;
    }
    SIZE sz{width_, height_};
    POINT ptSrc{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    return UpdateLayeredWindow(hwnd, nullptr, &ptDst, &sz, memDC_, &ptSrc, 0, &blend, ULW_ALPHA) != FALSE;
}

bool D2DSurface::BlitTo(HDC targetDC) const {
    if (!hasContent_ || !targetDC) {
        return false;
    }
    return BitBlt(targetDC, 0, 0, width_, height_, memDC_, 0, 0, SRCCOPY) != FALSE;
}

size_t D2DSurface::CountNonZeroAlphaPixels() const {
    if (!bits_ || width_ <= 0 || height_ <= 0) {
        return 0;
    }
    const auto* pixels = static_cast<const uint32_t*>(bits_);
    const size_t total = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    size_t count = 0;
    for (size_t i = 0; i < total; ++i) {
        if ((pixels[i] >> 24) != 0) {
            ++count;
        }
    }
    return count;
}

void DrawSparkline(
    D2DResources& res,
    D2DSurface& surf,
    D2D1_RECT_F rect,
    const RingBuffer<60>& series,
    double maxScale,
    const SparklineStyle& style) {
    ID2D1RenderTarget* rt = surf.RT();
    ID2D1Factory* factory = res.Factory();
    if (!rt || !factory) return;

    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;
    if (w <= 2.0f || h <= 2.0f) return;

    // 1. Box background
    if (auto* b = surf.Brush(style.boxBg)) {
        rt->FillRectangle(rect, b);
    }

    // 2. Clip inside box
    const D2D1_RECT_F inner = D2D1::RectF(rect.left + 1.0f, rect.top + 1.0f, rect.right - 1.0f, rect.bottom - 1.0f);
    rt->PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // 3. Optional faint grid lines (Task Manager feel)
    if (style.drawGrid) {
        if (auto* gridBrush = surf.Brush(style.gridColor)) {
            const float iw = inner.right - inner.left;
            const float ih = inner.bottom - inner.top;
            for (int col = 1; col <= 3; ++col) {
                const float gx = inner.left + iw * (static_cast<float>(col) / 4.0f);
                rt->DrawLine(D2D1::Point2F(gx, inner.top), D2D1::Point2F(gx, inner.bottom), gridBrush, 0.75f);
            }
            for (int row = 1; row <= 2; ++row) {
                const float gy = inner.top + ih * (static_cast<float>(row) / 3.0f);
                rt->DrawLine(D2D1::Point2F(inner.left, gy), D2D1::Point2F(inner.right, gy), gridBrush, 0.75f);
            }
        }
    }

    const double safeMax = (maxScale > 0.0) ? maxScale : 100.0;
    const float iw = inner.right - inner.left;
    const float ih = inner.bottom - inner.top;
    constexpr size_t cap = RingBuffer<60>::Capacity();
    static_assert(cap > 1, "o grafico precisa de pelo menos 2 pontos");
    const float stepX = iw / static_cast<float>(cap - 1);

    auto drawSeries = [&](const RingBuffer<60>& buf, D2D1_COLOR_F col, bool fill, ID2D1StrokeStyle* strokeStyle) {
        const size_t cnt = buf.Count();
        if (cnt == 0) return;

        // Desenho so acontece na thread de UI: reaproveita o buffer entre quadros.
        static std::vector<D2D1_POINT_2F> pts;
        pts.clear();
        pts.reserve(cap + 1);
        const size_t startSlot = cap - cnt;
        for (size_t i = 0; i < cnt; ++i) {
            const double norm = std::clamp(static_cast<double>(buf.AtOldest(i)) / safeMax, 0.0, 1.0);
            const float x = inner.left + static_cast<float>(startSlot + i) * stepX;
            const float y = inner.bottom - static_cast<float>(norm) * (ih - 1.5f);
            pts.push_back(D2D1::Point2F(x, y));
        }

        if (pts.size() == 1) {
            pts.insert(pts.begin(), D2D1::Point2F(inner.left, pts[0].y));
        }

        if (fill) {
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> fillGeo;
            if (SUCCEEDED(factory->CreatePathGeometry(fillGeo.GetAddressOf())) && fillGeo) {
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(fillGeo->Open(sink.GetAddressOf())) && sink) {
                    sink->BeginFigure(D2D1::Point2F(pts.front().x, inner.bottom), D2D1_FIGURE_BEGIN_FILLED);
                    for (const auto& pt : pts) {
                        sink->AddLine(pt);
                    }
                    sink->AddLine(D2D1::Point2F(pts.back().x, inner.bottom));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();

                    D2D1_COLOR_F fillCol = col;
                    fillCol.a = style.fillAlpha;
                    if (auto* fb = surf.Brush(fillCol)) {
                        rt->FillGeometry(fillGeo.Get(), fb);
                    }
                }
            }
        }

        Microsoft::WRL::ComPtr<ID2D1PathGeometry> lineGeo;
        if (SUCCEEDED(factory->CreatePathGeometry(lineGeo.GetAddressOf())) && lineGeo) {
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(lineGeo->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure(pts.front(), D2D1_FIGURE_BEGIN_HOLLOW);
                for (size_t i = 1; i < pts.size(); ++i) {
                    sink->AddLine(pts[i]);
                }
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();

                if (auto* lb = surf.Brush(col)) {
                    rt->DrawGeometry(lineGeo.Get(), lb, style.strokeWidth, strokeStyle);
                }
            }
        }
    };

    drawSeries(series, style.lineColor, true, nullptr);
    if (style.secondary) {
        drawSeries(*style.secondary, style.secondaryColor, false, style.secondaryStrokeStyle);
    }

    rt->PopAxisAlignedClip();

    // 4. Crisp 1px border aligned on half-pixel
    if (auto* borderBrush = surf.Brush(style.boxBorder)) {
        const D2D1_RECT_F bRect = D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f);
        rt->DrawRectangle(bRect, borderBrush, 1.0f);
    }
}

std::wstring FormatDecimalComma(double val, int decimals) {
    wchar_t buf[48]{};
    swprintf_s(buf, L"%.*f", decimals, val);
    std::wstring s(buf);
    std::replace(s.begin(), s.end(), L'.', L',');
    return s;
}

namespace {
std::wstring ReplaceDotWithComma(std::wstring s) {
    std::replace(s.begin(), s.end(), L'.', L',');
    return s;
}
} // namespace

std::wstring FormatDetailedBps(double bps) {
    wchar_t buf[32]{};
    const double kbps = bps / 1000.0;
    if (kbps < 10.0 && kbps > 0.05) {
        swprintf_s(buf, L"%.1f Kbps", kbps);
        return ReplaceDotWithComma(buf);
    }
    if (kbps < 1000.0) {
        swprintf_s(buf, L"%.0f Kbps", kbps);
        return ReplaceDotWithComma(buf);
    }
    const double mbps = kbps / 1000.0;
    if (mbps < 100.0) {
        swprintf_s(buf, L"%.1f Mbps", mbps);
        return ReplaceDotWithComma(buf);
    }
    if (mbps < 1000.0) {
        swprintf_s(buf, L"%.0f Mbps", mbps);
        return ReplaceDotWithComma(buf);
    }
    swprintf_s(buf, L"%.1f Gbps", mbps / 1000.0);
    return ReplaceDotWithComma(buf);
}

std::wstring FormatDiskSpeedMBs(double mbs) {
    wchar_t buf[32]{};
    if (mbs < 0.001) {
        return L"0 KB/s";
    }
    if (mbs < 1.0) {
        swprintf_s(buf, L"%.0f KB/s", mbs * 1024.0);
        return ReplaceDotWithComma(buf);
    }
    if (mbs < 100.0) {
        swprintf_s(buf, L"%.1f MB/s", mbs);
        return ReplaceDotWithComma(buf);
    }
    if (mbs < 1024.0) {
        swprintf_s(buf, L"%.0f MB/s", mbs);
        return ReplaceDotWithComma(buf);
    }
    swprintf_s(buf, L"%.1f GB/s", mbs / 1024.0);
    return ReplaceDotWithComma(buf);
}
