#include "stock_config.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <algorithm>
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

AppConfig DefaultConfig() {
    AppConfig config;
    config.stocks = {
        {L"POP", L"09992", L"hk", L"", L"", 1.0, false, 255.0, 260.0},
        {L"JD", L"09618", L"hk", L"", L"", 2.0, true, 134.0, 142.0},
        {L"BABA", L"09988", L"hk", L"", L"", 8.0, true, 170.0, 175.0},
        {L"Nio", L"09866", L"hk", L"", L"", 1.0, true, 52.0, 58.0},
        {L"XM", L"01810", L"hk", L"", L"", 1.0, false, std::nullopt, 56.5},
        {L"PINGAN", L"000001", L"cn", L"", L"", 1.0, false, std::nullopt, std::nullopt},
        {L"NVDA", L"NVDA", L"us", L"alpaca", L"", 1.0, false, std::nullopt, std::nullopt},
    };
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
        config.alpaca_key_id = Utf8ToWide(ReadStringValue(*settings, "alpaca_key_id").value_or(""));
        config.alpaca_secret_key =
            Utf8ToWide(ReadStringValue(*settings, "alpaca_secret_key").value_or(""));
        config.alpaca_feed = Utf8ToWide(ReadStringValue(*settings, "alpaca_feed").value_or("iex"));
        config.popup_activation_mode = ParsePopupActivationMode(
            ReadStringValue(*settings, "popup_activation_mode").value_or("hover"));
        config.sort_mode =
            ParseStockSortMode(ReadStringValue(*settings, "sort_mode").value_or("config"));
        if (auto count = ReadDoubleValue(*settings, "taskbar_symbol_count")) {
            const unsigned int requested = static_cast<unsigned int>(*count);
            if (requested == 2 || requested == 4 || requested == 6 || requested == 8) {
                config.taskbar_symbol_count = requested;
            }
        }
    }

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

        if (symbol == "_settings") {
            continue;
        }
        auto code = ReadStringValue(object, "code");
        if (!code || code->empty()) {
            continue;
        }

        StockTarget target;
        target.symbol = Utf8ToWide(symbol);
        target.code = Utf8ToWide(*code);
        target.market = Utf8ToWide(ReadStringValue(object, "market").value_or("hk"));
        target.source = Utf8ToWide(ReadStringValue(object, "source").value_or(""));
        target.alpaca_feed = Utf8ToWide(ReadStringValue(object, "alpaca_feed").value_or(""));
        target.adr_factor = ReadDoubleValue(object, "adr_factor").value_or(1.0);
        target.show_usd = ReadBoolValue(object, "show_usd").value_or(false);
        target.min_price = ReadDoubleValue(object, "min_price");
        target.max_price = ReadDoubleValue(object, "max_price");
        config.stocks.push_back(target);
    }

    if (config.stocks.empty()) {
        return DefaultConfig();
    }
    return config;
}

bool SaveConfig(const AppConfig& config) {
    const std::filesystem::path path(GetConfigPath());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    output << "{\n";
    output << "  \"_settings\": {\n";
    output << "    \"sample_interval_seconds\": " << config.sample_interval_seconds << ",\n";
    output << "    \"usd_hkd_rate\": " << config.usd_hkd_rate << ",\n";
    output << "    \"alpaca_key_id\": \"" << WideToUtf8(config.alpaca_key_id) << "\",\n";
    output << "    \"alpaca_secret_key\": \"" << WideToUtf8(config.alpaca_secret_key) << "\",\n";
    output << "    \"alpaca_feed\": \"" << WideToUtf8(config.alpaca_feed) << "\",\n";
    output << "    \"popup_activation_mode\": \""
           << SerializePopupActivationMode(config.popup_activation_mode) << "\",\n";
    output << "    \"taskbar_symbol_count\": " << config.taskbar_symbol_count << ",\n";
    output << "    \"sort_mode\": \"" << SerializeStockSortMode(config.sort_mode) << "\"\n";
    output << "  }";
    for (const auto& stock : config.stocks) {
        output << ",\n  \"" << WideToUtf8(stock.symbol) << "\": {\n";
        output << "    \"code\": \"" << WideToUtf8(stock.code) << "\",\n";
        output << "    \"market\": \"" << WideToUtf8(stock.market) << "\",\n";
        if (!stock.source.empty()) {
            output << "    \"source\": \"" << WideToUtf8(stock.source) << "\",\n";
        }
        if (!stock.alpaca_feed.empty()) {
            output << "    \"alpaca_feed\": \"" << WideToUtf8(stock.alpaca_feed) << "\",\n";
        }
        output << "    \"adr_factor\": " << stock.adr_factor << ",\n";
        output << "    \"show_usd\": " << (stock.show_usd ? "true" : "false");
        if (stock.min_price) {
            output << ",\n    \"min_price\": " << *stock.min_price;
        }
        if (stock.max_price) {
            output << ",\n    \"max_price\": " << *stock.max_price;
        }
        output << "\n  }";
    }
    output << "\n}\n";
    return true;
}

}  // namespace stock_taskbar_monitor
