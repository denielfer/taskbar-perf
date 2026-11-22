#pragma once

#include <windows.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

template <size_t N>
class RingBuffer {
public:
    void Push(float value) {
        data_[head_] = value;
        head_ = (head_ + 1) % N;
        if (count_ < N) {
            ++count_;
        }
    }

    size_t Count() const { return count_; }
    static constexpr size_t Capacity() { return N; }

    float AtOldest(size_t index) const {
        if (index >= count_) {
            return 0.0f;
        }
        const size_t start = (count_ == N) ? head_ : 0;
        return data_[(start + index) % N];
    }

    float Latest() const {
        if (count_ == 0) {
            return 0.0f;
        }
        return data_[(head_ + N - 1) % N];
    }

    float MaxValue() const {
        float m = 0.0f;
        for (size_t i = 0; i < count_; ++i) {
            m = std::max(m, AtOldest(i));
        }
        return m;
    }

private:
    std::array<float, N> data_{};
    size_t head_ = 0;
    size_t count_ = 0;
};

struct CpuMetrics {
    double utilizationPct = 0.0;
    double speedGhz = 0.0;
    uint32_t logicalCores = 0;
    RingBuffer<60> history;
};

struct RamMetrics {
    double usedGb = 0.0;
    double totalGb = 0.0;
    double usedPct = 0.0;
    RingBuffer<60> history;
};

struct DiskMetrics {
    int index = 0;  // numero N de \\.\PhysicalDriveN (igual ao indice do PDH)
    std::wstring instanceName;
    std::wstring driveLetters;
    std::wstring mediaType;
    double activePct = 0.0;
    double readMBs = 0.0;
    double writeMBs = 0.0;
    RingBuffer<60> history;
};

struct NetMetrics {
    uint64_t luid = 0;  // NET_LUID: estavel entre reinicializacoes
    std::wstring name;
    std::wstring description;
    bool isPhysical = false;
    double sendBps = 0.0;
    double recvBps = 0.0;
    double maxScaleBps = 100000.0;
    RingBuffer<60> sendHistory;
    RingBuffer<60> recvHistory;
};

struct GpuMetrics {
    LUID luid{};    // so vale ate o proximo boot/reset de driver: nunca persistir
    int index = 0;  // ordem de enumeracao DXGI (so para rotulos)
    // Identidade estavel (PCI) para persistir o pin; occurrence separa placas identicas.
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t subSysId = 0;
    uint32_t revision = 0;
    uint32_t occurrence = 0;
    std::wstring name;
    double utilizationPct = 0.0;
    double tempC = -1.0;
    double vramUsedGb = -1.0;  // < 0 = desconhecido (contador PDH indisponivel)
    double vramTotalGb = 0.0;
    RingBuffer<60> history;
};

struct SystemSnapshot {
    CpuMetrics cpu;
    RamMetrics ram;
    std::vector<DiskMetrics> disks;
    std::vector<NetMetrics> nets;
    std::vector<GpuMetrics> gpus;
};

// Identificadores de "pin" (persistidos no config.ini). Rede usa o NET_LUID (persistente
// entre reinicializacoes); GPU usa o ID PCI, porque o LUID do adaptador muda a cada boot.
inline std::wstring PinIdForDisk(const DiskMetrics& d) {
    return L"disk:" + std::to_wstring(d.index);
}

inline std::wstring PinIdForNet(const NetMetrics& n) {
    wchar_t buf[40]{};
    swprintf_s(buf, L"net:luid:%016llX", static_cast<unsigned long long>(n.luid));
    return buf;
}

inline std::wstring PinIdForGpu(const GpuMetrics& g) {
    wchar_t buf[64]{};
    swprintf_s(buf, L"gpu:pci:%04X-%04X-%08X-%02X-%u",
        g.vendorId, g.deviceId, g.subSysId, g.revision, g.occurrence);
    return buf;
}

// Formato intermediario (gpu:luid:...) gravado por uma versao anterior; so serve para migracao.
inline std::wstring LegacyLuidPinIdForGpu(const GpuMetrics& g) {
    const uint64_t v = (static_cast<uint64_t>(static_cast<uint32_t>(g.luid.HighPart)) << 32) | g.luid.LowPart;
    wchar_t buf[40]{};
    swprintf_s(buf, L"gpu:luid:%016llX", static_cast<unsigned long long>(v));
    return buf;
}
