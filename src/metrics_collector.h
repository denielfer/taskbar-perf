#pragma once

#include "metrics.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct IDXGIFactory1;

// Coletor de metricas. Nao e thread-safe: depois de Initialize(), so uma thread
// (a de coleta) deve chamar Sample()/RequestDiskRefresh().
class MetricsCollector {
public:
    MetricsCollector();
    ~MetricsCollector();

    MetricsCollector(const MetricsCollector&) = delete;
    MetricsCollector& operator=(const MetricsCollector&) = delete;

    bool Initialize();
    SystemSnapshot Sample(bool recordHistory = true);

    // Reconsulta discos/letras na proxima amostra (chamar em WM_DEVICECHANGE).
    void RequestDiskRefresh() { diskRefreshRequested_ = true; }

    // Refaz as linhas de base (CPU, rede, PDH) ao sair da pausa; a proxima Sample() mede ~1 s.
    void ResumeAfterPause();

    bool IsPdhCpuActive() const { return pdhCpuPerf_ != nullptr && pdhCpuFreq_ != nullptr; }
    bool IsPdhDiskActive() const { return pdhDiskIdle_ != nullptr; }
    bool IsPdhGpuActive() const { return pdhGpuEngine_ != nullptr; }
    bool IsPdhGpuMemActive() const { return pdhGpuMem_ != nullptr; }

private:
    struct DiskStaticInfo {
        std::wstring driveLetters;
        std::wstring mediaType;
    };

    struct NetPrevSample {
        uint64_t inOctets = 0;
        uint64_t outOctets = 0;
        uint64_t tickUs = 0;
        uint32_t lastSeenTick = 0;
        double maxScaleBps = 100000.0;
        RingBuffer<60> sendHistory;
        RingBuffer<60> recvHistory;
    };

    struct LuidKey {
        uint32_t low = 0;
        int32_t high = 0;
        bool operator==(const LuidKey& o) const { return low == o.low && high == o.high; }
    };

    struct LuidKeyHash {
        size_t operator()(const LuidKey& k) const {
            return (static_cast<size_t>(static_cast<uint32_t>(k.high)) << 32) | k.low;
        }
    };

    struct GpuState {
        RingBuffer<60> history;
        uint32_t lastSeenTick = 0;
        uint32_t kmtAdapter = 0;  // handle D3DKMT aberto (0 = nenhum)
        bool kmtOpenTried = false;
        uint32_t kmtRetryAtTick = 0;  // backoff quando o driver nao suporta a consulta
    };

    // Soma do contador "GPU Engine" por adaptador e engine ((phys << 16) | eng).
    struct GpuEngineSum {
        LuidKey luid;
        uint32_t engine = 0;
        double total = 0.0;
    };

    // Descricao estatica de um adaptador DXGI (reenumerado so quando a fabrica deixa de ser current).
    struct CachedAdapter {
        uint32_t luidLow = 0;
        int32_t luidHigh = 0;
        std::wstring name;
        uint32_t vendorId = 0;
        uint32_t deviceId = 0;
        uint32_t subSysId = 0;
        uint32_t revision = 0;
        uint64_t dedicatedVideoMemory = 0;
    };

    // Interface de rede exibivel. A lista so e refeita quando o Windows avisa de mudanca
    // (NotifyIpInterfaceChange/NotifyUnicastIpAddressChange) ou a cada ~1 min.
    struct NetIfaceInfo {
        uint64_t luid = 0;
        std::wstring name;
        std::wstring description;
        bool isPhysical = false;
    };

    struct DiskHistory {
        RingBuffer<60> history;
        uint32_t lastSeenTick = 0;
    };

    void ResetBaselines();
    void OpenPdhQuery();
    void AddMissingPdhCounters();
    void ClosePdh();
    template <typename Callback>
    void ReadPdhArray(void* hCounter, DWORD format, Callback&& callback);

    // requery = true reconsulta o tipo de todos os discos (disco trocado no mesmo indice).
    void RefreshDiskStaticCache(bool requery);
    static std::wstring QueryDiskMediaType(int diskIndex);
    void CloseGpuAdapter(GpuState& st);
    void PruneStaleEntries();
    void RebuildNetInterfaceList();
    void RegisterNetChangeNotifications();
    void RefreshAdapterCache();
    static void MarkNetListDirty(void* ctx);

    void CollectCpu(CpuMetrics& out, bool recordHistory);
    void CollectRam(RamMetrics& out, bool recordHistory);
    void CollectDisks(SystemSnapshot& out, bool recordHistory);
    void CollectNetworks(SystemSnapshot& out, uint64_t nowUs, bool recordHistory);
    void CollectGpus(SystemSnapshot& out, bool recordHistory);

    // PDH handles (nullptr = contador indisponivel)
    void* pdhQuery_ = nullptr;
    void* pdhCpuPerf_ = nullptr;
    void* pdhCpuFreq_ = nullptr;
    void* pdhDiskIdle_ = nullptr;
    void* pdhDiskRead_ = nullptr;
    void* pdhDiskWrite_ = nullptr;
    void* pdhGpuEngine_ = nullptr;
    void* pdhGpuMem_ = nullptr;

    // CPU (GetSystemTimes)
    uint64_t prevIdleTime_ = 0;
    uint64_t prevKernelTime_ = 0;
    uint64_t prevUserTime_ = 0;
    bool hasPrevCpuTimes_ = false;

    RingBuffer<60> cpuHistory_;
    RingBuffer<60> ramHistory_;
    std::map<int, DiskHistory> diskHistories_;
    std::unordered_map<uint64_t, NetPrevSample> netState_;
    std::unordered_map<LuidKey, GpuState, LuidKeyHash> gpuState_;

    // DXGI/D3DKMT (criados uma vez; a fabrica e recriada quando deixa de ser "current")
    IDXGIFactory1* dxgiFactory_ = nullptr;
    std::vector<CachedAdapter> adapters_;
    bool adaptersValid_ = false;
    void* fnKmtOpenFromLuid_ = nullptr;
    void* fnKmtQueryAdapterInfo_ = nullptr;
    void* fnKmtCloseAdapter_ = nullptr;
    std::vector<GpuEngineSum> gpuEngineScratch_;  // reaproveitado entre coletas

    // Rede: lista de interfaces em cache + avisos de mudanca do Windows
    std::vector<NetIfaceInfo> netIfaces_;
    bool netListValid_ = false;
    uint32_t netListBuiltTick_ = 0;
    std::atomic<bool> netListDirty_{true};
    void* netNotifyIf_ = nullptr;
    void* netNotifyAddr_ = nullptr;

    // Metadados estaticos dos discos (indice = N de PhysicalDriveN)
    std::map<int, DiskStaticInfo> diskStaticCache_;
    bool diskRefreshRequested_ = false;

    uint32_t tickCounter_ = 0;
    uint32_t logicalCores_ = 0;
    std::vector<uint64_t> pdhArrayBuffer_;
    std::vector<uint64_t> adapterAddrBuffer_;
};
