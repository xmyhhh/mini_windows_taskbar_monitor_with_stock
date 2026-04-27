#include "process_monitor.h"

#include <windows.h>
#include <psapi.h>

#include <pdhmsg.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minimal_taskbar_monitor {

namespace {

constexpr wchar_t kProcessV2ObjectName[] = L"Process V2";
constexpr wchar_t kProcessObjectName[] = L"Process";
constexpr wchar_t kCpuCounterName[] = L"% Processor Time";
constexpr wchar_t kWorkingSetCounterName[] = L"Working Set";
constexpr wchar_t kPrivateWorkingSetCounterName[] = L"Working Set - Private";
constexpr wchar_t kPrivateBytesCounterName[] = L"Private Bytes";
constexpr wchar_t kIoReadCounterName[] = L"IO Read Bytes/sec";
constexpr wchar_t kIoWriteCounterName[] = L"IO Write Bytes/sec";
constexpr wchar_t kIoOtherCounterName[] = L"IO Other Bytes/sec";
constexpr wchar_t kIdProcessCounterName[] = L"ID Process";
constexpr wchar_t kGpuEngineCounterPath[] = L"\\GPU Engine(*)\\Utilization Percentage";
constexpr wchar_t kGpuProcessMemoryDedicatedCounterPath[] =
    L"\\GPU Process Memory(*)\\Dedicated Usage";
constexpr wchar_t kGpuProcessMemorySharedCounterPath[] = L"\\GPU Process Memory(*)\\Shared Usage";

constexpr double kMegabyte = 1024.0 * 1024.0;

struct ProcessMemoryCountersEx2Compat {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
    SIZE_T PrivateWorkingSetSize;
    ULONGLONG SharedCommitUsage;
};

template <typename TValue, typename Extractor>
bool QueryCounterArray(PDH_HCOUNTER counter_handle,
                       DWORD format,
                       Extractor extractor,
                       std::vector<std::pair<std::wstring, TValue>>& values) {
    values.clear();

    DWORD buffer_size = 0;
    DWORD item_count = 0;
    PDH_STATUS status =
        PdhGetFormattedCounterArrayW(counter_handle, format, &buffer_size, &item_count, nullptr);
    if (status != PDH_MORE_DATA || buffer_size == 0) {
        return false;
    }

    std::vector<BYTE> buffer(buffer_size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(counter_handle, format, &buffer_size, &item_count, items);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    values.reserve(item_count);
    for (DWORD i = 0; i < item_count; ++i) {
        const DWORD counter_status = items[i].FmtValue.CStatus;
        if (counter_status != ERROR_SUCCESS && counter_status != PDH_CSTATUS_VALID_DATA &&
            counter_status != PDH_CSTATUS_NEW_DATA) {
            continue;
        }

        values.emplace_back(items[i].szName != nullptr ? items[i].szName : L"",
                            extractor(items[i].FmtValue));
    }
    return true;
}

void CloseQueryIfNeeded(PDH_HQUERY& query_handle) {
    if (query_handle != nullptr) {
        PdhCloseQuery(query_handle);
        query_handle = nullptr;
    }
}

bool AddCounterWithFallback(PDH_HQUERY query_handle,
                            const std::wstring& counter_path,
                            PDH_HCOUNTER& counter_handle) {
    counter_handle = nullptr;
    PDH_STATUS status = PdhAddCounterW(query_handle, counter_path.c_str(), 0, &counter_handle);
    if (status != ERROR_SUCCESS) {
        status = PdhAddEnglishCounterW(query_handle, counter_path.c_str(), 0, &counter_handle);
    }
    return status == ERROR_SUCCESS;
}

std::wstring MakeWildcardCounterPath(const wchar_t* object_name, const wchar_t* counter_name) {
    return std::wstring(L"\\") + object_name + L"(*)\\" + counter_name;
}

double Clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

double NormalizeShare(double value, double max_value) {
    if (value <= 0.0 || max_value <= 0.0) {
        return 0.0;
    }
    return Clamp01(value / max_value);
}

double NormalizeLogScale(double value, double reference_value) {
    if (value <= 0.0 || reference_value <= 0.0) {
        return 0.0;
    }
    return Clamp01(std::log1p(value) / std::log1p(reference_value));
}

bool TryParseUnsigned(const std::wstring& text, DWORD& value) {
    if (text.empty()) {
        return false;
    }

    unsigned long long parsed_value = 0;
    for (wchar_t character : text) {
        if (!iswdigit(character)) {
            return false;
        }
        parsed_value = parsed_value * 10ULL + static_cast<unsigned long long>(character - L'0');
        if (parsed_value > MAXDWORD) {
            return false;
        }
    }

    value = static_cast<DWORD>(parsed_value);
    return true;
}

bool ParseProcessV2Instance(const std::wstring& instance_name,
                            std::wstring& process_name,
                            DWORD& process_id) {
    const size_t separator_pos = instance_name.rfind(L':');
    if (separator_pos == std::wstring::npos || separator_pos + 1 >= instance_name.size()) {
        return false;
    }

    DWORD parsed_pid = 0;
    if (!TryParseUnsigned(instance_name.substr(separator_pos + 1), parsed_pid)) {
        return false;
    }

    process_name = instance_name.substr(0, separator_pos);
    process_id = parsed_pid;
    return true;
}

bool ParseGpuEnginePid(const std::wstring& instance_name, DWORD& process_id) {
    constexpr wchar_t kPidMarker[] = L"pid_";
    const size_t marker_pos = instance_name.find(kPidMarker);
    if (marker_pos == std::wstring::npos) {
        return false;
    }

    size_t value_start = marker_pos + (sizeof(kPidMarker) / sizeof(kPidMarker[0])) - 1;
    size_t value_end = value_start;
    while (value_end < instance_name.size() && iswdigit(instance_name[value_end])) {
        ++value_end;
    }

    if (value_end == value_start) {
        return false;
    }

    return TryParseUnsigned(instance_name.substr(value_start, value_end - value_start), process_id);
}

std::wstring StripProcessInstanceSuffix(const std::wstring& name) {
    const size_t suffix_pos = name.rfind(L'#');
    if (suffix_pos == std::wstring::npos || suffix_pos + 1 >= name.size()) {
        return name;
    }

    for (size_t index = suffix_pos + 1; index < name.size(); ++index) {
        if (!iswdigit(name[index])) {
            return name;
        }
    }
    return name.substr(0, suffix_pos);
}

std::wstring QueryExecutableName(DWORD process_id) {
    HANDLE process_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process_handle == nullptr) {
        return L"";
    }

    std::wstring buffer(512, L'\0');
    std::wstring executable_name;
    while (true) {
        DWORD size = static_cast<DWORD>(buffer.size());
        if (QueryFullProcessImageNameW(process_handle, 0, buffer.data(), &size)) {
            buffer.resize(size);
            executable_name = std::filesystem::path(buffer).filename().wstring();
            break;
        }

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer.size() >= 32768) {
            break;
        }

        buffer.resize(buffer.size() * 2);
    }

    CloseHandle(process_handle);
    return executable_name;
}

bool QueryProcessMemorySnapshot(DWORD process_id,
                                unsigned long long& working_set_bytes,
                                unsigned long long& private_working_set_bytes,
                                unsigned long long& commit_bytes) {
    working_set_bytes = 0;
    private_working_set_bytes = 0;
    commit_bytes = 0;

    HANDLE process_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process_handle == nullptr) {
        return false;
    }

    bool success = false;

    ProcessMemoryCountersEx2Compat counters_ex2{};
    counters_ex2.cb = sizeof(counters_ex2);
    if (GetProcessMemoryInfo(process_handle,
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters_ex2),
                             counters_ex2.cb)) {
        working_set_bytes = static_cast<unsigned long long>(counters_ex2.WorkingSetSize);
        private_working_set_bytes =
            static_cast<unsigned long long>(counters_ex2.PrivateWorkingSetSize);
        commit_bytes = static_cast<unsigned long long>(counters_ex2.PrivateUsage);
        success = true;
    } else {
        PROCESS_MEMORY_COUNTERS_EX counters_ex{};
        counters_ex.cb = sizeof(counters_ex);
        if (GetProcessMemoryInfo(process_handle,
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters_ex),
                                 counters_ex.cb)) {
            working_set_bytes = static_cast<unsigned long long>(counters_ex.WorkingSetSize);
            commit_bytes = static_cast<unsigned long long>(
                counters_ex.PrivateUsage > 0 ? counters_ex.PrivateUsage : counters_ex.PagefileUsage);
            success = true;
        }
    }

    CloseHandle(process_handle);
    return success;
}

}  // namespace

ProcessMonitor::ProcessMonitor() {
    logical_processor_count_ = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = sizeof(memory_status);
    if (GlobalMemoryStatusEx(&memory_status)) {
        total_physical_memory_bytes_ = memory_status.ullTotalPhys;
    }

    InitializeProcessQuery();
    InitializeGpuQuery();
}

ProcessMonitor::~ProcessMonitor() {
    CloseQueryIfNeeded(process_query_);
    CloseQueryIfNeeded(gpu_query_);
}

bool ProcessMonitor::InitializeProcessQuery() {
    CloseQueryIfNeeded(process_query_);
    process_cpu_counter_ = nullptr;
    process_working_set_counter_ = nullptr;
    process_private_working_set_counter_ = nullptr;
    process_private_bytes_counter_ = nullptr;
    process_io_read_counter_ = nullptr;
    process_io_write_counter_ = nullptr;
    process_io_other_counter_ = nullptr;
    process_id_counter_ = nullptr;
    process_query_initialized_ = false;
    has_network_proxy_counter_ = false;
    process_object_mode_ = ProcessObjectMode::kNone;

    const struct {
        const wchar_t* object_name;
        ProcessObjectMode mode;
        bool needs_id_counter;
    } candidates[] = {
        {kProcessV2ObjectName, ProcessObjectMode::kProcessV2, false},
        {kProcessObjectName, ProcessObjectMode::kProcess, true},
    };

    for (const auto& candidate : candidates) {
        PDH_HQUERY query_handle = nullptr;
        if (PdhOpenQueryW(nullptr, 0, &query_handle) != ERROR_SUCCESS) {
            continue;
        }

        PDH_HCOUNTER cpu_counter = nullptr;
        PDH_HCOUNTER working_set_counter = nullptr;
        PDH_HCOUNTER private_working_set_counter = nullptr;
        PDH_HCOUNTER private_bytes_counter = nullptr;
        PDH_HCOUNTER io_read_counter = nullptr;
        PDH_HCOUNTER io_write_counter = nullptr;
        PDH_HCOUNTER io_other_counter = nullptr;
        PDH_HCOUNTER id_counter = nullptr;

        const bool counters_ready =
            AddCounterWithFallback(query_handle,
                                   MakeWildcardCounterPath(candidate.object_name, kCpuCounterName),
                                   cpu_counter) &&
            AddCounterWithFallback(query_handle,
                                   MakeWildcardCounterPath(candidate.object_name,
                                                           kWorkingSetCounterName),
                                   working_set_counter) &&
            AddCounterWithFallback(query_handle,
                                   MakeWildcardCounterPath(candidate.object_name,
                                                           kPrivateWorkingSetCounterName),
                                   private_working_set_counter) &&
            AddCounterWithFallback(query_handle,
                                   MakeWildcardCounterPath(candidate.object_name,
                                                           kPrivateBytesCounterName),
                                   private_bytes_counter) &&
            AddCounterWithFallback(query_handle,
                                   MakeWildcardCounterPath(candidate.object_name, kIoReadCounterName),
                                   io_read_counter) &&
            AddCounterWithFallback(query_handle,
                                   MakeWildcardCounterPath(candidate.object_name, kIoWriteCounterName),
                                   io_write_counter) &&
            (!candidate.needs_id_counter ||
             AddCounterWithFallback(query_handle,
                                    MakeWildcardCounterPath(candidate.object_name,
                                                            kIdProcessCounterName),
                                    id_counter));

        if (!counters_ready) {
            CloseQueryIfNeeded(query_handle);
            continue;
        }

        has_network_proxy_counter_ =
            AddCounterWithFallback(query_handle,
                                   MakeWildcardCounterPath(candidate.object_name,
                                                           kIoOtherCounterName),
                                   io_other_counter);

        if (PdhCollectQueryData(query_handle) != ERROR_SUCCESS) {
            CloseQueryIfNeeded(query_handle);
            continue;
        }

        process_query_ = query_handle;
        process_cpu_counter_ = cpu_counter;
        process_working_set_counter_ = working_set_counter;
        process_private_working_set_counter_ = private_working_set_counter;
        process_private_bytes_counter_ = private_bytes_counter;
        process_io_read_counter_ = io_read_counter;
        process_io_write_counter_ = io_write_counter;
        process_io_other_counter_ = io_other_counter;
        process_id_counter_ = id_counter;
        process_object_mode_ = candidate.mode;
        process_query_initialized_ = true;
        return true;
    }

    return false;
}

bool ProcessMonitor::InitializeGpuQuery() {
    CloseQueryIfNeeded(gpu_query_);
    gpu_engine_counter_ = nullptr;
    gpu_memory_dedicated_counter_ = nullptr;
    gpu_memory_shared_counter_ = nullptr;
    gpu_query_initialized_ = false;
    gpu_memory_counters_available_ = false;

    if (PdhOpenQueryW(nullptr, 0, &gpu_query_) != ERROR_SUCCESS) {
        return false;
    }

    if (!AddCounterWithFallback(gpu_query_, kGpuEngineCounterPath, gpu_engine_counter_)) {
        CloseQueryIfNeeded(gpu_query_);
        return false;
    }

    const bool dedicated_ok = AddCounterWithFallback(gpu_query_,
                                                     kGpuProcessMemoryDedicatedCounterPath,
                                                     gpu_memory_dedicated_counter_);
    const bool shared_ok = AddCounterWithFallback(gpu_query_,
                                                  kGpuProcessMemorySharedCounterPath,
                                                  gpu_memory_shared_counter_);
    gpu_memory_counters_available_ = dedicated_ok && shared_ok;

    if (PdhCollectQueryData(gpu_query_) != ERROR_SUCCESS) {
        CloseQueryIfNeeded(gpu_query_);
        return false;
    }

    gpu_query_initialized_ = true;
    return true;
}

std::vector<ProcessMonitor::RawProcessSample> ProcessMonitor::CollectProcessSamples() {
    if (!process_query_initialized_ || PdhCollectQueryData(process_query_) != ERROR_SUCCESS) {
        return {};
    }

    std::vector<std::pair<std::wstring, double>> cpu_values;
    std::vector<std::pair<std::wstring, long long>> working_set_values;
    std::vector<std::pair<std::wstring, long long>> private_working_set_values;
    std::vector<std::pair<std::wstring, long long>> private_bytes_values;
    std::vector<std::pair<std::wstring, double>> io_read_values;
    std::vector<std::pair<std::wstring, double>> io_write_values;
    std::vector<std::pair<std::wstring, double>> io_other_values;
    std::vector<std::pair<std::wstring, long>> process_id_values;

    if (!QueryCounterArray<double>(process_cpu_counter_,
                                   PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                   [](const PDH_FMT_COUNTERVALUE& value) {
                                       return value.doubleValue;
                                   },
                                   cpu_values) ||
        !QueryCounterArray<long long>(process_working_set_counter_,
                                      PDH_FMT_LARGE,
                                      [](const PDH_FMT_COUNTERVALUE& value) {
                                          return value.largeValue;
                                      },
                                      working_set_values) ||
        !QueryCounterArray<long long>(process_private_working_set_counter_,
                                      PDH_FMT_LARGE,
                                      [](const PDH_FMT_COUNTERVALUE& value) {
                                          return value.largeValue;
                                      },
                                      private_working_set_values) ||
        !QueryCounterArray<long long>(process_private_bytes_counter_,
                                      PDH_FMT_LARGE,
                                      [](const PDH_FMT_COUNTERVALUE& value) {
                                          return value.largeValue;
                                      },
                                      private_bytes_values) ||
        !QueryCounterArray<double>(process_io_read_counter_,
                                   PDH_FMT_DOUBLE,
                                   [](const PDH_FMT_COUNTERVALUE& value) {
                                       return value.doubleValue;
                                   },
                                   io_read_values) ||
        !QueryCounterArray<double>(process_io_write_counter_,
                                   PDH_FMT_DOUBLE,
                                   [](const PDH_FMT_COUNTERVALUE& value) {
                                       return value.doubleValue;
                                   },
                                   io_write_values)) {
        return {};
    }

    if (has_network_proxy_counter_) {
        QueryCounterArray<double>(process_io_other_counter_,
                                  PDH_FMT_DOUBLE,
                                  [](const PDH_FMT_COUNTERVALUE& value) {
                                      return value.doubleValue;
                                  },
                                  io_other_values);
    }

    if (process_object_mode_ == ProcessObjectMode::kProcess &&
        !QueryCounterArray<long>(process_id_counter_,
                                 PDH_FMT_LONG,
                                 [](const PDH_FMT_COUNTERVALUE& value) {
                                     return value.longValue;
                                 },
                                 process_id_values)) {
        return {};
    }

    std::unordered_map<std::wstring, RawProcessSample> samples_by_instance;
    auto ensure_sample = [&samples_by_instance](const std::wstring& instance_name) -> RawProcessSample& {
        RawProcessSample& sample = samples_by_instance[instance_name];
        sample.instance_name = instance_name;
        return sample;
    };

    for (const auto& [instance_name, value] : cpu_values) {
        ensure_sample(instance_name).cpu_percent = std::max(0.0, value);
    }
    for (const auto& [instance_name, value] : working_set_values) {
        ensure_sample(instance_name).working_set_bytes =
            value > 0 ? static_cast<unsigned long long>(value) : 0ULL;
    }
    for (const auto& [instance_name, value] : private_working_set_values) {
        ensure_sample(instance_name).private_working_set_bytes =
            value > 0 ? static_cast<unsigned long long>(value) : 0ULL;
    }
    for (const auto& [instance_name, value] : private_bytes_values) {
        ensure_sample(instance_name).private_bytes =
            value > 0 ? static_cast<unsigned long long>(value) : 0ULL;
        ensure_sample(instance_name).vms_bytes =
            value > 0 ? static_cast<unsigned long long>(value) : 0ULL;
    }
    for (const auto& [instance_name, value] : io_read_values) {
        ensure_sample(instance_name).io_read_bytes_per_second =
            value > 0.0 ? static_cast<unsigned long long>(std::llround(value)) : 0ULL;
    }
    for (const auto& [instance_name, value] : io_write_values) {
        ensure_sample(instance_name).io_write_bytes_per_second =
            value > 0.0 ? static_cast<unsigned long long>(std::llround(value)) : 0ULL;
    }
    for (const auto& [instance_name, value] : io_other_values) {
        ensure_sample(instance_name).network_bytes_per_second =
            value > 0.0 ? static_cast<unsigned long long>(std::llround(value)) : 0ULL;
    }

    if (process_object_mode_ == ProcessObjectMode::kProcessV2) {
        for (auto& [_, sample] : samples_by_instance) {
            DWORD process_id = 0;
            std::wstring process_name;
            if (!ParseProcessV2Instance(sample.instance_name, process_name, process_id)) {
                continue;
            }
            sample.pid = process_id;
            sample.fallback_name = process_name;
        }
    } else {
        for (const auto& [instance_name, value] : process_id_values) {
            auto iterator = samples_by_instance.find(instance_name);
            if (iterator == samples_by_instance.end() || value < 0) {
                continue;
            }
            iterator->second.pid = static_cast<DWORD>(value);
            iterator->second.fallback_name = StripProcessInstanceSuffix(instance_name);
        }
    }

    std::unordered_map<DWORD, double> gpu_by_pid;
    std::unordered_map<DWORD, unsigned long long> gpu_dedicated_by_pid;
    std::unordered_map<DWORD, unsigned long long> gpu_shared_by_pid;
    if (gpu_query_initialized_ && PdhCollectQueryData(gpu_query_) == ERROR_SUCCESS) {
        std::vector<std::pair<std::wstring, double>> gpu_values;
        if (QueryCounterArray<double>(gpu_engine_counter_,
                                      PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                      [](const PDH_FMT_COUNTERVALUE& value) {
                                          return value.doubleValue;
                                      },
                                      gpu_values)) {
            for (const auto& [instance_name, value] : gpu_values) {
                DWORD process_id = 0;
                if (!ParseGpuEnginePid(instance_name, process_id)) {
                    continue;
                }
                gpu_by_pid[process_id] += std::max(0.0, value);
            }
        }

        if (gpu_memory_counters_available_) {
            std::vector<std::pair<std::wstring, long long>> gpu_dedicated_values;
            std::vector<std::pair<std::wstring, long long>> gpu_shared_values;
            if (QueryCounterArray<long long>(gpu_memory_dedicated_counter_,
                                             PDH_FMT_LARGE,
                                             [](const PDH_FMT_COUNTERVALUE& value) {
                                                 return value.largeValue;
                                             },
                                             gpu_dedicated_values)) {
                for (const auto& [instance_name, value] : gpu_dedicated_values) {
                    DWORD process_id = 0;
                    if (!ParseGpuEnginePid(instance_name, process_id) || value <= 0) {
                        continue;
                    }
                    gpu_dedicated_by_pid[process_id] += static_cast<unsigned long long>(value);
                }
            }
            if (QueryCounterArray<long long>(gpu_memory_shared_counter_,
                                             PDH_FMT_LARGE,
                                             [](const PDH_FMT_COUNTERVALUE& value) {
                                                 return value.largeValue;
                                             },
                                             gpu_shared_values)) {
                for (const auto& [instance_name, value] : gpu_shared_values) {
                    DWORD process_id = 0;
                    if (!ParseGpuEnginePid(instance_name, process_id) || value <= 0) {
                        continue;
                    }
                    gpu_shared_by_pid[process_id] += static_cast<unsigned long long>(value);
                }
            }
        }
    }

    std::vector<RawProcessSample> samples;
    samples.reserve(samples_by_instance.size());
    for (auto& [_, sample] : samples_by_instance) {
        const std::wstring normalized_name = StripProcessInstanceSuffix(sample.fallback_name.empty()
                                                                            ? sample.instance_name
                                                                            : sample.fallback_name);
        if (sample.pid == 0 || normalized_name == L"_Total" || normalized_name == L"Idle") {
            continue;
        }

        auto gpu_iterator = gpu_by_pid.find(sample.pid);
        if (gpu_iterator != gpu_by_pid.end()) {
            sample.gpu_percent = std::clamp(gpu_iterator->second, 0.0, 100.0);
        }
        auto gpu_dedicated_iterator = gpu_dedicated_by_pid.find(sample.pid);
        if (gpu_dedicated_iterator != gpu_dedicated_by_pid.end()) {
            sample.gpu_dedicated_bytes = gpu_dedicated_iterator->second;
        }
        auto gpu_shared_iterator = gpu_shared_by_pid.find(sample.pid);
        if (gpu_shared_iterator != gpu_shared_by_pid.end()) {
            sample.gpu_shared_bytes = gpu_shared_iterator->second;
        }

        sample.cpu_percent =
            std::clamp(sample.cpu_percent / static_cast<double>(logical_processor_count_),
                       0.0,
                       100.0);
        samples.push_back(std::move(sample));
    }

    RefreshProcessMemoryMetrics(samples);

    return samples;
}

void ProcessMonitor::RefreshProcessMemoryMetrics(std::vector<RawProcessSample>& samples) const {
    for (auto& sample : samples) {
        unsigned long long working_set_bytes = 0;
        unsigned long long private_working_set_bytes = 0;
        unsigned long long commit_bytes = 0;
        if (!QueryProcessMemorySnapshot(sample.pid,
                                        working_set_bytes,
                                        private_working_set_bytes,
                                        commit_bytes)) {
            continue;
        }

        if (working_set_bytes > 0) {
            sample.working_set_bytes = working_set_bytes;
        }
        if (private_working_set_bytes > 0) {
            sample.private_working_set_bytes = private_working_set_bytes;
        }
        if (commit_bytes > 0) {
            sample.private_bytes = commit_bytes;
            sample.vms_bytes = commit_bytes;
        }
    }
}

void ProcessMonitor::ApplyCompositeScores(std::vector<RawProcessSample>& samples,
                                          const MetricsSnapshot& system_snapshot) const {
    if (samples.empty()) {
        return;
    }

    double max_cpu = 0.0;
    double max_uss = 0.0;
    double max_rss = 0.0;
    double max_private_bytes = 0.0;
    double max_gpu = 0.0;
    double max_gpu_memory = 0.0;
    double max_io = 0.0;
    double max_network = 0.0;
    for (const auto& sample : samples) {
        max_cpu = std::max(max_cpu, sample.cpu_percent);
        max_uss = std::max(max_uss, static_cast<double>(sample.private_working_set_bytes));
        max_rss = std::max(max_rss, static_cast<double>(sample.working_set_bytes));
        max_private_bytes = std::max(max_private_bytes, static_cast<double>(sample.private_bytes));
        max_gpu = std::max(max_gpu, sample.gpu_percent);
        max_gpu_memory =
            std::max(max_gpu_memory,
                     static_cast<double>(sample.gpu_dedicated_bytes) +
                         static_cast<double>(sample.gpu_shared_bytes) * 0.35);
        max_io = std::max(max_io,
                          static_cast<double>(sample.io_read_bytes_per_second +
                                              sample.io_write_bytes_per_second));
        max_network =
            std::max(max_network, static_cast<double>(sample.network_bytes_per_second));
    }

    double cpu_weight = 0.28 + 0.10 * Clamp01(static_cast<double>(std::max(system_snapshot.cpu_percent, 0)) /
                                              100.0);
    double memory_weight =
        0.22 + 0.08 * Clamp01(static_cast<double>(std::max(system_snapshot.memory_percent, 0)) /
                              100.0);
    double gpu_weight = 0.16;
    if (system_snapshot.gpu_percent >= 0) {
        gpu_weight += 0.08 * Clamp01(static_cast<double>(system_snapshot.gpu_percent) / 100.0);
    }
    double io_weight = 0.20 +
                       0.12 * NormalizeLogScale(
                                  static_cast<double>(system_snapshot.disk_read_bytes_per_second +
                                                      system_snapshot.disk_write_bytes_per_second),
                                  256.0 * kMegabyte);
    double network_weight = has_network_proxy_counter_
                                ? 0.14 + 0.10 * NormalizeLogScale(
                                                    static_cast<double>(
                                                        system_snapshot.upload_bytes_per_second +
                                                        system_snapshot.download_bytes_per_second),
                                                    128.0 * kMegabyte)
                                : 0.0;

    if (max_gpu <= 0.0) {
        gpu_weight = 0.0;
    }
    if (max_io <= 0.0) {
        io_weight = 0.0;
    }
    if (max_network <= 0.0) {
        network_weight = 0.0;
    }

    const double weight_sum =
        cpu_weight + memory_weight + gpu_weight + io_weight + network_weight;
    if (weight_sum <= 0.0) {
        return;
    }

    cpu_weight /= weight_sum;
    memory_weight /= weight_sum;
    gpu_weight /= weight_sum;
    io_weight /= weight_sum;
    network_weight /= weight_sum;

    for (auto& sample : samples) {
        const double cpu_component =
            0.60 * Clamp01(sample.cpu_percent / 100.0) +
            0.40 * NormalizeShare(sample.cpu_percent, max_cpu);

        const auto normalize_memory_pressure =
            [this](unsigned long long bytes, double max_value) -> double {
            return 0.50 * (total_physical_memory_bytes_ > 0
                               ? Clamp01(std::sqrt(static_cast<double>(bytes) /
                                                   static_cast<double>(total_physical_memory_bytes_)))
                               : 0.0) +
                   0.50 * NormalizeShare(static_cast<double>(bytes), max_value);
        };

        const double uss_component =
            normalize_memory_pressure(sample.private_working_set_bytes, max_uss);
        const double rss_component =
            normalize_memory_pressure(sample.working_set_bytes, max_rss);
        const double private_bytes_component =
            normalize_memory_pressure(sample.private_bytes, max_private_bytes);
        const double memory_component =
            0.45 * uss_component + 0.30 * rss_component + 0.25 * private_bytes_component;

        const double gpu_util_component =
            0.60 * Clamp01(sample.gpu_percent / 100.0) +
            0.40 * NormalizeShare(sample.gpu_percent, max_gpu);
        const double gpu_memory_pressure =
            static_cast<double>(sample.gpu_dedicated_bytes) +
            static_cast<double>(sample.gpu_shared_bytes) * 0.35;
        const double gpu_memory_component =
            0.45 * NormalizeLogScale(gpu_memory_pressure, 1024.0 * kMegabyte) +
            0.55 * NormalizeShare(gpu_memory_pressure, max_gpu_memory);
        const double gpu_component =
            (max_gpu_memory > 0.0 ? 0.65 * gpu_util_component + 0.35 * gpu_memory_component
                                  : gpu_util_component);

        const double io_bytes = static_cast<double>(sample.io_read_bytes_per_second +
                                                    sample.io_write_bytes_per_second);
        const double io_component =
            0.45 * NormalizeLogScale(io_bytes, 128.0 * kMegabyte) +
            0.55 * NormalizeShare(io_bytes, max_io);

        const double network_component =
            0.45 * NormalizeLogScale(static_cast<double>(sample.network_bytes_per_second),
                                     64.0 * kMegabyte) +
            0.55 * NormalizeShare(static_cast<double>(sample.network_bytes_per_second),
                                  max_network);

        sample.score = 100.0 *
                       (cpu_weight * cpu_component + memory_weight * memory_component +
                        gpu_weight * gpu_component + io_weight * io_component +
                        network_weight * network_component);
    }
}

std::wstring ProcessMonitor::ResolveDisplayName(DWORD pid, const std::wstring& fallback_name) {
    for (const auto& [cached_pid, cached_name] : name_cache_) {
        if (cached_pid == pid) {
            return cached_name;
        }
    }

    std::wstring resolved_name = QueryExecutableName(pid);
    if (resolved_name.empty()) {
        resolved_name = fallback_name.empty() ? (L"PID " + std::to_wstring(pid)) : fallback_name;
    }

    name_cache_.emplace_back(pid, resolved_name);
    if (name_cache_.size() > 1024) {
        name_cache_.erase(name_cache_.begin(), name_cache_.begin() + 256);
    }
    return resolved_name;
}

void ProcessMonitor::ResolveDisplayNames(std::vector<ProcessPopupItem>& items) {
    std::unordered_map<std::wstring, int> name_counts;
    for (auto& item : items) {
        item.name = ResolveDisplayName(item.pid, item.name);
        ++name_counts[item.name];
    }

    for (auto& item : items) {
        if (name_counts[item.name] > 1) {
            item.name += L" (" + std::to_wstring(item.pid) + L")";
        }
    }
}

ProcessPopupSnapshot ProcessMonitor::Sample(const MetricsSnapshot& system_snapshot, size_t max_count) {
    ProcessPopupSnapshot snapshot{};
    snapshot.uptime_ms = GetTickCount64();
    snapshot.network_metric_available = has_network_proxy_counter_;
    snapshot.network_is_estimated = has_network_proxy_counter_;

    std::vector<RawProcessSample> samples = CollectProcessSamples();
    snapshot.total_process_count = static_cast<int>(samples.size());
    if (samples.empty()) {
        return snapshot;
    }

    ApplyCompositeScores(samples, system_snapshot);
    std::stable_sort(samples.begin(), samples.end(), [](const RawProcessSample& left,
                                                        const RawProcessSample& right) {
        if (std::abs(left.score - right.score) > 0.01) {
            return left.score > right.score;
        }
        if (std::abs(left.cpu_percent - right.cpu_percent) > 0.01) {
            return left.cpu_percent > right.cpu_percent;
        }
        if (left.private_working_set_bytes != right.private_working_set_bytes) {
            return left.private_working_set_bytes > right.private_working_set_bytes;
        }
        if (std::abs(left.gpu_percent - right.gpu_percent) > 0.01) {
            return left.gpu_percent > right.gpu_percent;
        }
        if (left.gpu_dedicated_bytes != right.gpu_dedicated_bytes) {
            return left.gpu_dedicated_bytes > right.gpu_dedicated_bytes;
        }
        const unsigned long long left_io =
            left.io_read_bytes_per_second + left.io_write_bytes_per_second;
        const unsigned long long right_io =
            right.io_read_bytes_per_second + right.io_write_bytes_per_second;
        if (left_io != right_io) {
            return left_io > right_io;
        }
        if (left.network_bytes_per_second != right.network_bytes_per_second) {
            return left.network_bytes_per_second > right.network_bytes_per_second;
        }
        return left.pid < right.pid;
    });

    if (max_count != 0 && samples.size() > max_count) {
        samples.resize(max_count);
    }

    snapshot.top_processes.reserve(samples.size());
    for (const auto& sample : samples) {
        snapshot.top_processes.push_back({sample.pid,
                                          sample.fallback_name,
                                          sample.cpu_percent,
                                          sample.working_set_bytes,
                                          sample.private_working_set_bytes,
                                          sample.vms_bytes,
                                          sample.gpu_percent,
                                          sample.gpu_dedicated_bytes,
                                          sample.io_read_bytes_per_second,
                                          sample.io_write_bytes_per_second,
                                          sample.network_bytes_per_second,
                                          sample.score});
    }

    ResolveDisplayNames(snapshot.top_processes);
    return snapshot;
}

}  // namespace minimal_taskbar_monitor
