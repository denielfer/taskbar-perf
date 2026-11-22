# TaskbarPerf (C++20 / Win32 + Direct2D)

Monitor de desempenho leve para a barra de tarefas do Windows 11, com gráficos em tempo real inspirados no Gerenciador de Tarefas e painel completo em *flyout*.

> **Outra versão:** existe uma implementação independente da mesma ideia em Zig 0.15, no repositório [**`taskbar-perf-zig`**](https://github.com/denielfer/taskbar-perf-zig) (*TaskbarPerf Zig*). Ela ocupa o espaço livre da metade esquerda da barra (medido por UI Automation) e tem um painel próprio. Esta versão em C++ é a mais completa em configuração: escolha de métricas, de monitor e de posição, além de iniciar com o Windows.

## Recursos

- **Barra de tarefas (sempre visível):** mini gráficos em tempo real com transparência real (`UpdateLayeredWindow`) para **CPU**, **Memória (RAM)**, **GPU (+ °C)**, **Disco** e **Rede**.
- **Painel completo (*Flyout*):** mostra CPU (% e GHz), RAM (usada/total GB e %), todos os discos físicos (`HDD (SATA)`, `SSD (NVMe)`, letras e %/MB/s), interfaces de rede ativas (`E:` / `R:`) e GPUs (% + temperatura °C via WDDM `D3DKMT` sem privilégios de administrador + VRAM).
- **Controles:**
  - **Clique esquerdo** na barra (ou no ícone da bandeja): abre/fecha o painel completo.
  - **Clique direito** na barra (ou na bandeja): abre o menu para escolher métricas visíveis, escolher **somente monitor principal**, **todos os monitores** ou um **display específico**, posição (**esquerda** ou **perto do relógio**), ocultar em tela cheia e iniciar com o Windows.
  - No painel: roda do mouse rola a lista, **Esc** fecha, clique duplo num cartão abre o Gerenciador de Tarefas.
  - Abrir o `TaskbarPerf.exe` de novo com ele já rodando abre/fecha o painel da instância existente.
- **Leve em segundo plano:** a coleta roda numa thread própria (a barra nunca trava esperando driver de GPU/rede/disco) e **pausa com a tela desligada ou a sessão bloqueada**. Discos são reconsultados quando um disco é conectado/removido.
- **Zero dependências externas:** binário nativo único usando apenas Windows SDK (`Direct2D`, `DirectWrite`, `DXGI`, `PDH`, `IPHlpApi`, `D3DKMT`).

## Como compilar

Requer Visual Studio ou Build Tools (MSVC x64) + Windows SDK. O `build.bat` encontra a instalação sozinho (via `vswhere`):

```bat
build.bat
```

O executável será gerado em `build\TaskbarPerf.exe`.

Alternativa com CMake (mesmas flags):

```bat
cmake -S . -B build-cmake -A x64
cmake --build build-cmake --config Release
```

Os dois builds ligam o *hardening* do binário: `/sdl`, Control Flow Guard (`/guard:cf`), CET shadow stack (`/CETCOMPAT`) e `/DEPENDENTLOADFLAG:0x800` (as DLLs do sistema são carregadas só de `System32`, nunca da pasta do executável). Só há suporte a x64.

## Configuração

As opções do menu ficam em `%APPDATA%\TaskbarPerf\config.ini` (UTF-16). Além delas, dá para ajustar à mão:

| Seção / chave | Padrão | Significado |
|---|---|---|
| `[Behavior] RefreshMs` | `1000` | Intervalo de atualização em ms (250 a 5000). |
| `[Behavior] AllMonitors` | `0` | `1` = barra em todos os monitores. |
| `[Behavior] MonitorId` | (vazio) | ID do display específico escolhido no menu (tem prioridade sobre `AllMonitors`). |
| `[Behavior] AlignRight` | `1` | `1` = perto do relógio; `0` = esquerda da barra. |
| `[Behavior] HideOnFullscreen` | `1` | Ocultar com jogo/vídeo em tela cheia. |
| `[Metrics] PinnedItems` | (automático) | Itens fixados, separados por `;`. A rede é identificada pelo NET_LUID (`net:luid:…`), que sobrevive a reinícios e a renomear o adaptador; a GPU pelo ID PCI (`gpu:pci:fabricante-dispositivo-subsistema-revisão-n`), porque o LUID da placa de vídeo muda a cada boot. Configurações antigas são convertidas sozinhas. |

## Como executar e testar

- Iniciar normalmente: `build\TaskbarPerf.exe`
- Verificação rápida de coleta + renderização offscreen: `build\TaskbarPerf.exe --self-test` (falha se nenhum contador PDH abrir; avisa `[WARN]` para contadores ausentes).

## Limitações conhecidas

- Se mudar a posição para **esquerda** (`AlignRight=0`), a barra é desenhada a partir da borda esquerda da barra de tarefas e pode cobrir o botão de Widgets (ícones centralizados) ou o Iniciar (ícones alinhados à esquerda). Por padrão ela fica na **Direita (perto do relógio)**.
- Sem o contador PDH `GPU Adapter Memory`, a VRAM em uso aparece como `--` (o Windows não expõe o uso total por outra API sem privilégios).
