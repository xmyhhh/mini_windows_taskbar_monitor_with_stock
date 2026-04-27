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

std::optional<PriceQuote> FetchRealtimePrice(const StockTarget& target, const AppConfig& config);

}  // namespace stock_taskbar_monitor
