#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SystemSnapshot;

struct AppConfig {
    std::vector<std::wstring> pinnedIds;
    bool hasSavedPins = false;

    bool allMonitors = false;      // false = somente monitor principal (ignorado se monitorId nao for vazio)
    std::wstring monitorId;        // vazio = usa allMonitors; preenchido = exibe apenas nesse display
    bool alignRight = true;        // false = esquerda da barra, true = perto do relogio
    bool hideOnFullscreen = true;  // ocultar quando ha janela/jogo em tela cheia
    uint32_t refreshMs = 1000;

    bool IsPinned(const std::wstring& id) const;
    bool TogglePin(const std::wstring& id);
    void EnsureDefaultPins(const SystemSnapshot& snap);

    // Converte pins de versoes antigas ("gpu:<indice>", "gpu:luid:...", "net:<nome>") para o
    // formato estavel (NET_LUID na rede, ID PCI na GPU) quando o dispositivo aparece no snapshot.
    bool MigrateLegacyPins(const SystemSnapshot& snap);
};

AppConfig LoadConfig();
void SaveConfig(const AppConfig& cfg);

// true somente se a entrada de inicializacao aponta para o executavel atual.
bool IsAutoStartEnabled();
bool SetAutoStartEnabled(bool enabled);
