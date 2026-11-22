#pragma once

#include "metrics.h"
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <unordered_map>

class D2DResources {
public:
    bool Initialize();
    void Shutdown();

    ID2D1Factory* Factory() const { return d2dFactory_.Get(); }
    IDWriteFactory* WriteFactory() const { return dwriteFactory_.Get(); }
    ID2D1StrokeStyle* DashedStroke() const { return dashedStroke_.Get(); }

    IDWriteTextFormat* FmtHeaderTitle() const { return fmtHeaderTitle_.Get(); }
    IDWriteTextFormat* FmtHeaderBtn() const { return fmtHeaderBtn_.Get(); }
    IDWriteTextFormat* FmtCardTitle() const { return fmtCardTitle_.Get(); }
    IDWriteTextFormat* FmtCardSub() const { return fmtCardSub_.Get(); }
    IDWriteTextFormat* FmtBarLabel() const { return fmtBarLabel_.Get(); }
    IDWriteTextFormat* FmtBarValue() const { return fmtBarValue_.Get(); }
    IDWriteTextFormat* FmtBarSmallLabel() const { return fmtBarSmallLabel_.Get(); }
    IDWriteTextFormat* FmtBarSmallValue() const { return fmtBarSmallValue_.Get(); }

    // Geometria do alfinete centrada em (0,0), criada uma vez e desenhada com transformacao.
    ID2D1PathGeometry* PinGeometry() const { return pinGeometry_.Get(); }

    // Dispositivo D2D sobre um D3D11 WARP sem threads internas (renderiza na CPU, na thread
    // que chama). Criado sob demanda; nullptr se indisponivel (as superficies usam o DCRT).
    ID2D1Device* Device();
    uint32_t DeviceGeneration() const { return deviceGen_; }
    // Chamar depois de D2DERR_RECREATE_TARGET: a proxima Device() recria tudo.
    void ResetDevice();

    // Igual a rt->DrawText, mas reaproveita o IDWriteTextLayout quando o mesmo texto e
    // desenhado com o mesmo formato e tamanho (rotulos e valores parados entre quadros).
    // So na thread de UI.
    void DrawTextCached(ID2D1RenderTarget* rt, const std::wstring& text, IDWriteTextFormat* fmt,
                        const D2D1_RECT_F& rect, ID2D1Brush* brush);

private:
    struct TextLayoutEntry {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        uint64_t lastUse = 0;
    };
    std::unordered_map<std::wstring, TextLayoutEntry> textLayouts_;
    uint64_t textUseCounter_ = 0;

    bool CreateTextFormat(
        float sizeDip,
        DWRITE_FONT_WEIGHT weight,
        DWRITE_TEXT_ALIGNMENT align,
        bool ellipsis,
        Microsoft::WRL::ComPtr<IDWriteTextFormat>& out);

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashedStroke_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> pinGeometry_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    bool deviceFailed_ = false;
    uint32_t deviceGen_ = 1;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtHeaderTitle_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtHeaderBtn_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtCardTitle_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtCardSub_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtBarLabel_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtBarValue_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtBarSmallLabel_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtBarSmallValue_;
};

class D2DSurface {
public:
    D2DSurface() = default;
    ~D2DSurface();

    D2DSurface(const D2DSurface&) = delete;
    D2DSurface& operator=(const D2DSurface&) = delete;

    bool Resize(D2DResources& res, int physW, int physH, UINT dpi);
    bool BeginDraw(D2DResources& res, D2D1_COLOR_F clearColor = D2D1::ColorF(0, 0, 0, 0));
    bool EndDraw();
    // Copiam o conteudo ja desenhado (EndDraw feito) para a janela.
    bool PresentLayered(HWND hwnd, POINT ptDst) const;
    bool BlitTo(HDC targetDC) const;
    bool HasContent() const { return hasContent_; }

    ID2D1RenderTarget* RT() const { return active_; }
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F color);
    int Width() const { return width_; }
    int Height() const { return height_; }
    size_t CountNonZeroAlphaPixels() const;

    void Release();

private:
    bool EnsureRenderTarget(D2DResources& res);
    bool EnsureDeviceBitmaps();
    void DropRenderTarget();

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    UINT dpi_ = 96;
    bool drawing_ = false;
    bool hasContent_ = false;

    // Caminho principal: device context do WARP sem threads desenhando num bitmap, copiado
    // para o DIB no EndDraw (o DCRT em software acorda varias threads do pool a cada quadro).
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> staging_;  // CPU_READ, para copiar para o DIB
    uint32_t dcGen_ = 0;
    int bitmapsW_ = 0;
    int bitmapsH_ = 0;
    UINT bitmapsDpi_ = 0;
    D2DResources* res_ = nullptr;

    // Fallback: DC render target em software, religado (BindDC) so quando o DIB muda.
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcrt_;
    bool dcrtBound_ = false;

    ID2D1RenderTarget* active_ = nullptr;  // dc_ ou dcrt_
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
};

struct SparklineStyle {
    D2D1_COLOR_F lineColor{};
    D2D1_COLOR_F boxBg{};
    D2D1_COLOR_F boxBorder{};
    D2D1_COLOR_F gridColor{};
    bool drawGrid = false;
    float strokeWidth = 1.25f;
    float fillAlpha = 0.28f;
    const RingBuffer<60>* secondary = nullptr;
    D2D1_COLOR_F secondaryColor{};
    ID2D1StrokeStyle* secondaryStrokeStyle = nullptr;
};

void DrawSparkline(
    D2DResources& res,
    D2DSurface& surf,
    D2D1_RECT_F rect,
    const RingBuffer<60>& series,
    double maxScale,
    const SparklineStyle& style);

// Numero com virgula decimal (pt-BR), ex.: 12,5
std::wstring FormatDecimalComma(double val, int decimals);
std::wstring FormatDetailedBps(double bps);
std::wstring FormatDiskSpeedMBs(double mbs);
