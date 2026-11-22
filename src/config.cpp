#include "config.h"
#include "metrics.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>
#include <string>

namespace {

constexpr wchar_t kRunRegPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"TaskbarPerf";
constexpr wchar_t kPinSeparator = L';';

std::wstring GetExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 6; ++attempt) {
        const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size()) {
            buf.resize(len);
            return buf;
        }
        buf.resize(buf.size() * 2);  // caminho longo: buffer truncado, tenta maior
    }
    return {};
}

std::wstring GetConfigDirPath() {
    PWSTR appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
        dir = appData;
    }
    CoTaskMemFree(appData);

    if (dir.empty()) {
        const DWORD need = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
        if (need > 0) {
            std::wstring env(need, L'\0');
            const DWORD got = GetEnvironmentVariableW(L"APPDATA", env.data(), need);
            if (got > 0 && got < need) {
                env.resize(got);
                dir = env;
            }
        }
    }
    if (dir.empty()) {
        const std::wstring exePath = GetExePath();
        const size_t slash = exePath.find_last_of(L"\\/");
        return (slash != std::wstring::npos) ? exePath.substr(0, slash) : L".";
    }
    return dir + L"\\TaskbarPerf";
}

bool WriteUtf16File(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    const unsigned char bom[] = {0xFF, 0xFE};
    DWORD written = 0;
    bool ok = WriteFile(h, bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    if (ok && !text.empty()) {
        const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
        ok = WriteFile(h, text.data(), bytes, &written, nullptr) && written == bytes;
    }
    CloseHandle(h);
    return ok;
}

// WritePrivateProfileStringW so grava Unicode se o arquivo ja existir com BOM
// UTF-16LE; senao grava em ANSI e corrompe nomes fora da code page. Cria o arquivo
// com BOM ou converte um config.ini antigo (ANSI) para UTF-16LE.
void EnsureUnicodeIni(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            WriteUtf16File(path, {});
        }
        return;
    }
    unsigned char bom[2]{};
    DWORD bomGot = 0;
    if (!ReadFile(h, bom, sizeof(bom), &bomGot, nullptr)) {
        CloseHandle(h);
        return;
    }
    if (bomGot == 2 && bom[0] == 0xFF && bom[1] == 0xFE) {
        CloseHandle(h);
        return;  // ja e UTF-16LE: evita ler o arquivo inteiro
    }

    LARGE_INTEGER size{};
    std::string ansi;
    bool readOk = GetFileSizeEx(h, &size) && size.QuadPart < (1 << 20);
    if (readOk && size.QuadPart > 0) {
        const size_t total = static_cast<size_t>(size.QuadPart);
        ansi.resize(total);
        const size_t prefix = std::min<size_t>(bomGot, total);
        for (size_t i = 0; i < prefix; ++i) {
            ansi[i] = static_cast<char>(bom[i]);
        }
        if (total > prefix) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(total - prefix);
            readOk = ReadFile(h, ansi.data() + prefix, want, &got, nullptr) && got == want;
        }
    }
    CloseHandle(h);
    if (!readOk) {
        return;
    }

    // Arquivo editado a mao pode estar em UTF-8 (padrao do Bloco de Notas): se for UTF-8
    // valido, usa UTF-8; senao e o ANSI que o WritePrivateProfileString gravava.
    size_t skip = 0;
    if (ansi.size() >= 3 && static_cast<unsigned char>(ansi[0]) == 0xEF &&
        static_cast<unsigned char>(ansi[1]) == 0xBB && static_cast<unsigned char>(ansi[2]) == 0xBF) {
        skip = 3;
    }
    std::wstring wide;
    if (ansi.size() > skip) {
        const char* src = ansi.data() + skip;
        const int srcLen = static_cast<int>(ansi.size() - skip);
        UINT codePage = CP_UTF8;
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, srcLen, nullptr, 0);
        if (n <= 0) {
            codePage = CP_ACP;
            n = MultiByteToWideChar(CP_ACP, 0, src, srcLen, nullptr, 0);
        }
        if (n <= 0) return;
        wide.resize(static_cast<size_t>(n));
        MultiByteToWideChar(codePage, 0, src, srcLen, wide.data(), n);
    }
    // Grava ao lado e troca de uma vez, para nao deixar um config.ini pela metade.
    const std::wstring tmp = path + L".tmp";
    if (WriteUtf16File(tmp, wide)) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            // Descarta o cache de INI do Windows, que ainda pode ter o arquivo antigo.
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
        } else {
            DeleteFileW(tmp.c_str());
        }
    }
}

std::wstring ReadIniString(const wchar_t* section, const wchar_t* key, const std::wstring& ini) {
    std::wstring buf(1024, L'\0');
    for (int attempt = 0; attempt < 8; ++attempt) {
        const DWORD len = GetPrivateProfileStringW(
            section, key, L"", buf.data(), static_cast<DWORD>(buf.size()), ini.c_str());
        if (len + 1 < buf.size()) {  // len == size-1 indica truncamento
            buf.resize(len);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
    return {};
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

bool AppConfig::IsPinned(const std::wstring& id) const {
    return std::find(pinnedIds.begin(), pinnedIds.end(), id) != pinnedIds.end();
}

bool AppConfig::TogglePin(const std::wstring& id) {
    if (id.empty() || id.find_first_of(L";\r\n") != std::wstring::npos) {
        return false;
    }
    auto it = std::find(pinnedIds.begin(), pinnedIds.end(), id);
    if (it != pinnedIds.end()) {
        if (pinnedIds.size() <= 1) {
            return false;
        }
        pinnedIds.erase(it);
        return true;
    }
    pinnedIds.push_back(id);
    return true;
}

void AppConfig::EnsureDefaultPins(const SystemSnapshot& snap) {
    if (hasSavedPins && !pinnedIds.empty()) {
        return;
    }
    pinnedIds.clear();
    pinnedIds.push_back(L"cpu");
    pinnedIds.push_back(L"ram");

    if (!snap.gpus.empty()) {
        pinnedIds.push_back(PinIdForGpu(snap.gpus[0]));
    }

    if (!snap.disks.empty()) {
        const DiskMetrics* chosen = &snap.disks[0];
        for (const auto& d : snap.disks) {
            if (d.driveLetters.find(L"C:") != std::wstring::npos) {
                chosen = &d;
                break;
            }
        }
        pinnedIds.push_back(PinIdForDisk(*chosen));
    }

    if (!snap.nets.empty()) {
        const NetMetrics* chosen = &snap.nets[0];
        for (const auto& n : snap.nets) {
            if (n.isPhysical) {
                chosen = &n;
                break;
            }
        }
        pinnedIds.push_back(PinIdForNet(*chosen));
    }
}

bool AppConfig::MigrateLegacyPins(const SystemSnapshot& snap) {
    bool changed = false;
    for (auto& id : pinnedIds) {
        std::wstring replacement;
        if (StartsWith(id, L"gpu:luid:")) {
            // LUID so e valido no mesmo boot; se nao bater e houver uma unica GPU, e ela.
            for (const auto& g : snap.gpus) {
                if (LegacyLuidPinIdForGpu(g) == id) {
                    replacement = PinIdForGpu(g);
                    break;
                }
            }
            if (replacement.empty() && snap.gpus.size() == 1) {
                replacement = PinIdForGpu(snap.gpus[0]);
            }
        } else if (StartsWith(id, L"gpu:") && !StartsWith(id, L"gpu:pci:")) {
            const std::wstring idxStr = id.substr(4);
            if (!idxStr.empty() && idxStr.size() <= 3 &&
                std::all_of(idxStr.begin(), idxStr.end(), [](wchar_t c) { return std::iswdigit(c) != 0; })) {
                const int idx = std::stoi(idxStr);
                for (const auto& g : snap.gpus) {
                    if (g.index == idx) {
                        replacement = PinIdForGpu(g);
                        break;
                    }
                }
            }
        } else if (StartsWith(id, L"net:") && !StartsWith(id, L"net:luid:")) {
            const std::wstring name = id.substr(4);
            for (const auto& n : snap.nets) {
                if (n.name == name) {
                    replacement = PinIdForNet(n);
                    break;
                }
            }
        }
        if (!replacement.empty() && replacement != id) {
            id = std::move(replacement);
            changed = true;
        }
    }
    if (changed) {
        // Remove duplicatas que a migracao possa ter criado.
        std::vector<std::wstring> unique;
        for (auto& id : pinnedIds) {
            if (std::find(unique.begin(), unique.end(), id) == unique.end()) {
                unique.push_back(std::move(id));
            }
        }
        pinnedIds = std::move(unique);
    }
    return changed;
}

AppConfig LoadConfig() {
    AppConfig cfg;
    const std::wstring ini = GetConfigDirPath() + L"\\config.ini";

    const std::wstring raw = ReadIniString(L"Metrics", L"PinnedItems", ini);
    size_t start = 0;
    while (start < raw.size()) {
        const size_t sep = raw.find(kPinSeparator, start);
        const std::wstring token = (sep == std::wstring::npos)
            ? raw.substr(start)
            : raw.substr(start, sep - start);
        if (!token.empty() && !cfg.IsPinned(token)) {
            cfg.pinnedIds.push_back(token);
        }
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }
    cfg.hasSavedPins = !cfg.pinnedIds.empty();

    cfg.allMonitors = GetPrivateProfileIntW(L"Behavior", L"AllMonitors", 0, ini.c_str()) != 0;
    cfg.monitorId = ReadIniString(L"Behavior", L"MonitorId", ini);
    cfg.monitorId.erase(
        std::remove_if(cfg.monitorId.begin(), cfg.monitorId.end(),
            [](wchar_t c) { return c == L'\r' || c == L'\n'; }),
        cfg.monitorId.end());
    if (!cfg.monitorId.empty()) {
        cfg.allMonitors = false;
    }
    cfg.alignRight = GetPrivateProfileIntW(L"Behavior", L"AlignRight", 1, ini.c_str()) != 0;
    cfg.hideOnFullscreen = GetPrivateProfileIntW(L"Behavior", L"HideOnFullscreen", 1, ini.c_str()) != 0;

    const UINT refresh = GetPrivateProfileIntW(L"Behavior", L"RefreshMs", 1000, ini.c_str());
    cfg.refreshMs = (refresh >= 250 && refresh <= 5000) ? refresh : 1000;

    return cfg;
}

void SaveConfig(const AppConfig& cfg) {
    const std::wstring dir = GetConfigDirPath();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const std::wstring ini = dir + L"\\config.ini";
    EnsureUnicodeIni(ini);

    std::wstring joined;
    for (const auto& id : cfg.pinnedIds) {
        // IDs novos nunca contem ';' nem quebra de linha; descarta qualquer legado que contenha.
        if (id.empty() || id.find_first_of(L";\r\n") != std::wstring::npos) continue;
        if (!joined.empty()) joined += kPinSeparator;
        joined += id;
    }
    WritePrivateProfileStringW(L"Metrics", L"PinnedItems", joined.c_str(), ini.c_str());

    auto writeBool = [&](const wchar_t* sec, const wchar_t* key, bool val) {
        WritePrivateProfileStringW(sec, key, val ? L"1" : L"0", ini.c_str());
    };
    writeBool(L"Behavior", L"AllMonitors", cfg.allMonitors);
    const std::wstring safeMonitorId =
        (cfg.monitorId.find_first_of(L"\r\n") == std::wstring::npos) ? cfg.monitorId : L"";
    WritePrivateProfileStringW(L"Behavior", L"MonitorId", safeMonitorId.c_str(), ini.c_str());
    writeBool(L"Behavior", L"AlignRight", cfg.alignRight);
    writeBool(L"Behavior", L"HideOnFullscreen", cfg.hideOnFullscreen);

    const std::wstring refreshStr = std::to_wstring(cfg.refreshMs);
    WritePrivateProfileStringW(L"Behavior", L"RefreshMs", refreshStr.c_str(), ini.c_str());
}

bool IsAutoStartEnabled() {
    DWORD cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunRegPath, kRunValueName, RRF_RT_REG_SZ, nullptr, nullptr, &cb) != ERROR_SUCCESS ||
        cb < 2 * sizeof(wchar_t)) {
        return false;
    }
    std::wstring value((cb + sizeof(wchar_t) - 1) / sizeof(wchar_t), L'\0');
    DWORD bufBytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kRunRegPath, kRunValueName, RRF_RT_REG_SZ, nullptr, value.data(), &bufBytes) != ERROR_SUCCESS) {
        return false;
    }
    value.resize(wcsnlen(value.c_str(), value.size()));

    // Valor gravado: "C:\caminho\TaskbarPerf.exe" (com aspas)
    if (value.size() >= 2 && value.front() == L'"') {
        const size_t close = value.find(L'"', 1);
        value = value.substr(1, (close == std::wstring::npos ? value.size() : close) - 1);
    }
    const std::wstring exe = GetExePath();
    return !exe.empty() && EqualsIgnoreCase(value, exe);
}

bool SetAutoStartEnabled(bool enabled) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LSTATUS st = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring exe = GetExePath();
        if (exe.empty()) {
            RegCloseKey(hKey);
            return false;
        }
        const std::wstring quoted = L"\"" + exe + L"\"";
        st = RegSetValueExW(
            hKey,
            kRunValueName,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(quoted.c_str()),
            static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(hKey, kRunValueName);
        if (st == ERROR_FILE_NOT_FOUND) {
            st = ERROR_SUCCESS;
        }
    }

    RegCloseKey(hKey);
    return st == ERROR_SUCCESS;
}
