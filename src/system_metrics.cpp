#include "system_metrics.h"

#include <ws2def.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winreg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace minimal_taskbar_monitor {

namespace {

constexpr wchar_t kCpuUtilityCounterPath[] = L"\\Processor Information(_Total)\\% Processor Utility";
constexpr wchar_t kCpuTimeCounterPath[] = L"\\Processor(_Total)\\% Processor Time";
constexpr wchar_t kGpuCounterPath[] = L"\\GPU Engine(*)\\Utilization Percentage";
constexpr wchar_t kDiskReadCounterPath[] = L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec";
constexpr wchar_t kDiskWriteCounterPath[] = L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec";
constexpr wchar_t kThemeRegistryPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kThemeRegistryValue[] = L"SystemUsesLightTheme";

unsigned long long ToUnsignedLongLong(const FILETIME& value) {
    ULARGE_INTEGER combined{};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}

std::wstring FormatByteRate(unsigned long long bytes_per_second) {
    double value = static_cast<double>(bytes_per_second);
    const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s"};
    size_t unit_index = 0;

    while (value >= 1024.0 && unit_index + 1 < _countof(units)) {
        value /= 1024.0;
        ++unit_index;
    }

    wchar_t buffer[64]{};
    if (value >= 100.0 || unit_index == 0) {
        swprintf_s(buffer, L"%.0f%ls", value, units[unit_index]);
    } else {
        swprintf_s(buffer, L"%.1f%ls", value, units[unit_index]);
    }

    return buffer;
}

std::wstring FormatBitsPerSecondRate(unsigned long long bytes_per_second) {
    double value = static_cast<double>(bytes_per_second) * 8.0;
    const wchar_t* units[] = {L"bps", L"Kbps", L"Mbps", L"Gbps"};
    size_t unit_index = 0;

    while (value >= 1000.0 && unit_index + 1 < _countof(units)) {
        value /= 1000.0;
        ++unit_index;
    }

    wchar_t buffer[64]{};
    if (value >= 100.0 || unit_index == 0) {
        swprintf_s(buffer, L"%.0f%ls", value, units[unit_index]);
    } else {
        swprintf_s(buffer, L"%.1f%ls", value, units[unit_index]);
    }

    return buffer;
}

std::wstring FormatBytesPerSecondRate(unsigned long long bytes_per_second) {
    double value = static_cast<double>(bytes_per_second);
    const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s"};
    size_t unit_index = 0;

    while (value >= 1024.0 && unit_index + 1 < _countof(units)) {
        value /= 1024.0;
        ++unit_index;
    }

    wchar_t buffer[64]{};
    if (value >= 100.0 || unit_index == 0) {
        swprintf_s(buffer, L"%.0f%ls", value, units[unit_index]);
    } else {
        swprintf_s(buffer, L"%.1f%ls", value, units[unit_index]);
    }

    return buffer;
}

bool QueryThemeValue(DWORD& value) {
    DWORD value_size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,
                        kThemeRegistryPath,
                        kThemeRegistryValue,
                        RRF_RT_REG_DWORD,
                        nullptr,
                        &value,
                        &value_size) == ERROR_SUCCESS;
}

std::wstring JoinSegments(const std::vector<std::wstring>& segments);

void AddColumn(std::vector<DisplayLines::Column>& columns,
               std::wstring top_text,
               std::wstring bottom_text = L"") {
    if (top_text.empty() && bottom_text.empty()) {
        return;
    }

    columns.push_back({std::move(top_text), std::move(bottom_text)});
}

void AddOptionalPairColumn(std::vector<DisplayLines::Column>& columns,
                           bool top_visible,
                           std::wstring top_text,
                           bool bottom_visible,
                           std::wstring bottom_text) {
    if (top_visible && bottom_visible) {
        AddColumn(columns, std::move(top_text), std::move(bottom_text));
    } else if (top_visible) {
        AddColumn(columns, std::move(top_text));
    } else if (bottom_visible) {
        AddColumn(columns, std::move(bottom_text));
    }
}

std::wstring JoinColumnTexts(const std::vector<DisplayLines::Column>& columns, bool top_line) {
    std::vector<std::wstring> segments;
    segments.reserve(columns.size());

    for (const auto& column : columns) {
        const std::wstring& text = top_line ? column.top_text : column.bottom_text;
        if (!text.empty()) {
            segments.push_back(text);
        }
    }

    return JoinSegments(segments);
}

void NormalizeDisplayColumns(DisplayLines& lines) {
    bool has_top_text = false;
    bool has_bottom_text = false;
    for (const auto& column : lines.columns) {
        has_top_text = has_top_text || !column.top_text.empty();
        has_bottom_text = has_bottom_text || !column.bottom_text.empty();
    }

    if (!has_top_text && has_bottom_text) {
        for (auto& column : lines.columns) {
            if (column.top_text.empty()) {
                column.top_text = std::move(column.bottom_text);
            } else {
                column.bottom_text.clear();
            }
        }
    }
}

std::wstring JoinSegments(const std::vector<std::wstring>& segments) {
    std::wstring result;
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index != 0) {
            result += L"  ";
        }
        result += segments[index];
    }
    return result;
}

void NormalizeDisplayLines(DisplayLines& lines, bool) {
    NormalizeDisplayColumns(lines);
    lines.line1 = JoinColumnTexts(lines.columns, true);
    lines.line2 = JoinColumnTexts(lines.columns, false);

    if (lines.line1.empty() && !lines.line2.empty()) {
        lines.line1 = lines.line2;
        lines.line2.clear();
    }
    if (lines.line1.empty()) {
        lines.line1 = L"No metrics";
        lines.columns = {{lines.line1, L""}};
    }
}

void CloseQueryIfNeeded(PDH_HQUERY& query_handle) {
    if (query_handle != nullptr) {
        PdhCloseQuery(query_handle);
        query_handle = nullptr;
    }
}

bool ShouldIncludeNetworkInterface(const MIB_IF_ROW2& row) {
    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.Type == IF_TYPE_TUNNEL) {
        return false;
    }
    return row.OperStatus == IfOperStatusUp;
}

bool IsHardwareNetworkInterface(const MIB_IF_ROW2& row) {
    return row.InterfaceAndOperStatusFlags.HardwareInterface != 0;
}

unsigned long long GetNetworkInterfaceKey(const MIB_IF_ROW2& row) {
    if (row.InterfaceLuid.Value != 0) {
        return row.InterfaceLuid.Value;
    }
    return row.InterfaceIndex;
}

}  // namespace

std::wstring FormatNetworkRateForDisplay(unsigned long long bytes_per_second,
                                         NetworkDisplayUnit network_display_unit) {
    switch (network_display_unit) {
    case NetworkDisplayUnit::kBytesPerSecond:
        return FormatBytesPerSecondRate(bytes_per_second);
    case NetworkDisplayUnit::kBitsPerSecond:
    default:
        return FormatBitsPerSecondRate(bytes_per_second);
    }
}

SystemMetrics::SystemMetrics() {
    InitializeCounterQuery(
        cpu_query_, cpu_query_initialized_, kCpuTimeCounterPath, cpu_counter_);
    if (!cpu_query_initialized_) {
        InitializeCounterQuery(
            cpu_query_, cpu_query_initialized_, kCpuUtilityCounterPath, cpu_counter_);
    }

    InitializeCounterQuery(
        gpu_query_, gpu_query_initialized_, kGpuCounterPath, gpu_counter_);

    InitializeCounterQuery(disk_query_,
                           disk_query_initialized_,
                           kDiskReadCounterPath,
                           disk_read_counter_,
                           kDiskWriteCounterPath,
                           &disk_write_counter_);
}

SystemMetrics::~SystemMetrics() {
    CloseQueryIfNeeded(cpu_query_);
    CloseQueryIfNeeded(gpu_query_);
    CloseQueryIfNeeded(disk_query_);
}

MetricsSnapshot SystemMetrics::Sample() {
    MetricsSnapshot snapshot{};
    snapshot.cpu_percent = SampleCpuPercent();
    snapshot.memory_percent = SampleMemoryPercent();
    snapshot.gpu_percent = SampleGpuPercent();
    SampleNetwork(snapshot.download_bytes_per_second, snapshot.upload_bytes_per_second);
    SampleDisk(snapshot.disk_read_bytes_per_second, snapshot.disk_write_bytes_per_second);
    return snapshot;
}

bool SystemMetrics::InitializeCounterQuery(PDH_HQUERY& query_handle,
                                           bool& initialized,
                                           const wchar_t* primary_counter_path,
                                           PDH_HCOUNTER& primary_counter,
                                           const wchar_t* secondary_counter_path,
                                           PDH_HCOUNTER* secondary_counter) {
    initialized = false;
    primary_counter = nullptr;
    if (secondary_counter != nullptr) {
        *secondary_counter = nullptr;
    }

    CloseQueryIfNeeded(query_handle);
    if (PdhOpenQueryW(nullptr, 0, &query_handle) != ERROR_SUCCESS) {
        return false;
    }

    if (!AddCounterWithFallback(query_handle, primary_counter_path, primary_counter)) {
        CloseQueryIfNeeded(query_handle);
        return false;
    }

    if (secondary_counter_path != nullptr && secondary_counter != nullptr &&
        !AddCounterWithFallback(query_handle, secondary_counter_path, *secondary_counter)) {
        CloseQueryIfNeeded(query_handle);
        primary_counter = nullptr;
        *secondary_counter = nullptr;
        return false;
    }

    if (PdhCollectQueryData(query_handle) != ERROR_SUCCESS) {
        CloseQueryIfNeeded(query_handle);
        primary_counter = nullptr;
        if (secondary_counter != nullptr) {
            *secondary_counter = nullptr;
        }
        return false;
    }

    initialized = true;
    return true;
}

bool SystemMetrics::AddCounterWithFallback(PDH_HQUERY query_handle,
                                           const wchar_t* counter_path,
                                           PDH_HCOUNTER& counter_handle) {
    counter_handle = nullptr;
    PDH_STATUS status = PdhAddCounterW(query_handle, counter_path, 0, &counter_handle);
    if (status != ERROR_SUCCESS) {
        status = PdhAddEnglishCounterW(query_handle, counter_path, 0, &counter_handle);
    }
    return status == ERROR_SUCCESS;
}

bool SystemMetrics::QueryDoubleCounter(PDH_HCOUNTER counter_handle, double& value) const {
    PDH_FMT_COUNTERVALUE formatted_value{};
    if (PdhGetFormattedCounterValue(counter_handle, PDH_FMT_DOUBLE, nullptr, &formatted_value) !=
            ERROR_SUCCESS ||
        (formatted_value.CStatus != ERROR_SUCCESS &&
         formatted_value.CStatus != PDH_CSTATUS_VALID_DATA &&
         formatted_value.CStatus != PDH_CSTATUS_NEW_DATA)) {
        value = 0.0;
        return false;
    }

    value = formatted_value.doubleValue;
    return true;
}

int SystemMetrics::SampleCpuPercent() {
    if (cpu_query_initialized_) {
        if (PdhCollectQueryData(cpu_query_) == ERROR_SUCCESS) {
            double value = 0.0;
            if (QueryDoubleCounter(cpu_counter_, value)) {
                const int percent = static_cast<int>(std::lround(value));
                return std::clamp(percent, 0, 100);
            }
        }
    }

    return SampleCpuPercentWithSystemTimesFallback();
}

int SystemMetrics::SampleCpuPercentWithSystemTimesFallback() {
    FILETIME idle_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        return -1;
    }

    const unsigned long long idle = ToUnsignedLongLong(idle_time);
    const unsigned long long kernel = ToUnsignedLongLong(kernel_time);
    const unsigned long long user = ToUnsignedLongLong(user_time);

    if (!cpu_initialized_) {
        last_idle_time_.QuadPart = idle;
        last_kernel_time_.QuadPart = kernel;
        last_user_time_.QuadPart = user;
        cpu_initialized_ = true;
        return 0;
    }

    const unsigned long long idle_delta = idle - last_idle_time_.QuadPart;
    const unsigned long long kernel_delta = kernel - last_kernel_time_.QuadPart;
    const unsigned long long user_delta = user - last_user_time_.QuadPart;
    const unsigned long long total_delta = kernel_delta + user_delta;

    last_idle_time_.QuadPart = idle;
    last_kernel_time_.QuadPart = kernel;
    last_user_time_.QuadPart = user;

    if (total_delta == 0) {
        return 0;
    }

    const double used_ratio =
        static_cast<double>(total_delta - idle_delta) / static_cast<double>(total_delta);
    const int percent = static_cast<int>(std::lround(used_ratio * 100.0));
    return std::clamp(percent, 0, 100);
}

int SystemMetrics::SampleMemoryPercent() const {
    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = sizeof(memory_status);
    if (!GlobalMemoryStatusEx(&memory_status)) {
        return -1;
    }
    return static_cast<int>(memory_status.dwMemoryLoad);
}

int SystemMetrics::SampleGpuPercent() {
    if (!gpu_query_initialized_) {
        return -1;
    }

    if (PdhCollectQueryData(gpu_query_) != ERROR_SUCCESS) {
        return -1;
    }

    DWORD buffer_size = 0;
    DWORD item_count = 0;
    PDH_STATUS status =
        PdhGetFormattedCounterArrayW(gpu_counter_, PDH_FMT_DOUBLE, &buffer_size, &item_count, nullptr);
    if (status != PDH_MORE_DATA || buffer_size == 0) {
        return -1;
    }

    std::vector<BYTE> buffer(buffer_size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(
        gpu_counter_, PDH_FMT_DOUBLE, &buffer_size, &item_count, items);
    if (status != ERROR_SUCCESS) {
        return -1;
    }

    std::map<std::wstring, double> usage_by_engine;
    for (DWORD i = 0; i < item_count; ++i) {
        if (items[i].FmtValue.CStatus != ERROR_SUCCESS &&
            items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
            items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) {
            continue;
        }

        std::wstring engine_name = items[i].szName ? items[i].szName : L"";
        const size_t suffix_pos = engine_name.rfind(L'_');
        if (suffix_pos != std::wstring::npos && suffix_pos + 1 < engine_name.size()) {
            engine_name = engine_name.substr(suffix_pos + 1);
        }
        usage_by_engine[engine_name] += items[i].FmtValue.doubleValue;
    }

    double max_usage = 0.0;
    for (const auto& [_, usage] : usage_by_engine) {
        max_usage = std::max(max_usage, usage);
    }

    return std::clamp(static_cast<int>(std::lround(max_usage)), 0, 100);
}

void SystemMetrics::SampleNetwork(unsigned long long& download_bytes_per_second,
                                  unsigned long long& upload_bytes_per_second) {
    NetworkInterfaceCounters current_interfaces;
    if (!QueryNetworkInterfaces(current_interfaces)) {
        download_bytes_per_second = 0;
        upload_bytes_per_second = 0;
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (!network_initialized_) {
        last_network_interfaces_ = std::move(current_interfaces);
        last_network_tick_ = now;
        network_initialized_ = true;
        download_bytes_per_second = 0;
        upload_bytes_per_second = 0;
        return;
    }

    const ULONGLONG elapsed_ms = now - last_network_tick_;
    if (elapsed_ms == 0) {
        download_bytes_per_second = 0;
        upload_bytes_per_second = 0;
        return;
    }

    unsigned long long in_delta = 0;
    unsigned long long out_delta = 0;
    for (const auto& [interface_key, current_totals] : current_interfaces) {
        const auto previous_iterator = last_network_interfaces_.find(interface_key);
        if (previous_iterator == last_network_interfaces_.end()) {
            continue;
        }

        const NetworkInterfaceTotals& previous_totals = previous_iterator->second;
        if (current_totals.in_bytes >= previous_totals.in_bytes) {
            in_delta += current_totals.in_bytes - previous_totals.in_bytes;
        }
        if (current_totals.out_bytes >= previous_totals.out_bytes) {
            out_delta += current_totals.out_bytes - previous_totals.out_bytes;
        }
    }

    download_bytes_per_second = (in_delta * 1000ULL) / elapsed_ms;
    upload_bytes_per_second = (out_delta * 1000ULL) / elapsed_ms;

    last_network_interfaces_ = std::move(current_interfaces);
    last_network_tick_ = now;
}

void SystemMetrics::SampleDisk(unsigned long long& read_bytes_per_second,
                               unsigned long long& write_bytes_per_second) {
    read_bytes_per_second = 0;
    write_bytes_per_second = 0;

    if (!disk_query_initialized_) {
        return;
    }

    if (PdhCollectQueryData(disk_query_) != ERROR_SUCCESS) {
        return;
    }

    double read_value = 0.0;
    double write_value = 0.0;
    if (QueryDoubleCounter(disk_read_counter_, read_value) && read_value > 0.0) {
        read_bytes_per_second = static_cast<unsigned long long>(std::llround(read_value));
    }
    if (QueryDoubleCounter(disk_write_counter_, write_value) && write_value > 0.0) {
        write_bytes_per_second = static_cast<unsigned long long>(std::llround(write_value));
    }
}

bool SystemMetrics::QueryNetworkInterfaces(NetworkInterfaceCounters& interfaces) const {
    interfaces.clear();

    MIB_IF_TABLE2* interface_table = nullptr;
    if (GetIfTable2(&interface_table) != NO_ERROR || interface_table == nullptr) {
        return false;
    }

    bool has_hardware_interface = false;
    for (ULONG i = 0; i < interface_table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = interface_table->Table[i];
        if (ShouldIncludeNetworkInterface(row) && IsHardwareNetworkInterface(row)) {
            has_hardware_interface = true;
            break;
        }
    }

    for (ULONG i = 0; i < interface_table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = interface_table->Table[i];
        if (!ShouldIncludeNetworkInterface(row) ||
            (has_hardware_interface && !IsHardwareNetworkInterface(row))) {
            continue;
        }
        NetworkInterfaceTotals& totals = interfaces[GetNetworkInterfaceKey(row)];
        totals.in_bytes += row.InOctets;
        totals.out_bytes += row.OutOctets;
    }

    FreeMibTable(interface_table);
    return true;
}

DisplayLines FormatMetricsLines(const MetricsSnapshot& snapshot,
                                const MetricVisibility& visibility,
                                NetworkDisplayUnit network_display_unit,
                                bool use_chinese_labels) {
    DisplayLines lines{};
    AddOptionalPairColumn(lines.columns,
                          visibility.show_cpu,
                          std::wstring(L"CPU ") +
                              std::to_wstring(std::max(snapshot.cpu_percent, 0)) + L"%",
                          visibility.show_memory,
                          std::wstring(L"MEM ") +
                              std::to_wstring(std::max(snapshot.memory_percent, 0)) + L"%");

    if (visibility.show_gpu) {
        if (snapshot.gpu_percent >= 0) {
            AddColumn(lines.columns,
                      std::wstring(L"GPU ") + std::to_wstring(snapshot.gpu_percent) + L"%");
        } else {
            AddColumn(lines.columns, L"GPU --");
        }
    }

    AddOptionalPairColumn(lines.columns,
                          visibility.show_upload,
                          std::wstring(L"\u2191 ") +
                              FormatNetworkRateForDisplay(snapshot.upload_bytes_per_second,
                                                          network_display_unit),
                          visibility.show_download,
                          std::wstring(L"\u2193 ") +
                          FormatNetworkRateForDisplay(snapshot.download_bytes_per_second,
                                                          network_display_unit));
    AddOptionalPairColumn(lines.columns,
                          visibility.show_disk_read,
                          std::wstring(L"R ") +
                              FormatByteRate(snapshot.disk_read_bytes_per_second),
                          visibility.show_disk_write,
                          std::wstring(L"W ") +
                              FormatByteRate(snapshot.disk_write_bytes_per_second));

    NormalizeDisplayLines(lines, use_chinese_labels);
    return lines;
}

DisplayLines GetMetricsSampleLines(const MetricVisibility& visibility,
                                   NetworkDisplayUnit network_display_unit,
                                   bool use_chinese_labels) {
    DisplayLines lines{};
    AddOptionalPairColumn(
        lines.columns,
        visibility.show_cpu,
        L"CPU 100%",
        visibility.show_memory,
        L"MEM 100%");
    if (visibility.show_gpu) {
        AddColumn(lines.columns, L"GPU 100%");
    }
    AddOptionalPairColumn(lines.columns,
                          visibility.show_upload,
                          network_display_unit == NetworkDisplayUnit::kBytesPerSecond
                              ? L"\u2191 999MB/s"
                              : L"\u2191 999Mbps",
                          visibility.show_download,
                          network_display_unit == NetworkDisplayUnit::kBytesPerSecond
                              ? L"\u2193 999MB/s"
                              : L"\u2193 999Mbps");
    AddOptionalPairColumn(lines.columns,
                          visibility.show_disk_read,
                          L"R 99.9GB/s",
                          visibility.show_disk_write,
                          L"W 99.9GB/s");

    NormalizeDisplayLines(lines, use_chinese_labels);
    return lines;
}

bool IsLightTaskbarTheme() {
    DWORD value = 0;
    if (!QueryThemeValue(value)) {
        return false;
    }
    return value != 0;
}

}  // namespace minimal_taskbar_monitor
