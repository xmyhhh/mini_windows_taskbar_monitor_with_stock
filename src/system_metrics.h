#pragma once

#include <pdh.h>
#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace minimal_taskbar_monitor {

enum class NetworkDisplayUnit {
    kBitsPerSecond,
    kBytesPerSecond,
};

struct MetricsSnapshot {
    int cpu_percent{-1};
    int memory_percent{-1};
    int gpu_percent{-1};
    unsigned long long upload_bytes_per_second{0};
    unsigned long long download_bytes_per_second{0};
    unsigned long long disk_read_bytes_per_second{0};
    unsigned long long disk_write_bytes_per_second{0};
};

struct MetricVisibility {
    bool show_cpu{true};
    bool show_memory{true};
    bool show_upload{true};
    bool show_download{true};
    bool show_gpu{true};
    bool show_disk_read{true};
    bool show_disk_write{true};
};

struct DisplayLines {
    std::wstring line1;
    std::wstring line2;
    struct Column {
        std::wstring top_text;
        std::wstring bottom_text;
    };
    std::vector<Column> columns;
};

class SystemMetrics {
public:
    SystemMetrics();
    ~SystemMetrics();

    MetricsSnapshot Sample();

private:
    struct NetworkInterfaceTotals {
        unsigned long long in_bytes{0};
        unsigned long long out_bytes{0};
    };
    using NetworkInterfaceCounters = std::map<unsigned long long, NetworkInterfaceTotals>;

    int SampleCpuPercent();
    int SampleCpuPercentWithSystemTimesFallback();
    int SampleMemoryPercent() const;
    int SampleGpuPercent();
    void SampleNetwork(unsigned long long& download_bytes_per_second,
                       unsigned long long& upload_bytes_per_second);
    void SampleDisk(unsigned long long& read_bytes_per_second,
                    unsigned long long& write_bytes_per_second);

    bool QueryNetworkInterfaces(NetworkInterfaceCounters& interfaces) const;
    bool InitializeCounterQuery(PDH_HQUERY& query_handle,
                                bool& initialized,
                                const wchar_t* primary_counter_path,
                                PDH_HCOUNTER& primary_counter,
                                const wchar_t* secondary_counter_path = nullptr,
                                PDH_HCOUNTER* secondary_counter = nullptr);
    bool AddCounterWithFallback(PDH_HQUERY query_handle,
                                const wchar_t* counter_path,
                                PDH_HCOUNTER& counter_handle);
    bool QueryDoubleCounter(PDH_HCOUNTER counter_handle, double& value) const;

    bool cpu_initialized_{false};
    ULARGE_INTEGER last_idle_time_{};
    ULARGE_INTEGER last_kernel_time_{};
    ULARGE_INTEGER last_user_time_{};

    bool network_initialized_{false};
    NetworkInterfaceCounters last_network_interfaces_{};
    ULONGLONG last_network_tick_{0};

    PDH_HQUERY cpu_query_{nullptr};
    PDH_HCOUNTER cpu_counter_{nullptr};
    bool cpu_query_initialized_{false};

    PDH_HQUERY gpu_query_{nullptr};
    PDH_HCOUNTER gpu_counter_{nullptr};
    bool gpu_query_initialized_{false};

    PDH_HQUERY disk_query_{nullptr};
    PDH_HCOUNTER disk_read_counter_{nullptr};
    PDH_HCOUNTER disk_write_counter_{nullptr};
    bool disk_query_initialized_{false};
};

DisplayLines FormatMetricsLines(const MetricsSnapshot& snapshot,
                                const MetricVisibility& visibility,
                                NetworkDisplayUnit network_display_unit,
                                bool use_chinese_labels = false);
DisplayLines GetMetricsSampleLines(const MetricVisibility& visibility,
                                   NetworkDisplayUnit network_display_unit,
                                   bool use_chinese_labels = false);
std::wstring FormatNetworkRateForDisplay(unsigned long long bytes_per_second,
                                         NetworkDisplayUnit network_display_unit);
bool IsLightTaskbarTheme();

}  // namespace minimal_taskbar_monitor
