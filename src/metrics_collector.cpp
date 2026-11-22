#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_4.h>
#include <winioctl.h>
#include <powrprof.h>
#include <wrl/client.h>

#include "metrics_collector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cwctype>
#include <set>
#include <string_view>

using Microsoft::WRL::ComPtr;

// As structs D3DKMT abaixo replicam o layout de d3dkmthk.h para x64. Em 32 bits
// (D3DKMT_PTR/D3DKMT_ALIGN64) o layout seria outro.
static_assert(sizeof(void*) == 8, "TaskbarPerf so suporta x64 (structs D3DKMT locais)");

namespace {

struct LocalProcessorPowerInfo {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

struct LocalD3DKMT_OPENADAPTERFROMLUID {
    LUID AdapterLuid;
    UINT hAdapter;
};

struct LocalD3DKMT_QUERYADAPTERINFO {
    UINT hAdapter;
    UINT Type;
    void* pPrivateDriverData;
    UINT PrivateDriverDataSize;
};

struct LocalD3DKMT_ADAPTER_PERFDATA {
    UINT32 PhysicalAdapterIndex;
    UINT64 MemoryFrequency;
    UINT64 MaxMemoryFrequency;
    UINT64 MaxMemoryFrequencyOC;
    UINT64 MemoryBandwidth;
    UINT64 PCIEBandwidth;
    UINT32 FanRPM;
    UINT32 Power;
    UINT32 Temperature;  // decimos de grau Celsius
    UINT8 PowerStateOverride;
};
static_assert(sizeof(LocalD3DKMT_ADAPTER_PERFDATA) == 64, "layout de D3DKMT_ADAPTER_PERFDATA");

struct LocalD3DKMT_CLOSEADAPTER {
    UINT hAdapter;
};

constexpr UINT kKmtQaiTypeAdapterPerfData = 62;  // KMTQAITYPE_ADAPTERPERFDATA

using FnD3DKMTOpenAdapterFromLuid = LONG(NTAPI*)(LocalD3DKMT_OPENADAPTERFROMLUID*);
using FnD3DKMTQueryAdapterInfo = LONG(NTAPI*)(LocalD3DKMT_QUERYADAPTERINFO*);
using FnD3DKMTCloseAdapter = LONG(NTAPI*)(LocalD3DKMT_CLOSEADAPTER*);

constexpr uint32_t kPdhRetryEveryTicks = 60;     // tenta reabrir contadores ausentes a cada ~1 min
constexpr uint32_t kPruneEveryTicks = 60;
constexpr uint32_t kStaleAfterTicks = 300;       // remove estado de dispositivos sumidos ha ~5 min
constexpr uint32_t kDiskFallbackRefreshTicks = 300;
constexpr int kMaxPhysicalDrives = 64;
constexpr uint32_t kKmtRetryBackoffTicks = 300;  // ~5 min quando a temperatura nao e suportada
const wchar_t kGenericDiskType[] = L"Disco";

uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint64_t GetTickMicroseconds() {
    static const uint64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    if (freq == 0) {
        return GetTickCount64() * 1000ULL;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const uint64_t q = static_cast<uint64_t>(now.QuadPart);
    // Divide antes de multiplicar: q * 1e6 estouraria 64 bits com ~10 dias de uptime.
    return (q / freq) * 1000000ULL + ((q % freq) * 1000000ULL) / freq;
}

double SnapNiceScaleBps(double rawMax) {
    static constexpr double kSteps[] = {
        100e3, 250e3, 500e3,
        1e6, 2.5e6, 5e6, 10e6, 25e6, 50e6, 100e6, 250e6, 500e6,
        1e9, 2.5e9, 5e9, 10e9
    };
    const double target = rawMax * 1.15;
    for (double step : kSteps) {
        if (target <= step) {
            return step;
        }
    }
    return std::max(target, 10e9);
}

bool ContainsCaseInsensitive(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
    return it != haystack.end();
}

int ParseLeadingDiskIndex(std::wstring_view instanceName) {
    if (instanceName.empty() || instanceName == L"_Total") {
        return -1;
    }
    int value = 0;
    size_t pos = 0;
    while (pos < instanceName.size() && pos < 4 && std::iswdigit(instanceName[pos])) {
        value = value * 10 + static_cast<int>(instanceName[pos] - L'0');
        ++pos;
    }
    return (pos == 0) ? -1 : value;
}

bool ParseHexU32(std::wstring_view sv, uint32_t& out) {
    if (sv.size() >= 2 && sv[0] == L'0' && (sv[1] == L'x' || sv[1] == L'X')) {
        sv.remove_prefix(2);
    }
    if (sv.empty() || sv.size() > 8) {
        return false;
    }
    uint32_t v = 0;
    for (wchar_t ch : sv) {
        v <<= 4;
        if (ch >= L'0' && ch <= L'9') v |= static_cast<uint32_t>(ch - L'0');
        else if (ch >= L'a' && ch <= L'f') v |= static_cast<uint32_t>(ch - L'a' + 10);
        else if (ch >= L'A' && ch <= L'F') v |= static_cast<uint32_t>(ch - L'A' + 10);
        else return false;
    }
    out = v;
    return true;
}

bool IsValidPdhStatus(DWORD cstatus) {
    return cstatus == PDH_CSTATUS_VALID_DATA || cstatus == PDH_CSTATUS_NEW_DATA;
}

bool ParseGpuLuid(std::wstring_view name, uint32_t& luidLow, int32_t& luidHigh, std::wstring_view* tailOut = nullptr) {
    const size_t luidPos = name.find(L"luid_");
    if (luidPos == std::wstring_view::npos) {
        return false;
    }
    std::wstring_view rest = name.substr(luidPos + 5);
    const size_t u1 = rest.find(L'_');
    if (u1 == std::wstring_view::npos) return false;
    const std::wstring_view highStr = rest.substr(0, u1);
    rest.remove_prefix(u1 + 1);

    const size_t u2 = rest.find(L'_');
    const std::wstring_view lowStr = (u2 == std::wstring_view::npos) ? rest : rest.substr(0, u2);
    if (u2 != std::wstring_view::npos) {
        rest.remove_prefix(u2 + 1);
    } else {
        rest = {};
    }

    uint32_t hi = 0;
    if (!ParseHexU32(highStr, hi) || !ParseHexU32(lowStr, luidLow)) {
        return false;
    }
    luidHigh = static_cast<int32_t>(hi);
    if (tailOut) {
        *tailOut = rest;
    }
    return true;
}

HANDLE OpenDeviceNoAccess(const wchar_t* path) {
    // Acesso 0: so consultas (IOCTL de propriedades), sem ler a midia.
    return CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

// Retorna os numeros de disco fisico que compoem o volume (vazio se falhar).
std::vector<int> QueryVolumeDiskNumbers(HANDLE hVol) {
    std::vector<int> disks;
    // Buffer de uint64_t garante o alinhamento de 8 bytes exigido por DISK_EXTENT.
    size_t extentsCap = 4;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const size_t bytes = offsetof(VOLUME_DISK_EXTENTS, Extents) + extentsCap * sizeof(DISK_EXTENT);
        std::vector<uint64_t> buf((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        DWORD ret = 0;
        const BOOL ok = DeviceIoControl(
            hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
            buf.data(), static_cast<DWORD>(buf.size() * sizeof(uint64_t)), &ret, nullptr);
        const auto* ext = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buf.data());
        if (ok) {
            const size_t returned = (ret >= offsetof(VOLUME_DISK_EXTENTS, Extents))
                ? (ret - offsetof(VOLUME_DISK_EXTENTS, Extents)) / sizeof(DISK_EXTENT)
                : 0;
            const size_t count = std::min<size_t>({ext->NumberOfDiskExtents, returned, extentsCap});
            for (size_t e = 0; e < count; ++e) {
                disks.push_back(static_cast<int>(ext->Extents[e].DiskNumber));
            }
            return disks;
        }
        if (GetLastError() != ERROR_MORE_DATA || ret < sizeof(DWORD) ||
            ext->NumberOfDiskExtents <= extentsCap || ext->NumberOfDiskExtents > 1024) {
            return disks;
        }
        extentsCap = ext->NumberOfDiskExtents;
    }
    return disks;
}

} // namespace

MetricsCollector::MetricsCollector() {
    logicalCores_ = std::max<uint32_t>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

    if (HMODULE hGdi = GetModuleHandleW(L"gdi32.dll")) {
        fnKmtOpenFromLuid_ = reinterpret_cast<void*>(GetProcAddress(hGdi, "D3DKMTOpenAdapterFromLuid"));
        fnKmtQueryAdapterInfo_ = reinterpret_cast<void*>(GetProcAddress(hGdi, "D3DKMTQueryAdapterInfo"));
        fnKmtCloseAdapter_ = reinterpret_cast<void*>(GetProcAddress(hGdi, "D3DKMTCloseAdapter"));
    }
}

MetricsCollector::~MetricsCollector() {
    for (auto& [_, st] : gpuState_) {
        CloseGpuAdapter(st);
    }
    if (dxgiFactory_) {
        dxgiFactory_->Release();
        dxgiFactory_ = nullptr;
    }
    ClosePdh();
}

bool MetricsCollector::Initialize() {
    OpenPdhQuery();
    AddMissingPdhCounters();
    RefreshDiskStaticCache(true);
    ResetBaselines();
    return true;
}

void MetricsCollector::ResumeAfterPause() {
    // Sem isso a primeira amostra depois da pausa mediria CPU, rede e PDH contra os valores
    // de antes dela e mostraria a media de todo o periodo pausado como se fosse 1 s.
    ResetBaselines();
}

void MetricsCollector::ResetBaselines() {
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        prevIdleTime_ = FileTimeToU64(idle);
        prevKernelTime_ = FileTimeToU64(kernel);
        prevUserTime_ = FileTimeToU64(user);
        hasPrevCpuTimes_ = true;
    }

    // Linha de base da rede
    MIB_IF_TABLE2* ifTable = nullptr;
    if (GetIfTable2(&ifTable) == NO_ERROR && ifTable) {
        const uint64_t nowUs = GetTickMicroseconds();
        for (ULONG i = 0; i < ifTable->NumEntries; ++i) {
            const MIB_IF_ROW2& row = ifTable->Table[i];
            if (row.OperStatus == IfOperStatusUp && row.Type != IF_TYPE_SOFTWARE_LOOPBACK) {
                NetPrevSample& st = netState_[row.InterfaceLuid.Value];
                st.inOctets = row.InOctets;
                st.outOctets = row.OutOctets;
                st.tickUs = nowUs;
            }
        }
        FreeMibTable(ifTable);
    }

    if (pdhQuery_) {
        PdhCollectQueryData(static_cast<PDH_HQUERY>(pdhQuery_));
    }
}

void MetricsCollector::OpenPdhQuery() {
    ClosePdh();
    PDH_HQUERY hq = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &hq) == ERROR_SUCCESS && hq) {
        pdhQuery_ = hq;
    }
}

void MetricsCollector::AddMissingPdhCounters() {
    if (!pdhQuery_) {
        OpenPdhQuery();
        if (!pdhQuery_) return;
    }
    auto hq = static_cast<PDH_HQUERY>(pdhQuery_);
    auto addIfMissing = [&](const wchar_t* path, void*& counter) {
        if (counter) return;
        PDH_HCOUNTER hc = nullptr;
        if (PdhAddEnglishCounterW(hq, path, 0, &hc) == ERROR_SUCCESS && hc) {
            counter = hc;
        }
    };

    addIfMissing(L"\\Processor Information(_Total)\\% Processor Performance", pdhCpuPerf_);
    addIfMissing(L"\\Processor Information(_Total)\\Processor Frequency", pdhCpuFreq_);
    addIfMissing(L"\\PhysicalDisk(*)\\% Idle Time", pdhDiskIdle_);
    addIfMissing(L"\\PhysicalDisk(*)\\Disk Read Bytes/sec", pdhDiskRead_);
    addIfMissing(L"\\PhysicalDisk(*)\\Disk Write Bytes/sec", pdhDiskWrite_);
    addIfMissing(L"\\GPU Engine(*)\\Utilization Percentage", pdhGpuEngine_);
    addIfMissing(L"\\GPU Adapter Memory(*)\\Dedicated Usage", pdhGpuMem_);
}

void MetricsCollector::ClosePdh() {
    if (pdhQuery_) {
        PdhCloseQuery(static_cast<PDH_HQUERY>(pdhQuery_));
        pdhQuery_ = nullptr;
    }
    pdhCpuPerf_ = nullptr;
    pdhCpuFreq_ = nullptr;
    pdhDiskIdle_ = nullptr;
    pdhDiskRead_ = nullptr;
    pdhDiskWrite_ = nullptr;
    pdhGpuEngine_ = nullptr;
    pdhGpuMem_ = nullptr;
}

template <typename Callback>
void MetricsCollector::ReadPdhArray(void* hCounter, DWORD format, Callback&& callback) {
    if (!hCounter) return;
    DWORD bufSz = 0;
    DWORD itemCount = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(
        static_cast<PDH_HCOUNTER>(hCounter), format, &bufSz, &itemCount, nullptr);
    if (st != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bufSz == 0) {
        return;
    }
    const size_t needWords = (bufSz + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    if (pdhArrayBuffer_.size() < needWords) {
        pdhArrayBuffer_.resize(needWords);
    }
    bufSz = static_cast<DWORD>(pdhArrayBuffer_.size() * sizeof(uint64_t));
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(pdhArrayBuffer_.data());
    st = PdhGetFormattedCounterArrayW(
        static_cast<PDH_HCOUNTER>(hCounter), format, &bufSz, &itemCount, items);
    if (st != ERROR_SUCCESS) {
        return;
    }
    for (DWORD i = 0; i < itemCount; ++i) {
        if (!items[i].szName || !IsValidPdhStatus(items[i].FmtValue.CStatus)) continue;
        const double v = items[i].FmtValue.doubleValue;
        if (!std::isfinite(v)) continue;
        callback(std::wstring_view(items[i].szName), v);
    }
}

std::wstring MetricsCollector::QueryDiskMediaType(int diskIndex) {
    wchar_t devPath[64]{};
    swprintf_s(devPath, L"\\\\.\\PhysicalDrive%d", diskIndex);

    bool knownSeek = false;
    bool incursSeek = true;
    STORAGE_BUS_TYPE busType = BusTypeUnknown;

    HANDLE hDev = OpenDeviceNoAccess(devPath);
    if (hDev != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY q{};
        q.QueryType = PropertyStandardQuery;
        q.PropertyId = StorageDeviceSeekPenaltyProperty;
        DEVICE_SEEK_PENALTY_DESCRIPTOR spd{};
        DWORD bytesRet = 0;
        if (DeviceIoControl(hDev, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &spd, sizeof(spd), &bytesRet, nullptr)) {
            knownSeek = true;
            incursSeek = (spd.IncursSeekPenalty != FALSE);
        }

        // Barramento do proprio dispositivo (nao da controladora: com RAID/RST
        // a controladora aparece como RAID mesmo com um NVMe atras dela).
        q.PropertyId = StorageDeviceProperty;
        std::vector<uint64_t> devBuf(512 / sizeof(uint64_t));
        if (DeviceIoControl(hDev, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                devBuf.data(), static_cast<DWORD>(devBuf.size() * sizeof(uint64_t)), &bytesRet, nullptr) &&
            bytesRet >= offsetof(STORAGE_DEVICE_DESCRIPTOR, BusType) + sizeof(STORAGE_BUS_TYPE)) {
            busType = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(devBuf.data())->BusType;
        }
        CloseHandle(hDev);
    }

    if (busType == BusTypeNvme) {
        return L"SSD (NVMe)";
    }
    if (knownSeek) {
        if (!incursSeek) {
            if (busType == BusTypeSata) return L"SSD (SATA)";
            if (busType == BusTypeUsb) return L"SSD (USB)";
            return L"SSD";
        }
        if (busType == BusTypeUsb) return L"HDD (USB)";
        if (busType == BusTypeSas) return L"HDD (SAS)";
        if (busType == BusTypeSata) return L"HDD (SATA)";
        return L"HDD";
    }
    if (busType == BusTypeUsb) return L"Unidade (USB)";
    if (busType == BusTypeSd || busType == BusTypeMmc) return L"Cart\u00E3o";
    return kGenericDiskType;
}

void MetricsCollector::RefreshDiskStaticCache(bool requery) {
    std::map<int, std::set<wchar_t>> diskLetters;

    // 1. Discos fisicos: o nome \\.\PhysicalDriveN ja e o numero do disco (o mesmo
    //    indice do PDH). Numeros podem ter buracos (disco removido), por isso sonda todos.
    for (int n = 0; n < kMaxPhysicalDrives; ++n) {
        wchar_t devPath[64]{};
        swprintf_s(devPath, L"\\\\.\\PhysicalDrive%d", n);
        HANDLE h = OpenDeviceNoAccess(devPath);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            diskLetters[n];
        }
    }

    // 2. Letras de unidade -> disco fisico
    wchar_t drivesBuf[512]{};
    DWORD len = GetLogicalDriveStringsW(ARRAYSIZE(drivesBuf) - 1, drivesBuf);
    if (len >= ARRAYSIZE(drivesBuf) - 1) {
        len = 0;  // nao coube (impossivel com 26 letras): ignora em vez de ler lixo
    }
    for (const wchar_t* p = drivesBuf; p < drivesBuf + len && *p != L'\0'; p += wcslen(p) + 1) {
        const UINT driveType = GetDriveTypeW(p);
        if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE) {
            continue;
        }
        const wchar_t letter = static_cast<wchar_t>(std::towupper(p[0]));
        wchar_t volPath[16]{};
        swprintf_s(volPath, L"\\\\.\\%c:", letter);
        HANDLE hVol = OpenDeviceNoAccess(volPath);
        if (hVol == INVALID_HANDLE_VALUE) {
            continue;
        }
        for (int disk : QueryVolumeDiskNumbers(hVol)) {
            if (disk >= 0 && disk < kMaxPhysicalDrives) {
                diskLetters[disk].insert(letter);
            }
        }
        CloseHandle(hVol);
    }

    std::map<int, DiskStaticInfo> fresh;
    for (const auto& [idx, letters] : diskLetters) {
        DiskStaticInfo info;
        for (wchar_t ch : letters) {
            if (!info.driveLetters.empty()) info.driveLetters += L' ';
            info.driveLetters += ch;
            info.driveLetters += L':';
        }
        // Reaproveita o tipo so na atualizacao periodica, e nunca o generico "Disco"
        // (o driver de um USB recem-conectado pode ainda nao responder). Em conexao/remocao
        // reconsulta tudo: outro dispositivo pode ter assumido o mesmo indice.
        auto old = diskStaticCache_.find(idx);
        const bool reuse = !requery && old != diskStaticCache_.end() &&
                           !old->second.mediaType.empty() && old->second.mediaType != kGenericDiskType;
        info.mediaType = reuse ? old->second.mediaType : QueryDiskMediaType(idx);
        fresh[idx] = std::move(info);
    }
    diskStaticCache_ = std::move(fresh);
}

void MetricsCollector::CloseGpuAdapter(GpuState& st) {
    if (st.kmtAdapter != 0 && fnKmtCloseAdapter_) {
        LocalD3DKMT_CLOSEADAPTER ca{st.kmtAdapter};
        reinterpret_cast<FnD3DKMTCloseAdapter>(fnKmtCloseAdapter_)(&ca);
    }
    st.kmtAdapter = 0;
}

void MetricsCollector::PruneStaleEntries() {
    auto isStale = [&](uint32_t lastSeen) { return tickCounter_ - lastSeen > kStaleAfterTicks; };

    for (auto it = netState_.begin(); it != netState_.end();) {
        it = isStale(it->second.lastSeenTick) ? netState_.erase(it) : std::next(it);
    }
    for (auto it = diskHistories_.begin(); it != diskHistories_.end();) {
        it = isStale(it->second.lastSeenTick) ? diskHistories_.erase(it) : std::next(it);
    }
    for (auto it = gpuState_.begin(); it != gpuState_.end();) {
        if (isStale(it->second.lastSeenTick)) {
            CloseGpuAdapter(it->second);
            it = gpuState_.erase(it);
        } else {
            ++it;
        }
    }
}

void MetricsCollector::CollectCpu(CpuMetrics& out, bool recordHistory) {
    out.logicalCores = logicalCores_;

    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const uint64_t i = FileTimeToU64(idle);
        const uint64_t k = FileTimeToU64(kernel);
        const uint64_t u = FileTimeToU64(user);
        if (hasPrevCpuTimes_) {
            const uint64_t dIdle = i - prevIdleTime_;
            const uint64_t dKernel = k - prevKernelTime_;  // tempo de kernel inclui o ocioso
            const uint64_t dUser = u - prevUserTime_;
            const uint64_t dTotal = dKernel + dUser;
            if (dTotal > 0 && dTotal >= dIdle) {
                out.utilizationPct = std::clamp(
                    (static_cast<double>(dTotal - dIdle) / static_cast<double>(dTotal)) * 100.0,
                    0.0, 100.0);
            }
        }
        prevIdleTime_ = i;
        prevKernelTime_ = k;
        prevUserTime_ = u;
        hasPrevCpuTimes_ = true;
    }

    bool gotFreq = false;
    if (pdhCpuFreq_ && pdhCpuPerf_) {
        PDH_FMT_COUNTERVALUE vFreq{};
        PDH_FMT_COUNTERVALUE vPerf{};
        // NOCAP100: % Processor Performance passa de 100 com turbo.
        if (PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(pdhCpuFreq_), PDH_FMT_DOUBLE, nullptr, &vFreq) == ERROR_SUCCESS &&
            PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(pdhCpuPerf_), PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &vPerf) == ERROR_SUCCESS &&
            IsValidPdhStatus(vFreq.CStatus) && IsValidPdhStatus(vPerf.CStatus) &&
            std::isfinite(vFreq.doubleValue) && std::isfinite(vPerf.doubleValue) &&
            vFreq.doubleValue > 0.0 && vPerf.doubleValue > 0.0) {
            out.speedGhz = (vFreq.doubleValue / 1000.0) * (vPerf.doubleValue / 100.0);
            gotFreq = true;
        }
    }

    if (!gotFreq) {
        std::vector<LocalProcessorPowerInfo> pinfo(logicalCores_);
        const ULONG sz = static_cast<ULONG>(pinfo.size() * sizeof(LocalProcessorPowerInfo));
        if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, pinfo.data(), sz) == 0) {
            // Com mais de 64 threads a API pode preencher so o grupo atual: media apenas
            // das entradas preenchidas, senao as zeradas puxariam a frequencia para baixo.
            double sumCur = 0.0;
            size_t filled = 0;
            for (const auto& p : pinfo) {
                if (p.CurrentMhz > 0) {
                    sumCur += static_cast<double>(p.CurrentMhz);
                    ++filled;
                }
            }
            if (filled > 0) {
                out.speedGhz = (sumCur / static_cast<double>(filled)) / 1000.0;
            }
        }
    }

    if (recordHistory) {
        cpuHistory_.Push(static_cast<float>(out.utilizationPct));
    }
    out.history = cpuHistory_;
}

void MetricsCollector::CollectRam(RamMetrics& out, bool recordHistory) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
        out.totalGb = static_cast<double>(ms.ullTotalPhys) / kGiB;
        out.usedGb = static_cast<double>(ms.ullTotalPhys - ms.ullAvailPhys) / kGiB;
        out.usedPct = (out.totalGb > 0.0)
            ? std::clamp((out.usedGb / out.totalGb) * 100.0, 0.0, 100.0)
            : static_cast<double>(ms.dwMemoryLoad);
    }
    if (recordHistory) {
        ramHistory_.Push(static_cast<float>(out.usedPct));
    }
    out.history = ramHistory_;
}

void MetricsCollector::CollectDisks(SystemSnapshot& out, bool recordHistory) {
    struct DiskSampleAcc {
        double activePct = 0.0;
        double readMBs = 0.0;
        double writeMBs = 0.0;
        std::wstring pdhLetters;
    };
    std::map<int, DiskSampleAcc> acc;

    if (recordHistory) {
        ReadPdhArray(pdhDiskIdle_, PDH_FMT_DOUBLE, [&](std::wstring_view inst, double idleVal) {
            const int idx = ParseLeadingDiskIndex(inst);
            if (idx < 0) return;
            acc[idx].activePct = std::clamp(100.0 - idleVal, 0.0, 100.0);
            const size_t sp = inst.find(L' ');
            if (sp != std::wstring_view::npos && sp + 1 < inst.size()) {
                acc[idx].pdhLetters.assign(inst.substr(sp + 1));
            }
        });
        ReadPdhArray(pdhDiskRead_, PDH_FMT_DOUBLE, [&](std::wstring_view name, double bytesSec) {
            const int idx = ParseLeadingDiskIndex(name);
            if (idx >= 0) acc[idx].readMBs = std::max(0.0, bytesSec / (1024.0 * 1024.0));
        });
        ReadPdhArray(pdhDiskWrite_, PDH_FMT_DOUBLE, [&](std::wstring_view name, double bytesSec) {
            const int idx = ParseLeadingDiskIndex(name);
            if (idx >= 0) acc[idx].writeMBs = std::max(0.0, bytesSec / (1024.0 * 1024.0));
        });
    }

    // Disco que o PDH conhece mas o cache nao: confirma se ainda existe antes de cachear
    // (evita re-inserir disco recem-removido por amostra residual do PDH).
    for (const auto& [idx, _] : acc) {
        if (idx >= 0 && idx < kMaxPhysicalDrives && diskStaticCache_.find(idx) == diskStaticCache_.end()) {
            wchar_t devPath[64]{};
            swprintf_s(devPath, L"\\\\.\\PhysicalDrive%d", idx);
            HANDLE h = OpenDeviceNoAccess(devPath);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                diskStaticCache_[idx].mediaType = QueryDiskMediaType(idx);
            }
        }
    }

    std::set<int> allIndices;
    for (const auto& [idx, _] : diskStaticCache_) allIndices.insert(idx);
    for (const auto& [idx, _] : acc) allIndices.insert(idx);

    for (int idx : allIndices) {
        DiskMetrics dm;
        dm.index = idx;
        auto staticIt = diskStaticCache_.find(idx);
        if (staticIt != diskStaticCache_.end()) {
            dm.driveLetters = staticIt->second.driveLetters;
            dm.mediaType = staticIt->second.mediaType;
        }
        if (dm.mediaType.empty()) {
            dm.mediaType = kGenericDiskType;
        }

        auto accIt = acc.find(idx);
        if (accIt != acc.end()) {
            dm.activePct = accIt->second.activePct;
            dm.readMBs = accIt->second.readMBs;
            dm.writeMBs = accIt->second.writeMBs;
            if (dm.driveLetters.empty() && !accIt->second.pdhLetters.empty()) {
                dm.driveLetters = accIt->second.pdhLetters;
            }
        }

        dm.instanceName = L"Disco " + std::to_wstring(idx);
        if (!dm.driveLetters.empty()) {
            dm.instanceName += L" (" + dm.driveLetters + L")";
        }

        DiskHistory& hist = diskHistories_[idx];
        hist.lastSeenTick = tickCounter_;
        if (recordHistory) {
            hist.history.Push(static_cast<float>(dm.activePct));
        }
        dm.history = hist.history;
        out.disks.push_back(std::move(dm));
    }
}

void MetricsCollector::CollectNetworks(SystemSnapshot& out, uint64_t nowUs, bool recordHistory) {
    constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (adapterAddrBuffer_.empty()) {
        adapterAddrBuffer_.resize((16 * 1024) / sizeof(uint64_t));
    }
    ULONG ret = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && ret == ERROR_BUFFER_OVERFLOW; ++attempt) {
        ULONG bufLen = static_cast<ULONG>(adapterAddrBuffer_.size() * sizeof(uint64_t));
        ret = GetAdaptersAddresses(
            AF_UNSPEC, kFlags, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapterAddrBuffer_.data()), &bufLen);
        if (ret == ERROR_BUFFER_OVERFLOW) {
            adapterAddrBuffer_.resize((bufLen + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        }
    }
    if (ret != ERROR_SUCCESS) {
        return;
    }

    std::vector<NetMetrics> collected;
    for (auto* aa = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(adapterAddrBuffer_.data()); aa != nullptr; aa = aa->Next) {
        if (aa->OperStatus != IfOperStatusUp) continue;
        if (aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK || aa->IfType == IF_TYPE_TUNNEL) continue;
        if (!aa->FirstUnicastAddress) continue;

        std::wstring friendly = aa->FriendlyName ? aa->FriendlyName : L"";
        std::wstring desc = aa->Description ? aa->Description : L"";

        if (ContainsCaseInsensitive(desc, L"WAN Miniport") ||
            ContainsCaseInsensitive(desc, L"Kernel Debug") ||
            ContainsCaseInsensitive(friendly, L"vEthernet (WSL")) {
            continue;
        }

        MIB_IF_ROW2 row{};
        row.InterfaceLuid = aa->Luid;
        if (GetIfEntry2(&row) != NO_ERROR) {
            continue;
        }
        if (row.InterfaceAndOperStatusFlags.FilterInterface) {
            continue;
        }

        const bool isPhysical = (row.InterfaceAndOperStatusFlags.ConnectorPresent != FALSE) &&
                                !ContainsCaseInsensitive(desc, L"Virtual") &&
                                !ContainsCaseInsensitive(desc, L"VPN");

        const uint64_t luidVal = row.InterfaceLuid.Value;
        NetPrevSample& prev = netState_[luidVal];
        prev.lastSeenTick = tickCounter_;

        double sendBps = 0.0;
        double recvBps = 0.0;
        if (prev.tickUs > 0 && nowUs > prev.tickUs) {
            const double dt = static_cast<double>(nowUs - prev.tickUs) / 1000000.0;
            if (dt > 0.05 && row.OutOctets >= prev.outOctets && row.InOctets >= prev.inOctets) {
                sendBps = static_cast<double>(row.OutOctets - prev.outOctets) * 8.0 / dt;
                recvBps = static_cast<double>(row.InOctets - prev.inOctets) * 8.0 / dt;
            }
        }
        prev.inOctets = row.InOctets;
        prev.outOctets = row.OutOctets;
        prev.tickUs = nowUs;

        if (recordHistory) {
            prev.sendHistory.Push(static_cast<float>(sendBps));
            prev.recvHistory.Push(static_cast<float>(recvBps));
        }
        prev.maxScaleBps = SnapNiceScaleBps(
            std::max<double>(prev.sendHistory.MaxValue(), prev.recvHistory.MaxValue()));

        NetMetrics nm;
        nm.luid = luidVal;
        nm.name = friendly.empty() ? desc : friendly;
        nm.description = desc;
        nm.isPhysical = isPhysical;
        nm.sendBps = sendBps;
        nm.recvBps = recvBps;
        nm.maxScaleBps = prev.maxScaleBps;
        nm.sendHistory = prev.sendHistory;
        nm.recvHistory = prev.recvHistory;
        collected.push_back(std::move(nm));
    }

    std::stable_sort(collected.begin(), collected.end(), [](const NetMetrics& a, const NetMetrics& b) {
        if (a.isPhysical != b.isPhysical) return a.isPhysical > b.isPhysical;
        return a.name < b.name;
    });
    out.nets = std::move(collected);
}

void MetricsCollector::CollectGpus(SystemSnapshot& out, bool recordHistory) {
    // 1. Uso por engine e VRAM dedicada via PDH (mesma metodologia do Gerenciador de Tarefas:
    //    soma dos processos por engine, e o uso da GPU e o maior engine).
    std::unordered_map<LuidKey, std::unordered_map<std::wstring, double>, LuidKeyHash> gpuEngineTotals;
    std::unordered_map<LuidKey, double, LuidKeyHash> gpuDedicatedMemBytes;

    if (recordHistory) {
        ReadPdhArray(pdhGpuEngine_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, [&](std::wstring_view name, double val) {
            uint32_t low = 0;
            int32_t high = 0;
            std::wstring_view engine;
            if (ParseGpuLuid(name, low, high, &engine) && !engine.empty()) {
                gpuEngineTotals[{low, high}][std::wstring(engine)] += std::max(0.0, val);
            }
        });
    }
    ReadPdhArray(pdhGpuMem_, PDH_FMT_DOUBLE, [&](std::wstring_view name, double val) {
        uint32_t low = 0;
        int32_t high = 0;
        if (ParseGpuLuid(name, low, high)) {
            gpuDedicatedMemBytes[{low, high}] += std::max(0.0, val);
        }
    });

    // 2. Adaptadores via DXGI (fabrica reaproveitada enquanto continuar valida)
    if (dxgiFactory_ && !dxgiFactory_->IsCurrent()) {
        dxgiFactory_->Release();
        dxgiFactory_ = nullptr;
    }
    if (!dxgiFactory_) {
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&dxgiFactory_)))) {
            dxgiFactory_ = nullptr;
            return;
        }
    }

    const auto fnOpen = reinterpret_cast<FnD3DKMTOpenAdapterFromLuid>(fnKmtOpenFromLuid_);
    const auto fnQuery = reinterpret_cast<FnD3DKMTQueryAdapterInfo>(fnKmtQueryAdapterInfo_);
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

    std::vector<LuidKey> seenLuids;
    std::vector<std::array<uint32_t, 4>> seenPciIds;  // para numerar placas identicas
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT idx = 0; dxgiFactory_->EnumAdapters1(idx, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++idx) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            continue;
        }
        const LuidKey lk{desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart};
        if (std::find(seenLuids.begin(), seenLuids.end(), lk) != seenLuids.end()) {
            continue;
        }
        seenLuids.push_back(lk);

        GpuState& st = gpuState_[lk];
        st.lastSeenTick = tickCounter_;

        GpuMetrics gm;
        gm.luid = desc.AdapterLuid;
        gm.index = static_cast<int>(out.gpus.size());
        gm.name = desc.Description;
        gm.vendorId = desc.VendorId;
        gm.deviceId = desc.DeviceId;
        gm.subSysId = desc.SubSysId;
        gm.revision = desc.Revision;
        const std::array<uint32_t, 4> pciId{desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision};
        gm.occurrence = static_cast<uint32_t>(std::count(seenPciIds.begin(), seenPciIds.end(), pciId));
        seenPciIds.push_back(pciId);
        gm.vramTotalGb = static_cast<double>(desc.DedicatedVideoMemory) / kGiB;

        // Sem o contador PDH a VRAM em uso do sistema e desconhecida (QueryVideoMemoryInfo
        // so informa o uso deste processo, por isso nao serve de fallback).
        auto memIt = gpuDedicatedMemBytes.find(lk);
        gm.vramUsedGb = (memIt != gpuDedicatedMemBytes.end()) ? memIt->second / kGiB : -1.0;

        auto engIt = gpuEngineTotals.find(lk);
        if (engIt != gpuEngineTotals.end()) {
            double maxEng = 0.0;
            for (const auto& [_, val] : engIt->second) {
                maxEng = std::max(maxEng, val);
            }
            gm.utilizationPct = std::clamp(maxEng, 0.0, 100.0);
        }

        // 3. Temperatura via D3DKMT (handle aberto uma vez por adaptador)
        bool freshlyOpened = false;
        if (fnOpen && fnQuery && !st.kmtOpenTried &&
            static_cast<int32_t>(tickCounter_ - st.kmtRetryAtTick) >= 0) {
            st.kmtOpenTried = true;
            LocalD3DKMT_OPENADAPTERFROMLUID oa{};
            oa.AdapterLuid = desc.AdapterLuid;
            if (fnOpen(&oa) == 0) {
                st.kmtAdapter = oa.hAdapter;
                freshlyOpened = true;
            }
        }
        if (st.kmtAdapter != 0 && fnQuery) {
            LocalD3DKMT_ADAPTER_PERFDATA pd{};
            LocalD3DKMT_QUERYADAPTERINFO qi{};
            qi.hAdapter = st.kmtAdapter;
            qi.Type = kKmtQaiTypeAdapterPerfData;
            qi.pPrivateDriverData = &pd;
            qi.PrivateDriverDataSize = sizeof(pd);
            if (fnQuery(&qi) == 0) {
                const double tempC = static_cast<double>(pd.Temperature) / 10.0;
                if (tempC > 0.0 && tempC < 150.0) {
                    gm.tempC = tempC;
                }
            } else {
                CloseGpuAdapter(st);
                st.kmtOpenTried = false;
                // Falhou com o handle recem-aberto: o driver nao suporta a consulta, entao
                // espera ~5 min. Falhou num handle antigo: reset de driver/TDR, reabre ja.
                st.kmtRetryAtTick = freshlyOpened ? tickCounter_ + kKmtRetryBackoffTicks : tickCounter_;
            }
        }

        if (recordHistory) {
            st.history.Push(static_cast<float>(gm.utilizationPct));
        }
        gm.history = st.history;
        out.gpus.push_back(std::move(gm));
    }
}

SystemSnapshot MetricsCollector::Sample(bool recordHistory) {
    if (recordHistory) {
        ++tickCounter_;
        if (tickCounter_ % kPdhRetryEveryTicks == 0 &&
            (!pdhCpuPerf_ || !pdhCpuFreq_ || !pdhDiskIdle_ || !pdhDiskRead_ || !pdhDiskWrite_ ||
             !pdhGpuEngine_ || !pdhGpuMem_)) {
            AddMissingPdhCounters();
        }
        if (diskRefreshRequested_ || tickCounter_ % kDiskFallbackRefreshTicks == 0) {
            const bool deviceChanged = diskRefreshRequested_;
            diskRefreshRequested_ = false;
            RefreshDiskStaticCache(deviceChanged);
        }
        if (pdhQuery_) {
            PdhCollectQueryData(static_cast<PDH_HQUERY>(pdhQuery_));
        }
    }

    const uint64_t nowUs = GetTickMicroseconds();
    SystemSnapshot snap;
    CollectCpu(snap.cpu, recordHistory);
    CollectRam(snap.ram, recordHistory);
    CollectDisks(snap, recordHistory);
    CollectNetworks(snap, nowUs, recordHistory);
    CollectGpus(snap, recordHistory);

    if (recordHistory && tickCounter_ % kPruneEveryTicks == 0) {
        PruneStaleEntries();
    }
    return snap;
}
