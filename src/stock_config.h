#pragma once

#include <optional>
#include <string>
#include <vector>

namespace stock_taskbar_monitor {

enum class PopupActivationMode {
    kHover,
    kClick,
};

enum class StockSortMode {
    kConfigOrder,
    kTopGainers,
    kTopLosers,
};

struct StockTarget {
    std::wstring symbol;
    std::wstring code;
    std::wstring market{L"hk"};
    std::wstring source{};
    std::wstring alpaca_feed{};
    double adr_factor{1.0};
    bool show_usd{false};
    std::optional<double> min_price;
    std::optional<double> max_price;
};

struct AppConfig {
    unsigned int sample_interval_seconds{2};
    double usd_hkd_rate{7.84};
    std::wstring alpaca_key_id;
    std::wstring alpaca_secret_key;
    std::wstring alpaca_feed{L"iex"};
    PopupActivationMode popup_activation_mode{PopupActivationMode::kHover};
    unsigned int taskbar_symbol_count{4};
    StockSortMode sort_mode{StockSortMode::kConfigOrder};
    std::vector<StockTarget> stocks;
};

std::wstring GetConfigPath();
AppConfig LoadOrCreateConfig();
bool SaveConfig(const AppConfig& config);

}  // namespace stock_taskbar_monitor
