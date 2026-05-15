#pragma once

#include "stock_config.h"

#include <optional>
#include <string>

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
std::optional<NetworkUtcTime> GetEstimatedNetworkUtcTime();

}  // namespace stock_taskbar_monitor
