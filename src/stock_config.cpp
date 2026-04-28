#include "stock_config.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <algorithm>
#include <cwchar>
#include <cstdlib>

namespace stock_taskbar_monitor {
namespace {

constexpr wchar_t kConfigFileName[] = L"stocks_config.json";

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return L"";
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0);
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                        needed);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return "";
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                        needed, nullptr, nullptr);
    return result;
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

bool IsSupportedAlpacaFeed(std::wstring_view feed) {
    return feed == L"sip" || feed == L"iex" || feed == L"delayed_sip" || feed == L"boats" ||
           feed == L"overnight";
}

std::wstring NormalizeAlpacaFeed(const std::wstring& feed) {
    const std::wstring normalized = ToLower(feed);
    if (IsSupportedAlpacaFeed(normalized)) {
        return normalized;
    }
    return L"iex";
}

std::wstring GetExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size()) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    if (length == 0) {
        return L".";
    }
    path.resize(length);
    return std::filesystem::path(path).parent_path().wstring();
}

std::string ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

size_t SkipWhitespace(const std::string& text, size_t pos) {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n')) {
        ++pos;
    }
    return pos;
}

size_t FindMatchingBrace(const std::string& text, size_t open_pos) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = open_pos; i < text.size(); ++i) {
        const char ch = text[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::string::npos;
}

std::optional<std::string> ReadStringValue(const std::string& object, const std::string& key) {
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = object.find(quoted_key);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = object.find(':', pos + quoted_key.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = SkipWhitespace(object, pos + 1);
    if (pos >= object.size() || object[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    std::string result;
    bool escaped = false;
    for (; pos < object.size(); ++pos) {
        const char ch = object[pos];
        if (escaped) {
            result.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            return result;
        } else {
            result.push_back(ch);
        }
    }
    return std::nullopt;
}

std::optional<double> ReadDoubleValue(const std::string& object, const std::string& key) {
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = object.find(quoted_key);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = object.find(':', pos + quoted_key.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = SkipWhitespace(object, pos + 1);
    char* end = nullptr;
    const double value = std::strtod(object.c_str() + pos, &end);
    if (end == object.c_str() + pos) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> ReadBoolValue(const std::string& object, const std::string& key) {
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = object.find(quoted_key);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = object.find(':', pos + quoted_key.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = SkipWhitespace(object, pos + 1);
    if (object.compare(pos, 4, "true") == 0) {
        return true;
    }
    if (object.compare(pos, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

PopupActivationMode ParsePopupActivationMode(std::string_view value) {
    if (value == "click") {
        return PopupActivationMode::kClick;
    }
    return PopupActivationMode::kHover;
}

std::string_view SerializePopupActivationMode(PopupActivationMode mode) {
    switch (mode) {
    case PopupActivationMode::kClick:
        return "click";
    case PopupActivationMode::kHover:
    default:
        return "hover";
    }
}

StockSortMode ParseStockSortMode(std::string_view value) {
    if (value == "top_gainers") {
        return StockSortMode::kTopGainers;
    }
    if (value == "top_losers") {
        return StockSortMode::kTopLosers;
    }
    return StockSortMode::kConfigOrder;
}

std::string_view SerializeStockSortMode(StockSortMode mode) {
    switch (mode) {
    case StockSortMode::kTopGainers:
        return "top_gainers";
    case StockSortMode::kTopLosers:
        return "top_losers";
    case StockSortMode::kConfigOrder:
    default:
        return "config";
    }
}

StockTarget ParseStockTarget(const std::string& symbol_key, const std::string& object) {
    StockTarget target;
    target.symbol = Utf8ToWide(symbol_key);
    target.code = Utf8ToWide(ReadStringValue(object, "code").value_or(""));
    target.market = Utf8ToWide(ReadStringValue(object, "market").value_or("hk"));
    target.source = Utf8ToWide(ReadStringValue(object, "source").value_or(""));
    const std::wstring alpaca_feed =
        Utf8ToWide(ReadStringValue(object, "alpaca_feed").value_or(""));
    target.alpaca_feed = alpaca_feed.empty() ? L"" : NormalizeAlpacaFeed(alpaca_feed);
    target.adr_factor = ReadDoubleValue(object, "adr_factor").value_or(1.0);
    target.show_usd = ReadBoolValue(object, "show_usd").value_or(false);
    target.min_price = ReadDoubleValue(object, "min_price");
    target.max_price = ReadDoubleValue(object, "max_price");
    return target;
}

void AssignDefaultGroups(AppConfig& config) {
    config.active_group = L"Default";
    config.stock_groups = {{config.active_group, config.stocks}};
}

std::vector<StockTarget> ParseStocksFromContainer(const std::string& content,
                                                  bool skip_reserved_keys) {
    std::vector<StockTarget> stocks;
    size_t pos = 0;
    while (true) {
        pos = content.find('"', pos);
        if (pos == std::string::npos) {
            break;
        }
        const size_t key_end = content.find('"', pos + 1);
        if (key_end == std::string::npos) {
            break;
        }
        const std::string symbol = content.substr(pos + 1, key_end - pos - 1);
        pos = SkipWhitespace(content, key_end + 1);
        if (pos >= content.size() || content[pos] != ':') {
            continue;
        }
        pos = SkipWhitespace(content, pos + 1);
        if (pos >= content.size() || content[pos] != '{') {
            continue;
        }
        const size_t object_end = FindMatchingBrace(content, pos);
        if (object_end == std::string::npos) {
            break;
        }
        const std::string object = content.substr(pos, object_end - pos + 1);
        pos = object_end + 1;

        if (skip_reserved_keys && (symbol == "_settings" || symbol == "_groups")) {
            continue;
        }
        auto code = ReadStringValue(object, "code");
        if (!code || code->empty()) {
            continue;
        }
        stocks.push_back(ParseStockTarget(symbol, object));
    }
    return stocks;
}

std::vector<StockGroup> ParseStockGroupsFromObject(const std::string& groups_object) {
    std::vector<StockGroup> groups;
    size_t pos = 0;
    while (true) {
        pos = groups_object.find('"', pos);
        if (pos == std::string::npos) {
            break;
        }
        const size_t key_end = groups_object.find('"', pos + 1);
        if (key_end == std::string::npos) {
            break;
        }
        const std::string group_name = groups_object.substr(pos + 1, key_end - pos - 1);
        pos = SkipWhitespace(groups_object, key_end + 1);
        if (pos >= groups_object.size() || groups_object[pos] != ':') {
            continue;
        }
        pos = SkipWhitespace(groups_object, pos + 1);
        if (pos >= groups_object.size() || groups_object[pos] != '{') {
            continue;
        }
        const size_t object_end = FindMatchingBrace(groups_object, pos);
        if (object_end == std::string::npos) {
            break;
        }
        const std::string object = groups_object.substr(pos, object_end - pos + 1);
        pos = object_end + 1;

        StockGroup group;
        group.name = Utf8ToWide(group_name);
        group.stocks = ParseStocksFromContainer(object, false);
        if (!group.name.empty() && !group.stocks.empty()) {
            groups.push_back(std::move(group));
            if (groups.size() >= kMaxStockGroups) {
                break;
            }
        }
    }
    return groups;
}

void TrimStockGroups(AppConfig& config) {
    if (config.stock_groups.size() > kMaxStockGroups) {
        config.stock_groups.resize(kMaxStockGroups);
    }
}

void ActivateConfiguredGroup(AppConfig& config) {
    TrimStockGroups(config);
    if (config.stock_groups.empty()) {
        AssignDefaultGroups(config);
        return;
    }

    if (config.active_group.empty()) {
        config.active_group = config.stock_groups.front().name;
    }

    auto active_group = std::find_if(config.stock_groups.begin(),
                                     config.stock_groups.end(),
                                     [&config](const StockGroup& group) {
                                         return _wcsicmp(group.name.c_str(),
                                                         config.active_group.c_str()) == 0;
                                     });
    if (active_group == config.stock_groups.end()) {
        active_group = config.stock_groups.begin();
        config.active_group = active_group->name;
    }
    config.stocks = active_group->stocks;
}

AppConfig DefaultConfig() {
    AppConfig config;
    StockGroup hk_group;
    hk_group.name = L"HK";
    hk_group.stocks = {
        {L"POP", L"09992", L"hk", L"", L"", 1.0, false, 255.0, 260.0},
        {L"JD", L"09618", L"hk", L"", L"", 2.0, true, 134.0, 142.0},
        {L"BABA", L"09988", L"hk", L"", L"", 8.0, true, 170.0, 175.0},
        {L"Nio", L"09866", L"hk", L"", L"", 1.0, true, 52.0, 58.0},
        {L"XM", L"01810", L"hk", L"", L"", 1.0, false, std::nullopt, 56.5},
    };

    StockGroup cn_group;
    cn_group.name = L"CN";
    cn_group.stocks = {
        {L"PINGAN", L"000001", L"cn", L"", L"", 1.0, false, std::nullopt, std::nullopt},
    };

    StockGroup us_group;
    us_group.name = L"US";
    us_group.stocks = {
        {L"NVDA", L"NVDA", L"us", L"alpaca", L"", 1.0, false, std::nullopt, std::nullopt},
    };

    config.active_group = hk_group.name;
    config.stock_groups = {hk_group, cn_group, us_group};
    config.stocks = hk_group.stocks;
    return config;
}

std::optional<std::string> ExtractObjectForKey(const std::string& content, const std::string& key) {
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = content.find(quoted_key);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = content.find('{', pos + quoted_key.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    const size_t end = FindMatchingBrace(content, pos);
    if (end == std::string::npos) {
        return std::nullopt;
    }
    return content.substr(pos, end - pos + 1);
}

}  // namespace

std::wstring GetConfigPath() {
    return (std::filesystem::path(GetExecutableDirectory()) / kConfigFileName).wstring();
}

AppConfig LoadOrCreateConfig() {
    const std::filesystem::path path(GetConfigPath());
    if (!std::filesystem::exists(path)) {
        AppConfig config = DefaultConfig();
        SaveConfig(config);
        return config;
    }

    const std::string content = ReadWholeFile(path);
    if (content.empty()) {
        return DefaultConfig();
    }

    AppConfig config;
    if (auto settings = ExtractObjectForKey(content, "_settings")) {
        if (auto interval = ReadDoubleValue(*settings, "sample_interval_seconds")) {
            config.sample_interval_seconds =
                static_cast<unsigned int>(std::clamp(*interval, 1.0, 60.0));
        }
        if (auto rate = ReadDoubleValue(*settings, "usd_hkd_rate")) {
            config.usd_hkd_rate = *rate;
        }
        config.alpaca_endpoint =
            Utf8ToWide(ReadStringValue(*settings, "alpaca_endpoint")
                           .value_or(ReadStringValue(*settings, "endpoint")
                                         .value_or("https://data.alpaca.markets")));
        config.alpaca_key_id =
            Utf8ToWide(ReadStringValue(*settings, "alpaca_key")
                           .value_or(ReadStringValue(*settings, "alpaca_key_id").value_or("")));
        config.alpaca_secret_key =
            Utf8ToWide(ReadStringValue(*settings, "alpaca_secret_key").value_or(""));
        config.alpaca_feed =
            NormalizeAlpacaFeed(Utf8ToWide(ReadStringValue(*settings, "alpaca_feed").value_or("iex")));
        config.popup_activation_mode = ParsePopupActivationMode(
            ReadStringValue(*settings, "popup_activation_mode").value_or("hover"));
        config.sort_mode =
            ParseStockSortMode(ReadStringValue(*settings, "sort_mode").value_or("config"));
        config.active_group =
            Utf8ToWide(ReadStringValue(*settings, "active_group").value_or(""));
        if (auto count = ReadDoubleValue(*settings, "taskbar_symbol_count")) {
            const unsigned int requested = static_cast<unsigned int>(*count);
            if (requested == 2 || requested == 4 || requested == 6 || requested == 8) {
                config.taskbar_symbol_count = requested;
            }
        }
    }

    if (auto groups = ExtractObjectForKey(content, "_groups")) {
        config.stock_groups = ParseStockGroupsFromObject(*groups);
    }
    if (config.stock_groups.empty()) {
        config.stocks = ParseStocksFromContainer(content, true);
        if (!config.stocks.empty()) {
            AssignDefaultGroups(config);
        }
    } else {
        ActivateConfiguredGroup(config);
    }

    if (config.stocks.empty() && config.stock_groups.empty()) {
        return DefaultConfig();
    }
    if (config.stock_groups.empty()) {
        AssignDefaultGroups(config);
    } else if (config.stocks.empty()) {
        ActivateConfiguredGroup(config);
    }
    return config;
}

bool SaveConfig(const AppConfig& config) {
    AppConfig normalized = config;
    if (normalized.stock_groups.empty()) {
        AssignDefaultGroups(normalized);
    }
    normalized.alpaca_feed = NormalizeAlpacaFeed(normalized.alpaca_feed);
    TrimStockGroups(normalized);
    if (normalized.active_group.empty()) {
        normalized.active_group = normalized.stock_groups.front().name;
    }

    auto active_group = std::find_if(normalized.stock_groups.begin(),
                                     normalized.stock_groups.end(),
                                     [&normalized](const StockGroup& group) {
                                         return _wcsicmp(group.name.c_str(),
                                                         normalized.active_group.c_str()) == 0;
                                     });
    if (active_group == normalized.stock_groups.end()) {
        normalized.active_group = normalized.stock_groups.front().name;
        normalized.stocks = normalized.stock_groups.front().stocks;
    } else {
        active_group->stocks = normalized.stocks;
    }

    const std::filesystem::path path(GetConfigPath());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    output << "{\n";
    output << "  \"_settings\": {\n";
    output << "    \"sample_interval_seconds\": " << normalized.sample_interval_seconds << ",\n";
    output << "    \"usd_hkd_rate\": " << normalized.usd_hkd_rate << ",\n";
    output << "    \"alpaca_endpoint\": \"" << WideToUtf8(normalized.alpaca_endpoint) << "\",\n";
    output << "    \"alpaca_key\": \"" << WideToUtf8(normalized.alpaca_key_id) << "\",\n";
    output << "    \"alpaca_secret_key\": \"" << WideToUtf8(normalized.alpaca_secret_key) << "\",\n";
    output << "    \"alpaca_feed\": \"" << WideToUtf8(normalized.alpaca_feed) << "\",\n";
    output << "    \"popup_activation_mode\": \""
           << SerializePopupActivationMode(normalized.popup_activation_mode) << "\",\n";
    output << "    \"taskbar_symbol_count\": " << normalized.taskbar_symbol_count << ",\n";
    output << "    \"sort_mode\": \"" << SerializeStockSortMode(normalized.sort_mode) << "\",\n";
    output << "    \"active_group\": \"" << WideToUtf8(normalized.active_group) << "\"\n";
    output << "  },\n";
    output << "  \"_groups\": {";

    bool first_group = true;
    for (const auto& group : normalized.stock_groups) {
        if (group.name.empty()) {
            continue;
        }
        output << (first_group ? "\n" : ",\n");
        first_group = false;
        output << "    \"" << WideToUtf8(group.name) << "\": {";

        bool first_stock = true;
        for (const auto& stock : group.stocks) {
            if (stock.symbol.empty() || stock.code.empty()) {
                continue;
            }
            const std::wstring stock_alpaca_feed = stock.alpaca_feed.empty()
                                                       ? L""
                                                       : NormalizeAlpacaFeed(stock.alpaca_feed);
            output << (first_stock ? "\n" : ",\n");
            first_stock = false;
            output << "      \"" << WideToUtf8(stock.symbol) << "\": {\n";
            output << "        \"code\": \"" << WideToUtf8(stock.code) << "\",\n";
            output << "        \"market\": \"" << WideToUtf8(stock.market) << "\",\n";
            if (!stock.source.empty()) {
                output << "        \"source\": \"" << WideToUtf8(stock.source) << "\",\n";
            }
            if (!stock_alpaca_feed.empty()) {
                output << "        \"alpaca_feed\": \"" << WideToUtf8(stock_alpaca_feed) << "\",\n";
            }
            output << "        \"adr_factor\": " << stock.adr_factor << ",\n";
            output << "        \"show_usd\": " << (stock.show_usd ? "true" : "false");
            if (stock.min_price) {
                output << ",\n        \"min_price\": " << *stock.min_price;
            }
            if (stock.max_price) {
                output << ",\n        \"max_price\": " << *stock.max_price;
            }
            output << "\n      }";
        }

        if (!first_stock) {
            output << "\n";
        }
        output << "    }";
    }
    if (!first_group) {
        output << "\n";
    }
    output << "  }\n";
    output << "\n}\n";
    return true;
}

}  // namespace stock_taskbar_monitor
