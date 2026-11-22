#pragma once

#include <windows.h>
#include <d2d1.h>
#include <cstdint>

inline D2D1_COLOR_F ColorFromHex(uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f,
        alpha);
}

inline bool IsSystemLightTheme() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    if (status == ERROR_SUCCESS) {
        return value != 0;
    }
    return false;
}

struct ThemeColors {
    bool isLight = false;

    // Task Manager metric colors
    D2D1_COLOR_F cpu{};
    D2D1_COLOR_F ram{};
    D2D1_COLOR_F disk{};
    D2D1_COLOR_F netRecv{};
    D2D1_COLOR_F netSend{};
    D2D1_COLOR_F gpu{};

    // Taskbar overlay colors
    D2D1_COLOR_F barHitPillBg{};
    D2D1_COLOR_F barHoverBg{};
    D2D1_COLOR_F barTextPrimary{};
    D2D1_COLOR_F barTextMuted{};
    D2D1_COLOR_F barBoxBg{};
    D2D1_COLOR_F barBoxBorder{};

    // Flyout panel colors
    D2D1_COLOR_F flyoutBg{};
    D2D1_COLOR_F flyoutHeaderBorder{};
    D2D1_COLOR_F flyoutCardBg{};
    D2D1_COLOR_F flyoutCardHoverBg{};
    D2D1_COLOR_F flyoutCardSelectedBg{};
    D2D1_COLOR_F flyoutCardBorder{};
    D2D1_COLOR_F flyoutBoxBg{};
    D2D1_COLOR_F flyoutBoxBorder{};
    D2D1_COLOR_F flyoutGridLine{};
    D2D1_COLOR_F flyoutTextPrimary{};
    D2D1_COLOR_F flyoutTextSecondary{};
    D2D1_COLOR_F flyoutButtonHover{};
    D2D1_COLOR_F flyoutScrollThumb{};
};

inline ThemeColors GetThemeColors(bool lightTheme) {
    ThemeColors c;
    c.isLight = lightTheme;

    c.cpu = ColorFromHex(0x26A9E0);
    c.ram = ColorFromHex(0x4D8BFF);
    c.disk = ColorFromHex(0x84CC16);
    c.netRecv = ColorFromHex(0xE84D8A);
    c.netSend = ColorFromHex(0xF59BC0);
    c.gpu = ColorFromHex(0xC05CE0);

    if (lightTheme) {
        c.barHitPillBg = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.012f);
        c.barHoverBg = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.07f);
        c.barTextPrimary = ColorFromHex(0x141414);
        c.barTextMuted = ColorFromHex(0x4D4D4D);
        c.barBoxBg = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.65f);
        c.barBoxBorder = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.22f);

        c.flyoutBg = ColorFromHex(0xF6F6F6);
        c.flyoutHeaderBorder = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f);
        c.flyoutCardBg = ColorFromHex(0xFFFFFF, 0.72f);
        c.flyoutCardHoverBg = ColorFromHex(0xEAEAEA);
        c.flyoutCardSelectedBg = ColorFromHex(0xE1E6EF);
        c.flyoutCardBorder = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.07f);
        c.flyoutBoxBg = ColorFromHex(0xFAFAFA);
        c.flyoutBoxBorder = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.16f);
        c.flyoutGridLine = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f);
        c.flyoutTextPrimary = ColorFromHex(0x161616);
        c.flyoutTextSecondary = ColorFromHex(0x5A5A5A);
        c.flyoutButtonHover = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.07f);
        c.flyoutScrollThumb = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f);
    } else {
        c.barHitPillBg = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.012f);
        c.barHoverBg = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
        c.barTextPrimary = ColorFromHex(0xF4F4F4);
        c.barTextMuted = ColorFromHex(0xB4B4B4);
        c.barBoxBg = ColorFromHex(0x181818, 0.65f);
        c.barBoxBorder = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f);

        c.flyoutBg = ColorFromHex(0x1C1C1C);
        c.flyoutHeaderBorder = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
        c.flyoutCardBg = ColorFromHex(0x232323, 0.85f);
        c.flyoutCardHoverBg = ColorFromHex(0x2E2E2E);
        c.flyoutCardSelectedBg = ColorFromHex(0x343840);
        c.flyoutCardBorder = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.07f);
        c.flyoutBoxBg = ColorFromHex(0x161616);
        c.flyoutBoxBorder = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f);
        c.flyoutGridLine = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f);
        c.flyoutTextPrimary = ColorFromHex(0xF2F2F2);
        c.flyoutTextSecondary = ColorFromHex(0xB0B0B0);
        c.flyoutButtonHover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f);
        c.flyoutScrollThumb = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f);
    }

    return c;
}
