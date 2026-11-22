#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <dbt.h>
#include <wtsapi32.h>

#include "config.h"
#include "d2d_layered.h"
#include "flyout_window.h"
#include "metrics.h"
#include "metrics_collector.h"
#include "taskbar_bar.h"
#include "taskbar_tracker.h"
#include "theme.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kControllerClassName[] = L"TaskbarPerfControllerWnd";
constexpr UINT kTrayIconId = 1;

constexpr UINT WM_TRAY_CALLBACK = WM_APP + 1;
constexpr UINT WM_SYNC_TASKBARS = WM_APP + 2;
constexpr UINT WM_SHOW_FLYOUT_CMD = WM_APP + 3;
constexpr UINT WM_METRICS_READY = WM_APP + 4;

// {53F56307-B6BF-11D0-94F2-00A0C91EFB8B} = GUID_DEVINTERFACE_DISK
constexpr GUID kGuidDevInterfaceDisk = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
// {6FE69556-704A-47A0-8F24-C28D936FDA47} = GUID_CONSOLE_DISPLAY_STATE
constexpr GUID kGuidConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

enum MenuCmdId : UINT {
    ID_MENU_OPEN_FLYOUT = 1001,
    ID_MENU_OPEN_TASKMGR = 1002,
    ID_MENU_MON_PRIMARY = 1020,
    ID_MENU_MON_ALL = 1021,
    ID_MENU_POS_LEFT = 1030,
    ID_MENU_POS_RIGHT = 1031,
    ID_MENU_HIDE_FULLSCREEN = 1040,
    ID_MENU_AUTOSTART = 1041,
    ID_MENU_EXIT = 1099,
    ID_MENU_PIN_BASE = 2000,
    ID_MENU_MON_BASE = 4000
};

HICON CreateTrayGraphIcon() {
    constexpr int sz = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sz;
    bi.bV5Height = -sz;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP hColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hColor || !bits) {
        if (hColor) DeleteObject(hColor);
        return nullptr;
    }

    auto* px = static_cast<uint32_t*>(bits);
    auto setPx = [&](int x, int y, uint32_t argb) {
        if (x >= 0 && x < sz && y >= 0 && y < sz) {
            px[y * sz + x] = argb;
        }
    };

    // Caixa escura com borda ciano e um mini grafico dentro
    for (int y = 2; y < sz - 2; ++y) {
        for (int x = 2; x < sz - 2; ++x) {
            const bool border = (x == 2 || x == sz - 3 || y == 2 || y == sz - 3);
            setPx(x, y, border ? 0xFF3FAEE0U : 0xEB181C22U);
        }
    }

    constexpr int curveY[24] = {
        22, 21, 19, 20, 17, 15, 16, 12,
        14, 18, 16, 11, 9,  12, 15, 13,
        10, 8,  11, 14, 12, 9,  11, 10
    };
    for (int i = 0; i < 24; ++i) {
        const int x = 4 + i;
        const int cy = curveY[i];
        for (int y = cy + 1; y <= sz - 4; ++y) {
            setPx(x, y, 0x5526A9E0U);
        }
        setPx(x, cy, 0xFF38C4FFU);
        setPx(x, cy - 1, 0xFF38C4FFU);
        if (i > 0) {
            const int py = curveY[i - 1];
            const int lo = std::min(cy, py);
            const int hi = std::max(cy, py);
            for (int y = lo; y <= hi; ++y) {
                setPx(x, y, 0xFF38C4FFU);
            }
        }
    }

    // Mascara monocromatica zerada (o alpha da cor e que define a transparencia).
    std::vector<uint8_t> maskBits((sz / 8) * sz, 0);
    HBITMAP hMask = CreateBitmap(sz, sz, 1, 1, maskBits.data());
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hColor;
    HICON icon = hMask ? CreateIconIndirect(&ii) : nullptr;

    if (hMask) DeleteObject(hMask);
    DeleteObject(hColor);
    return icon;
}

// "&" em nomes de rede/GPU viraria atalho sublinhado no menu.
std::wstring EscapeMenuText(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        out += ch;
        if (ch == L'&') out += L'&';
    }
    return out;
}

// Coleta as metricas numa thread propria: chamadas lentas (PDH, drivers de GPU/rede,
// IOCTL de disco) nunca travam a barra nem o painel.
//
// Todo o estado que a thread usa (coletor, eventos, mutex, snapshot pendente) fica num
// bloco compartilhado por shared_ptr: se a thread travar num driver e o Stop() desistir
// de esperar, ela continua com uma referencia valida e nada e liberado embaixo dela.
class MetricsWorker {
public:
    ~MetricsWorker() { Stop(); }

    bool Start(std::unique_ptr<MetricsCollector> collector, HWND notifyHwnd, UINT notifyMsg, DWORD intervalMs) {
        auto shared = std::make_shared<Shared>();
        shared->collector = std::move(collector);
        shared->notifyHwnd = notifyHwnd;
        shared->notifyMsg = notifyMsg;
        shared->intervalMs = intervalMs;
        shared->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        shared->wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!shared->stopEvent || !shared->wakeEvent || !shared->collector) {
            return false;
        }
        auto* threadRef = new std::shared_ptr<Shared>(shared);
        thread_ = CreateThread(nullptr, 0, &MetricsWorker::ThreadProc, threadRef, 0, nullptr);
        if (!thread_) {
            delete threadRef;
            return false;
        }
        shared_ = std::move(shared);
        return true;
    }

    void Stop() {
        if (thread_) {
            SetEvent(shared_->stopEvent);
            // Se a thread nao sair a tempo (presa num driver), apenas a abandona: ela
            // mantem sua propria referencia ao estado e o processo esta encerrando.
            WaitForSingleObject(thread_, 5000);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        shared_.reset();
    }

    void SetPaused(bool paused) {
        if (shared_ && shared_->paused.exchange(paused) != paused) {
            SetEvent(shared_->wakeEvent);
        }
    }

    void RequestDiskRefresh() {
        if (shared_) shared_->diskRefresh = true;
    }

    std::unique_ptr<SystemSnapshot> TakeSnapshot() {
        if (!shared_) return nullptr;
        std::lock_guard<std::mutex> lock(shared_->mutex);
        return std::move(shared_->pending);
    }

private:
    struct Shared {
        std::unique_ptr<MetricsCollector> collector;
        HWND notifyHwnd = nullptr;
        UINT notifyMsg = 0;
        DWORD intervalMs = 1000;
        HANDLE stopEvent = nullptr;
        HANDLE wakeEvent = nullptr;
        std::atomic<bool> paused{false};
        std::atomic<bool> diskRefresh{false};
        std::mutex mutex;
        std::unique_ptr<SystemSnapshot> pending;

        ~Shared() {
            if (stopEvent) CloseHandle(stopEvent);
            if (wakeEvent) CloseHandle(wakeEvent);
        }
    };

    static DWORD WINAPI ThreadProc(void* param) {
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            // A thread e dona de uma referencia propria ao estado ate terminar; libera
            // antes de CoUninitialize() caso o coletor segure algum objeto COM.
            std::unique_ptr<std::shared_ptr<Shared>> ref(static_cast<std::shared_ptr<Shared>*>(param));
            Loop(**ref);
        }
        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
        return 0;
    }

    static void Loop(Shared& st) {
        ULONGLONG next = GetTickCount64() + st.intervalMs;
        const HANDLE handles[2] = {st.stopEvent, st.wakeEvent};
        bool wasPaused = false;
        for (;;) {
            DWORD timeout = INFINITE;
            if (st.paused) {
                wasPaused = true;
            } else {
                const ULONGLONG now = GetTickCount64();
                timeout = (next > now) ? static_cast<DWORD>(next - now) : 0;
            }
            const DWORD r = WaitForMultipleObjects(2, handles, FALSE, timeout);
            if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) {
                break;
            }
            if (r == WAIT_OBJECT_0 + 1) {
                if (wasPaused && !st.paused) {
                    // Retomou: refaz as linhas de base e coleta 1 s depois, para a primeira
                    // amostra nao ser a media de todo o periodo pausado.
                    wasPaused = false;
                    st.collector->ResumeAfterPause();
                    next = GetTickCount64() + st.intervalMs;
                }
                continue;
            }
            if (r != WAIT_TIMEOUT || st.paused) {
                continue;
            }

            if (st.diskRefresh.exchange(false)) {
                st.collector->RequestDiskRefresh();
            }
            auto snap = std::make_unique<SystemSnapshot>(st.collector->Sample(true));
            if (WaitForSingleObject(st.stopEvent, 0) == WAIT_OBJECT_0) {
                break;  // pediram para parar durante a coleta: nao avisa uma janela que ja pode ter sumido
            }
            {
                std::lock_guard<std::mutex> lock(st.mutex);
                st.pending = std::move(snap);
            }
            PostMessageW(st.notifyHwnd, st.notifyMsg, 0, 0);

            const ULONGLONG now = GetTickCount64();
            next += st.intervalMs;
            if (next <= now) {
                next = now + st.intervalMs;  // atrasou (coleta lenta/suspensao): nao acumula rajadas
            }
        }
    }

    std::shared_ptr<Shared> shared_;
    HANDLE thread_ = nullptr;
};

class AppController {
public:
    bool Init(HINSTANCE hInst);
    int Run();
    void Shutdown();

private:
    static LRESULT CALLBACK ControllerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void CALLBACK WinEventProc(
        HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
        DWORD idEventThread, DWORD dwmsEventTime);

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void AddOrRefreshTrayIcon();
    void RemoveTrayIcon();
    void OnMetricsReady();
    void SyncBarsAndRender();
    void UpdateLocationHook();
    void RequestSync();
    void UpdatePauseState();
    bool IsPaused() const { return displayOff_ || sessionLocked_; }
    void ToggleFlyoutForBar(HWND barHwnd);
    void ShowContextMenu(POINT ptScreen, HWND anchorBarHwnd);
    void OpenTaskManager();

    static AppController* instance_;

    HINSTANCE hInst_ = nullptr;
    HWND controllerHwnd_ = nullptr;
    HICON trayIcon_ = nullptr;
    UINT msgTaskbarCreated_ = 0;
    HWINEVENTHOOK hookForeground_ = nullptr;
    HWINEVENTHOOK hookLocation_ = nullptr;
    DWORD hookedExplorerThread_ = 0;
    HPOWERNOTIFY displayNotify_ = nullptr;
    HDEVNOTIFY diskNotify_ = nullptr;
    bool sessionNotifyRegistered_ = false;
    bool trayIconAdded_ = false;
    bool displayOff_ = false;
    bool sessionLocked_ = false;
    bool syncPending_ = false;
    bool menuOpen_ = false;

    AppConfig cfg_{};
    ThemeColors colors_{};
    D2DResources d2d_;
    MetricsWorker worker_;
    SystemSnapshot snapshot_{};
    std::vector<TaskbarInfo> lastTaskbars_;
    std::vector<std::unique_ptr<TaskbarBarWindow>> bars_;
    FlyoutWindow flyout_;
};

AppController* AppController::instance_ = nullptr;

bool AppController::Init(HINSTANCE hInst) {
    hInst_ = hInst;
    instance_ = this;

    cfg_ = LoadConfig();
    colors_ = GetThemeColors(IsSystemLightTheme());

    if (!d2d_.Initialize()) {
        return false;
    }

    // Primeira amostra sincrona (para os pins padrao); depois o coletor passa para a thread.
    auto collector = std::make_unique<MetricsCollector>();
    collector->Initialize();
    snapshot_ = collector->Sample(false);
    cfg_.EnsureDefaultPins(snapshot_);
    const bool migrated = cfg_.MigrateLegacyPins(snapshot_);
    if (!cfg_.hasSavedPins || migrated) {
        cfg_.hasSavedPins = true;
        SaveConfig(cfg_);
    }

    if (!TaskbarBarWindow::RegisterWindowClass(hInst_) || !FlyoutWindow::RegisterWindowClass(hInst_)) {
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ControllerWndProc;
    wc.hInstance = hInst_;
    wc.lpszClassName = kControllerClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    controllerHwnd_ = CreateWindowExW(
        0,
        kControllerClassName,
        L"TaskbarPerfController",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr,
        nullptr,
        hInst_,
        this);
    if (!controllerHwnd_) {
        return false;
    }

    if (!flyout_.Create(hInst_, controllerHwnd_)) {
        return false;
    }

    msgTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    trayIcon_ = CreateTrayGraphIcon();
    AddOrRefreshTrayIcon();

    // Primeiro plano: evento raro, pode ser global. Movimento: so da thread da barra
    // do Explorer (um gancho global acordaria o app a cada movimento de mouse/caret).
    hookForeground_ = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    UpdateLocationHook();

    // Pausa a coleta com a tela desligada ou a sessao bloqueada.
    displayNotify_ = RegisterPowerSettingNotification(controllerHwnd_, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    sessionNotifyRegistered_ = WTSRegisterSessionNotification(controllerHwnd_, NOTIFY_FOR_THIS_SESSION) != FALSE;

    // Discos conectados/removidos (volumes com letra ja chegam por broadcast).
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = kGuidDevInterfaceDisk;
    diskNotify_ = RegisterDeviceNotificationW(controllerHwnd_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    SyncBarsAndRender();
    return worker_.Start(std::move(collector), controllerHwnd_, WM_METRICS_READY, cfg_.refreshMs);
}

int AppController::Run() {
    MSG msg{};
    BOOL r = 0;
    while ((r = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (r == -1) {
            return 1;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void AppController::Shutdown() {
    worker_.Stop();
    if (hookForeground_) {
        UnhookWinEvent(hookForeground_);
        hookForeground_ = nullptr;
    }
    if (hookLocation_) {
        UnhookWinEvent(hookLocation_);
        hookLocation_ = nullptr;
    }
    if (displayNotify_) {
        UnregisterPowerSettingNotification(displayNotify_);
        displayNotify_ = nullptr;
    }
    if (diskNotify_) {
        UnregisterDeviceNotification(diskNotify_);
        diskNotify_ = nullptr;
    }
    if (sessionNotifyRegistered_ && controllerHwnd_) {
        WTSUnRegisterSessionNotification(controllerHwnd_);
        sessionNotifyRegistered_ = false;
    }
    RemoveTrayIcon();
    if (trayIcon_) {
        DestroyIcon(trayIcon_);
        trayIcon_ = nullptr;
    }
    bars_.clear();
    flyout_.Destroy();
    if (controllerHwnd_) {
        DestroyWindow(controllerHwnd_);  // WM_NCDESTROY zera controllerHwnd_
        controllerHwnd_ = nullptr;
    }
    d2d_.Shutdown();
    instance_ = nullptr;
}

void AppController::AddOrRefreshTrayIcon() {
    if (!controllerHwnd_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = controllerHwnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = trayIcon_ ? trayIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"TaskbarPerf - Monitor de Desempenho");
    if (Shell_NotifyIconW(NIM_MODIFY, &nid) || Shell_NotifyIconW(NIM_ADD, &nid)) {
        trayIconAdded_ = true;
    }
}

void AppController::RemoveTrayIcon() {
    if (!trayIconAdded_ || !controllerHwnd_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = controllerHwnd_;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayIconAdded_ = false;
}

void AppController::OnMetricsReady() {
    auto snap = worker_.TakeSnapshot();
    if (!snap || IsPaused()) {
        return;
    }
    snapshot_ = std::move(*snap);
    if (cfg_.MigrateLegacyPins(snapshot_)) {
        SaveConfig(cfg_);
    }
    SyncBarsAndRender();
    flyout_.UpdateIfVisible(d2d_, snapshot_, cfg_, colors_);
}

void AppController::UpdateLocationHook() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD pid = 0;
    const DWORD tid = tray ? GetWindowThreadProcessId(tray, &pid) : 0;
    if (tid == hookedExplorerThread_ && (hookLocation_ || tid == 0)) {
        return;
    }
    if (hookLocation_) {
        UnhookWinEvent(hookLocation_);
        hookLocation_ = nullptr;
    }
    hookedExplorerThread_ = tid;
    if (tid != 0) {
        hookLocation_ = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, WinEventProc, pid, tid, WINEVENT_OUTOFCONTEXT);
    }
}

void AppController::RequestSync() {
    if (syncPending_ || !controllerHwnd_) {
        return;
    }
    syncPending_ = PostMessageW(controllerHwnd_, WM_SYNC_TASKBARS, 0, 0) != FALSE;
}

void AppController::UpdatePauseState() {
    const bool paused = IsPaused();
    worker_.SetPaused(paused);
    if (!paused) {
        // Ao retomar redesenha tudo (o que chegou durante a pausa foi ignorado).
        SyncBarsAndRender();
        flyout_.UpdateIfVisible(d2d_, snapshot_, cfg_, colors_);
    }
}

void AppController::SyncBarsAndRender() {
    // Tela desligada ou sessao bloqueada: nada de enumerar janelas nem desenhar.
    // UpdatePauseState() sincroniza tudo ao retomar.
    if (IsPaused()) {
        return;
    }
    UpdateLocationHook();  // Explorer pode ter reiniciado

    std::vector<HWND> ownBarHwnds;
    ownBarHwnds.reserve(bars_.size());
    for (const auto& b : bars_) {
        if (b->Hwnd()) ownBarHwnds.push_back(b->Hwnd());
    }

    lastTaskbars_ = EnumerateTaskbars(cfg_.allMonitors, cfg_.monitorId, flyout_.Hwnd(), ownBarHwnds);

    // Remove barras cujo monitor nao deve mais exibir o app
    bars_.erase(
        std::remove_if(bars_.begin(), bars_.end(), [&](const std::unique_ptr<TaskbarBarWindow>& bar) {
            return std::none_of(lastTaskbars_.begin(), lastTaskbars_.end(), [&](const TaskbarInfo& tb) {
                return tb.monitor == bar->Monitor();
            });
        }),
        bars_.end());

    for (const auto& tb : lastTaskbars_) {
        TaskbarBarWindow* target = nullptr;
        for (const auto& b : bars_) {
            if (b->Monitor() == tb.monitor) {
                target = b.get();
                break;
            }
        }
        if (!target) {
            auto created = std::make_unique<TaskbarBarWindow>();
            if (created->Create(hInst_, controllerHwnd_, tb.monitor, tb.taskbarHwnd)) {
                target = created.get();
                bars_.push_back(std::move(created));
            }
        }
        if (!target) continue;

        if (!tb.isVisible || (cfg_.hideOnFullscreen && tb.isFullscreenCovered)) {
            target->Hide();
        } else {
            target->UpdateAndRender(d2d_, tb, snapshot_, cfg_, colors_);
        }
    }
}

void AppController::ToggleFlyoutForBar(HWND barHwnd) {
    if (flyout_.IsVisible()) {
        flyout_.Hide();
        return;
    }
    if (flyout_.WasJustDeactivated()) {
        return;
    }

    if (lastTaskbars_.empty()) {
        SyncBarsAndRender();
    }

    auto barForMonitor = [&](HMONITOR mon) -> const TaskbarBarWindow* {
        for (const auto& b : bars_) {
            if (b->Monitor() == mon) return b.get();
        }
        return nullptr;
    };

    TaskbarInfo chosenTb{};
    const TaskbarBarWindow* chosenBar = nullptr;
    bool found = false;

    if (barHwnd) {
        for (const auto& b : bars_) {
            if (b->Hwnd() != barHwnd) continue;
            for (const auto& tb : lastTaskbars_) {
                if (tb.monitor == b->Monitor()) {
                    chosenTb = tb;
                    chosenBar = b.get();
                    found = true;
                    break;
                }
            }
            break;
        }
    }

    if (!found && !lastTaskbars_.empty()) {
        // Bandeja/atalho: usa a barra principal e a barra de metricas DELA
        // (bars_[0] pode ser de outro monitor depois que o Explorer reinicia).
        auto primary = std::find_if(lastTaskbars_.begin(), lastTaskbars_.end(),
            [](const TaskbarInfo& tb) { return tb.isPrimary; });
        chosenTb = (primary != lastTaskbars_.end()) ? *primary : lastTaskbars_[0];
        chosenBar = barForMonitor(chosenTb.monitor);
        found = true;
    }

    if (!found) {
        // Ex.: display escolhido foi desconectado — abre ancorado no monitor principal.
        const auto allDisplays = EnumerateAllDisplays(flyout_.Hwnd(), {});
        if (!allDisplays.empty()) {
            auto primary = std::find_if(allDisplays.begin(), allDisplays.end(),
                [](const TaskbarInfo& tb) { return tb.isPrimary; });
            chosenTb = (primary != allDisplays.end()) ? *primary : allDisplays[0];
            found = true;
        }
    }

    if (found) {
        const RECT anchorRect = (chosenBar && chosenBar->Hwnd() && IsWindowVisible(chosenBar->Hwnd()))
            ? chosenBar->CurrentBarRect()
            : chosenTb.taskbarRect;
        flyout_.ShowAnchored(d2d_, chosenTb, anchorRect, snapshot_, cfg_, colors_);
    }
}

void AppController::OpenTaskManager() {
    // Caminho completo: um nome relativo procuraria primeiro na pasta do exe/diretorio atual.
    wchar_t sysDir[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(sysDir, ARRAYSIZE(sysDir));
    if (len == 0 || len >= ARRAYSIZE(sysDir)) {
        return;
    }
    const std::wstring path = std::wstring(sysDir) + L"\\Taskmgr.exe";
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void AppController::ShowContextMenu(POINT ptScreen, HWND anchorBarHwnd) {
    if (menuOpen_) {
        return;  // TrackPopupMenuEx bombeia mensagens: evita abrir um menu dentro do outro
    }

    HMENU menu = CreatePopupMenu();
    HMENU metricsSub = CreatePopupMenu();
    HMENU monitorsSub = CreatePopupMenu();
    HMENU posSub = CreatePopupMenu();
    if (!menu || !metricsSub || !monitorsSub || !posSub) {
        if (menu) DestroyMenu(menu);
        if (metricsSub) DestroyMenu(metricsSub);
        if (monitorsSub) DestroyMenu(monitorsSub);
        if (posSub) DestroyMenu(posSub);
        return;
    }

    std::vector<std::wstring> menuPinIds;
    auto addPinItem = [&](const std::wstring& id, const std::wstring& label) {
        const UINT itemId = ID_MENU_PIN_BASE + static_cast<UINT>(menuPinIds.size());
        menuPinIds.push_back(id);
        AppendMenuW(metricsSub, MF_STRING | (cfg_.IsPinned(id) ? MF_CHECKED : 0), itemId, EscapeMenuText(label).c_str());
    };

    addPinItem(L"cpu", L"CPU");
    addPinItem(L"ram", L"Memória (RAM)");
    for (const auto& d : snapshot_.disks) {
        addPinItem(PinIdForDisk(d), d.instanceName);
    }
    for (const auto& n : snapshot_.nets) {
        addPinItem(PinIdForNet(n), n.name);
    }
    for (const auto& g : snapshot_.gpus) {
        addPinItem(PinIdForGpu(g), L"GPU " + std::to_wstring(g.index) + L" (" + g.name + L")");
    }

    std::vector<HWND> ownBarHwnds;
    ownBarHwnds.reserve(bars_.size());
    for (const auto& b : bars_) {
        if (b->Hwnd()) ownBarHwnds.push_back(b->Hwnd());
    }
    const auto displays = EnumerateAllDisplays(flyout_.Hwnd(), ownBarHwnds);

    const bool byPrimary = cfg_.monitorId.empty() && !cfg_.allMonitors;
    const bool byAll = cfg_.monitorId.empty() && cfg_.allMonitors;
    AppendMenuW(monitorsSub, MF_STRING | (byPrimary ? MF_CHECKED : 0), ID_MENU_MON_PRIMARY, L"Somente monitor principal");
    AppendMenuW(monitorsSub, MF_STRING | (byAll ? MF_CHECKED : 0), ID_MENU_MON_ALL, L"Todos os monitores");

    std::vector<std::wstring> menuMonitorIds;
    if (!displays.empty()) {
        AppendMenuW(monitorsSub, MF_SEPARATOR, 0, nullptr);
    }
    bool matchedSavedMonitor = false;
    for (size_t i = 0; i < displays.size(); ++i) {
        const auto& d = displays[i];
        const int w = d.monitorRect.right - d.monitorRect.left;
        const int h = d.monitorRect.bottom - d.monitorRect.top;
        std::wstring label = L"Monitor " + std::to_wstring(i + 1);
        if (!d.friendlyName.empty()) {
            label += L" \u2014 " + d.friendlyName;
        }
        label += L" (" + std::to_wstring(w) + L"\u00D7" + std::to_wstring(h) + L")";
        if (d.isPrimary) {
            label += L" [Principal]";
        }
        if (!d.taskbarHwnd) {
            label += L" (sem barra)";
        }
        const bool checked = !cfg_.monitorId.empty() && MatchesMonitorId(d, cfg_.monitorId);
        if (checked) {
            matchedSavedMonitor = true;
        }
        const UINT itemId = ID_MENU_MON_BASE + static_cast<UINT>(menuMonitorIds.size());
        menuMonitorIds.push_back(d.monitorId);
        AppendMenuW(monitorsSub, MF_STRING | (checked ? MF_CHECKED : 0), itemId, EscapeMenuText(label).c_str());
    }
    if (!cfg_.monitorId.empty() && !matchedSavedMonitor) {
        AppendMenuW(monitorsSub, MF_STRING | MF_CHECKED | MF_GRAYED, 0, L"Monitor salvo (desconectado)");
    }

    AppendMenuW(posSub, MF_STRING | (!cfg_.alignRight ? MF_CHECKED : 0), ID_MENU_POS_LEFT, L"Esquerda da barra");
    AppendMenuW(posSub, MF_STRING | (cfg_.alignRight ? MF_CHECKED : 0), ID_MENU_POS_RIGHT, L"Direita (perto do relógio)");

    AppendMenuW(menu, MF_STRING, ID_MENU_OPEN_FLYOUT, L"Abrir painel completo");
    SetMenuDefaultItem(menu, ID_MENU_OPEN_FLYOUT, FALSE);
    AppendMenuW(menu, MF_STRING, ID_MENU_OPEN_TASKMGR, L"Abrir Gerenciador de Tarefas");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Submenu que nao foi anexado nao e destruido junto com o menu: libera aqui.
    auto appendSub = [&](HMENU sub, const wchar_t* label) {
        if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), label)) {
            DestroyMenu(sub);
        }
    };
    appendSub(metricsSub, L"Fixados na barra");
    appendSub(monitorsSub, L"Monitores");
    appendSub(posSub, L"Posição na barra");
    AppendMenuW(menu, MF_STRING | (cfg_.hideOnFullscreen ? MF_CHECKED : 0), ID_MENU_HIDE_FULLSCREEN, L"Ocultar em tela cheia");
    AppendMenuW(menu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0), ID_MENU_AUTOSTART, L"Iniciar com o Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Sair");

    menuOpen_ = true;
    SetForegroundWindow(controllerHwnd_);
    const UINT cmd = TrackPopupMenuEx(
        menu,
        TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
        ptScreen.x, ptScreen.y,
        controllerHwnd_,
        nullptr);
    PostMessageW(controllerHwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    menuOpen_ = false;

    bool changed = false;
    if (cmd >= ID_MENU_PIN_BASE && cmd < ID_MENU_PIN_BASE + static_cast<UINT>(menuPinIds.size()) &&
        menuPinIds.size() < (ID_MENU_MON_BASE - ID_MENU_PIN_BASE)) {
        if (cfg_.TogglePin(menuPinIds[cmd - ID_MENU_PIN_BASE])) {
            changed = true;
        }
    } else if (cmd >= ID_MENU_MON_BASE && cmd < ID_MENU_MON_BASE + static_cast<UINT>(menuMonitorIds.size())) {
        const std::wstring& chosenId = menuMonitorIds[cmd - ID_MENU_MON_BASE];
        if (cfg_.allMonitors || cfg_.monitorId != chosenId) {
            cfg_.allMonitors = false;
            cfg_.monitorId = chosenId;
            changed = true;
        }
    } else {
        switch (cmd) {
        case ID_MENU_OPEN_FLYOUT:
            ToggleFlyoutForBar(anchorBarHwnd);
            break;
        case ID_MENU_OPEN_TASKMGR:
            OpenTaskManager();
            break;
        case ID_MENU_MON_PRIMARY:
            if (cfg_.allMonitors || !cfg_.monitorId.empty()) {
                cfg_.allMonitors = false;
                cfg_.monitorId.clear();
                changed = true;
            }
            break;
        case ID_MENU_MON_ALL:
            if (!cfg_.allMonitors || !cfg_.monitorId.empty()) {
                cfg_.allMonitors = true;
                cfg_.monitorId.clear();
                changed = true;
            }
            break;
        case ID_MENU_POS_LEFT:
            if (cfg_.alignRight) { cfg_.alignRight = false; changed = true; }
            break;
        case ID_MENU_POS_RIGHT:
            if (!cfg_.alignRight) { cfg_.alignRight = true; changed = true; }
            break;
        case ID_MENU_HIDE_FULLSCREEN:
            cfg_.hideOnFullscreen = !cfg_.hideOnFullscreen;
            changed = true;
            break;
        case ID_MENU_AUTOSTART:
            if (!SetAutoStartEnabled(!IsAutoStartEnabled())) {
                MessageBoxW(controllerHwnd_,
                    L"Não foi possível alterar a inicialização com o Windows.",
                    L"TaskbarPerf", MB_OK | MB_ICONWARNING);
            }
            break;
        case ID_MENU_EXIT:
            PostQuitMessage(0);
            break;
        default:
            break;
        }
    }

    if (changed) {
        SaveConfig(cfg_);
        SyncBarsAndRender();
        flyout_.UpdateIfVisible(d2d_, snapshot_, cfg_, colors_);
    }
}

void CALLBACK AppController::WinEventProc(
    HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
    DWORD, DWORD) {
    AppController* self = instance_;
    if (!self || !self->controllerHwnd_ || self->IsPaused()) return;

    if (event == EVENT_SYSTEM_FOREGROUND) {
        self->RequestSync();
        return;
    }

    if (event == EVENT_OBJECT_LOCATIONCHANGE && idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd) {
        for (const auto& tb : self->lastTaskbars_) {
            if (tb.taskbarHwnd == hwnd) {
                self->RequestSync();
                break;
            }
        }
    }
}

LRESULT CALLBACK AppController::ControllerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<AppController*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->controllerHwnd_ = hwnd;
        return DefWindowProcW(hwnd, msg, wParam, lParam);  // grava o titulo da janela
    }

    auto* self = reinterpret_cast<AppController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self) self->controllerHwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT AppController::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msgTaskbarCreated_ != 0 && msg == msgTaskbarCreated_) {
        AddOrRefreshTrayIcon();
        UpdateLocationHook();
        SyncBarsAndRender();
        return 0;
    }

    switch (msg) {
    case WM_METRICS_READY:
        OnMetricsReady();
        return 0;

    case WM_SYNC_TASKBARS:
        syncPending_ = false;
        SyncBarsAndRender();
        return 0;

    case WM_SHOW_FLYOUT_CMD:
        ToggleFlyoutForBar(nullptr);
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        colors_ = GetThemeColors(IsSystemLightTheme());
        if (!IsPaused()) {
            SyncBarsAndRender();
            flyout_.UpdateIfVisible(d2d_, snapshot_, cfg_, colors_);
        }
        return 0;

    case WM_POWERBROADCAST:
        if (wParam == PBT_POWERSETTINGCHANGE) {
            const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
            if (setting && IsEqualGUID(setting->PowerSetting, kGuidConsoleDisplayState) &&
                setting->DataLength >= sizeof(DWORD)) {
                DWORD state = 0;  // 0 = desligada, 1 = ligada, 2 = escurecida
                std::memcpy(&state, setting->Data, sizeof(state));
                displayOff_ = (state == 0);
                UpdatePauseState();
            }
        }
        return TRUE;

    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK) {
            sessionLocked_ = true;
            UpdatePauseState();
        } else if (wParam == WTS_SESSION_UNLOCK) {
            sessionLocked_ = false;
            UpdatePauseState();
        }
        return 0;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            worker_.RequestDiskRefresh();
        }
        return TRUE;

    case TaskbarBarWindow::WM_BAR_LEFT_CLICK:
        ToggleFlyoutForBar(reinterpret_cast<HWND>(wParam));
        return 0;

    case TaskbarBarWindow::WM_BAR_RIGHT_CLICK: {
        POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        ShowContextMenu(pt, reinterpret_cast<HWND>(wParam));
        return 0;
    }

    case FlyoutWindow::WM_FLYOUT_OPEN_MENU: {
        POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        ShowContextMenu(pt, nullptr);
        return 0;
    }

    case FlyoutWindow::WM_FLYOUT_OPEN_TASKMGR:
        OpenTaskManager();
        return 0;

    case FlyoutWindow::WM_FLYOUT_TOGGLE_PIN: {
        const std::wstring id = flyout_.TakePendingTogglePinId();
        if (!id.empty() && cfg_.TogglePin(id)) {
            SaveConfig(cfg_);
            SyncBarsAndRender();
            flyout_.UpdateIfVisible(d2d_, snapshot_, cfg_, colors_);
        }
        return 0;
    }

    case WM_TRAY_CALLBACK:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
            ToggleFlyoutForBar(nullptr);
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            POINT pt{};
            GetCursorPos(&pt);
            ShowContextMenu(pt, nullptr);
            break;
        }
        }
        return 0;

    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(controllerHwnd_, msg, wParam, lParam);
}

void EnsureConsoleForSelfTest() {
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
        }
    }
}

int RunSelfTest() {
    EnsureConsoleForSelfTest();

    D2DResources d2d;
    if (!d2d.Initialize()) {
        std::printf("[FAIL] D2DResources::Initialize failed\n");
        return 1;
    }

    MetricsCollector collector;
    collector.Initialize();
    const SystemSnapshot initSnap = collector.Sample(false);
    Sleep(500);
    collector.Sample(true);
    Sleep(250);
    const SystemSnapshot snap = collector.Sample(true);

    const bool pdhCpu = collector.IsPdhCpuActive();
    const bool pdhDisk = collector.IsPdhDiskActive();
    const bool pdhGpu = collector.IsPdhGpuActive();
    const bool pdhGpuMem = collector.IsPdhGpuMemActive();

    std::printf("=== TaskbarPerf Self-Test ===\n");
    std::printf("PDH active: cpu=%d disk=%d gpu=%d gpumem=%d\n", pdhCpu ? 1 : 0, pdhDisk ? 1 : 0, pdhGpu ? 1 : 0, pdhGpuMem ? 1 : 0);
    std::printf("Initial snapshot disks=%zu (history=%zu)\n",
        initSnap.disks.size(),
        initSnap.disks.empty() ? 0 : initSnap.disks[0].history.Count());
    std::printf("CPU: %.1f%% @ %.2f GHz (cores=%u)\n",
        snap.cpu.utilizationPct, snap.cpu.speedGhz, snap.cpu.logicalCores);
    std::printf("RAM: %.2f / %.2f GB (%.1f%%)\n",
        snap.ram.usedGb, snap.ram.totalGb, snap.ram.usedPct);

    std::printf("Disks (%zu):\n", snap.disks.size());
    for (const auto& d : snap.disks) {
        std::printf("  - %ls | %ls | active=%.1f%% R=%.1f W=%.1f MB/s\n",
            d.instanceName.c_str(), d.mediaType.c_str(), d.activePct, d.readMBs, d.writeMBs);
    }

    std::printf("Networks (%zu):\n", snap.nets.size());
    for (const auto& n : snap.nets) {
        std::printf("  - %ls (%ls) phys=%d send=%.0f bps recv=%.0f bps\n",
            n.name.c_str(), n.description.c_str(), n.isPhysical ? 1 : 0, n.sendBps, n.recvBps);
    }

    std::printf("GPUs (%zu):\n", snap.gpus.size());
    for (const auto& g : snap.gpus) {
        std::printf("  - GPU %d: %ls | util=%.1f%% temp=%.1f C VRAM=%.2f/%.2f GB\n",
            g.index, g.name.c_str(), g.utilizationPct, g.tempC, g.vramUsedGb, g.vramTotalGb);
    }

    const auto displays = EnumerateAllDisplays(nullptr, {});
    std::printf("Displays (%zu):\n", displays.size());
    for (size_t i = 0; i < displays.size(); ++i) {
        const auto& d = displays[i];
        std::printf("  - Display %zu: %ls (%ldx%ld) primary=%d taskbar=%d id=%ls\n",
            i + 1, d.friendlyName.c_str(),
            d.monitorRect.right - d.monitorRect.left,
            d.monitorRect.bottom - d.monitorRect.top,
            d.isPrimary ? 1 : 0,
            d.taskbarHwnd ? 1 : 0,
            d.monitorId.c_str());
    }
    if (!displays.empty()) {
        const auto selected = EnumerateTaskbars(false, displays[0].monitorId, nullptr, {});
        if (selected.size() != 1 || !MatchesMonitorId(selected[0], displays[0].monitorId)) {
            std::printf("[FAIL] EnumerateTaskbars(selectedMonitorId) did not match display 1\n");
            return 1;
        }
    }

    AppConfig cfg;
    cfg.EnsureDefaultPins(snap);
    std::printf("Default pins (%zu):", cfg.pinnedIds.size());
    for (const auto& id : cfg.pinnedIds) {
        std::printf(" [%ls]", id.c_str());
    }
    std::printf("\n");

    // Migracao de pins legados (formato antigo por indice/nome)
    if (!snap.gpus.empty() || !snap.nets.empty()) {
        AppConfig legacy;
        legacy.pinnedIds = {L"cpu"};
        if (!snap.gpus.empty()) legacy.pinnedIds.push_back(L"gpu:" + std::to_wstring(snap.gpus[0].index));
        if (!snap.gpus.empty()) legacy.pinnedIds.push_back(LegacyLuidPinIdForGpu(snap.gpus[0]));
        if (!snap.nets.empty()) legacy.pinnedIds.push_back(L"net:" + snap.nets[0].name);
        legacy.MigrateLegacyPins(snap);
        const bool gpuOk = snap.gpus.empty() ||
            (legacy.IsPinned(PinIdForGpu(snap.gpus[0])) &&
             !legacy.IsPinned(LegacyLuidPinIdForGpu(snap.gpus[0])));
        const bool netOk = snap.nets.empty() || legacy.IsPinned(PinIdForNet(snap.nets[0]));
        if (!gpuOk || !netOk) {
            std::printf("[FAIL] MigrateLegacyPins did not convert legacy ids\n");
            return 1;
        }
    }

    const std::wstring diskPin = snap.disks.empty() ? L"disk:0" : PinIdForDisk(snap.disks[0]);
    const bool hadDisk = cfg.IsPinned(diskPin);
    if (!cfg.TogglePin(diskPin) || cfg.IsPinned(diskPin) == hadDisk) {
        std::printf("[FAIL] TogglePin(%ls) failed\n", diskPin.c_str());
        return 1;
    }
    cfg.TogglePin(diskPin);

    const ThemeColors colors = GetThemeColors(false);
    float barW = 0.0f, barH = 0.0f;
    TaskbarBarWindow::ComputeLayoutDip(cfg, snap, 1000.0f, 48.0f, barW, barH);

    D2DSurface barSurf;
    if (!TaskbarBarWindow::RenderToSurface(d2d, barSurf, barW, barH, 96, snap, cfg, false, colors)) {
        std::printf("[FAIL] Bar offscreen render failed\n");
        return 1;
    }
    const size_t barPixels = barSurf.CountNonZeroAlphaPixels();
    std::printf("Bar render: %dx%d px, non-zero alpha pixels=%zu\n",
        barSurf.Width(), barSurf.Height(), barPixels);
    if (barPixels < 100) {
        std::printf("[FAIL] Bar render produced too few pixels\n");
        return 1;
    }

    D2DSurface flySurf;
    float contentH = 0.0f;
    if (!FlyoutWindow::RenderToSurface(
            d2d, flySurf, FlyoutWindow::kWidthDip, 540.0f, 96, snap, cfg, colors, 0.0f, 0, 0, 0, 0, contentH)) {
        std::printf("[FAIL] Flyout offscreen render failed\n");
        return 1;
    }
    const size_t flyPixels = flySurf.CountNonZeroAlphaPixels();
    std::printf("Flyout render: %dx%d px, contentH=%.1f, non-zero alpha pixels=%zu\n",
        flySurf.Width(), flySurf.Height(), contentH, flyPixels);
    if (flyPixels < 1000) {
        std::printf("[FAIL] Flyout render produced too few pixels\n");
        return 1;
    }

    if (snap.ram.totalGb <= 0.0 || snap.cpu.logicalCores == 0) {
        std::printf("[FAIL] Invalid core CPU/RAM metrics\n");
        return 1;
    }

    // Sem nenhum contador PDH o app so mostra CPU/RAM: isso e falha, nao sucesso.
    if (!pdhCpu && !pdhDisk && !pdhGpu && !pdhGpuMem) {
        std::printf("[FAIL] No PDH counter could be opened (disk/GPU metrics unavailable)\n");
        return 1;
    }
    if (!pdhCpu) std::printf("[WARN] PDH CPU frequency counters unavailable (using fallback)\n");
    if (!pdhDisk) std::printf("[WARN] PDH disk counters unavailable\n");
    if (!pdhGpu) std::printf("[WARN] PDH GPU engine counter unavailable\n");
    if (!pdhGpuMem) std::printf("[WARN] PDH GPU memory counter unavailable (VRAM shown as n/a)\n");

    std::printf("[PASS] Self-test succeeded.\n");
    return 0;
}

bool HasSelfTestArg() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--self-test") == 0) {
                found = true;
                break;
            }
        }
        LocalFree(argv);
    }
    return found;
}

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    // DLLs carregadas em tempo de execucao: so de System32 (as importadas ja usam
    // /DEPENDENTLOADFLAG). Sem caixas de "insira um disco" ao consultar unidades vazias.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    // A consciencia de DPI (PerMonitorV2) vem do manifesto.

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(hrCo);

    int exitCode = 0;
    if (HasSelfTestArg()) {
        exitCode = RunSelfTest();
        if (comInitialized) CoUninitialize();
        return exitCode;
    }

    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"Local\\TaskbarPerf_SingleInstance");
    const DWORD mutexErr = GetLastError();
    // ACCESS_DENIED: o mutex existe mas foi criado com outra ACL -> tambem ha outra instancia.
    if ((hMutex && mutexErr == ERROR_ALREADY_EXISTS) || (!hMutex && mutexErr == ERROR_ACCESS_DENIED)) {
        if (HWND existing = FindWindowW(kControllerClassName, nullptr)) {
            PostMessageW(existing, WM_SHOW_FLYOUT_CMD, 0, 0);
        }
        if (hMutex) CloseHandle(hMutex);
        if (comInitialized) CoUninitialize();
        return 0;
    }
    if (!hMutex) {
        if (comInitialized) CoUninitialize();
        return 1;
    }

    {
        AppController app;
        if (app.Init(hInstance)) {
            exitCode = app.Run();
        } else {
            exitCode = 1;
        }
        app.Shutdown();
    }

    if (hMutex) {
        CloseHandle(hMutex);
    }
    if (comInitialized) CoUninitialize();
    return exitCode;
}
