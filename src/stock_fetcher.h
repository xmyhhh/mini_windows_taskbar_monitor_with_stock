#pragma once

#include "stock_config.h"

#include <optional>
#include <functional>
#include <string>
#include <vector>

namespace stock_taskbar_monitor {

struct PriceQuote {
    std::wstring name;
    double price{0.0};
    std::optional<double> change_percent;
};

struct NetworkUtcTime {
    unsigned short year{0};
    unsigned short month{0};
    unsigned short day{0};
    unsigned short day_of_week{0};
    unsigned short hour{0};
    unsigned short minute{0};
    unsigned short second{0};
};

std::optional<PriceQuote> FetchRealtimePrice(const StockTarget& target, const AppConfig& config);
std::optional<PriceQuote> ParseSinaResponse(const std::string& body, const StockTarget& target);
std::vector<std::optional<PriceQuote>> ParseSinaBatchResponse(
    const std::string& body, const std::vector<StockTarget>& targets);
std::vector<std::optional<PriceQuote>> ParseAlpacaBatchResponse(
    const std::string& body, const std::vector<StockTarget>& targets);
std::vector<std::optional<PriceQuote>> FetchRealtimePrices(
    const std::vector<StockTarget>& targets, const AppConfig& config,
    const std::function<bool()>& cancelled);
std::optional<NetworkUtcTime> GetEstimatedNetworkUtcTime();

}  // namespace stock_taskbar_monitor
