#pragma once

#include "system_metrics.h"

#include <windows.h>
#include <pdh.h>

#include <string>
#include <vector>

namespace minimal_taskbar_monitor {

struct ProcessPopupItem {
    DWORD pid{0};
    std::wstring name;
    double cpu_percent{0.0};
    unsigned long long rss_bytes{0};
    unsigned long long uss_bytes{0};
    unsigned long long vms_bytes{0};
    double gpu_percent{0.0};
    unsigned long long vram_bytes{0};
    unsigned long long io_read_bytes_per_second{0};
    unsigned long long io_write_bytes_per_second{0};
    unsigned long long network_bytes_per_second{0};
    double score{0.0};
};

struct ProcessPopupSnapshot {
    std::vector<ProcessPopupItem> top_processes;
    int total_process_count{0};
    ULONGLONG uptime_ms{0};
    bool network_metric_available{false};
    bool network_is_estimated{false};
};

class ProcessMonitor {
public:
    ProcessMonitor();
    ~ProcessMonitor();

    ProcessPopupSnapshot Sample(const MetricsSnapshot& system_snapshot, size_t max_count = 10);

private:
    enum class ProcessObjectMode {
        kNone,
        kProcessV2,
        kProcess
    };

    struct RawProcessSample {
        DWORD pid{0};
        std::wstring instance_name;
        std::wstring fallback_name;
        double cpu_percent{0.0};
        unsigned long long working_set_bytes{0};
        unsigned long long private_working_set_bytes{0};
        unsigned long long private_bytes{0};
        unsigned long long vms_bytes{0};
        unsigned long long io_read_bytes_per_second{0};
        unsigned long long io_write_bytes_per_second{0};
        unsigned long long network_bytes_per_second{0};
        double gpu_percent{0.0};
        unsigned long long gpu_dedicated_bytes{0};
        unsigned long long gpu_shared_bytes{0};
        double score{0.0};
    };

    bool InitializeProcessQuery();
    bool InitializeGpuQuery();
    std::vector<RawProcessSample> CollectProcessSamples();
    void ApplyCompositeScores(std::vector<RawProcessSample>& samples,
                              const MetricsSnapshot& system_snapshot) const;
    void RefreshProcessMemoryMetrics(std::vector<RawProcessSample>& samples) const;
    std::wstring ResolveDisplayName(DWORD pid, const std::wstring& fallback_name);
    void ResolveDisplayNames(std::vector<ProcessPopupItem>& items);

    PDH_HQUERY process_query_{nullptr};
    PDH_HCOUNTER process_cpu_counter_{nullptr};
    PDH_HCOUNTER process_working_set_counter_{nullptr};
    PDH_HCOUNTER process_private_working_set_counter_{nullptr};
    PDH_HCOUNTER process_private_bytes_counter_{nullptr};
    PDH_HCOUNTER process_io_read_counter_{nullptr};
    PDH_HCOUNTER process_io_write_counter_{nullptr};
    PDH_HCOUNTER process_io_other_counter_{nullptr};
    PDH_HCOUNTER process_id_counter_{nullptr};
    bool process_query_initialized_{false};
    bool has_network_proxy_counter_{false};
    ProcessObjectMode process_object_mode_{ProcessObjectMode::kNone};

    PDH_HQUERY gpu_query_{nullptr};
    PDH_HCOUNTER gpu_engine_counter_{nullptr};
    PDH_HCOUNTER gpu_memory_dedicated_counter_{nullptr};
    PDH_HCOUNTER gpu_memory_shared_counter_{nullptr};
    bool gpu_query_initialized_{false};
    bool gpu_memory_counters_available_{false};

    DWORD logical_processor_count_{1};
    unsigned long long total_physical_memory_bytes_{0};
    std::vector<std::pair<DWORD, std::wstring>> name_cache_;
};

}  // namespace minimal_taskbar_monitor
