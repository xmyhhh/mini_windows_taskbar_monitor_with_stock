#include "stock_fetcher.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <optional>
#include <mutex>
#include <map>
#include <sstream>
#include <string_view>
#include <vector>

namespace stock_taskbar_monitor {
namespace {

std::mutex g_network_time_mutex;
unsigned long long g_network_utc_file_time{0};
unsigned long long g_network_utc_tick_ms{0};

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

std::wstring AnsiToWide(const std::string& value) {
    if (value.empty()) {
        return L"";
    }
    const int needed = MultiByteToWideChar(936, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0);
    if (needed <= 0) {
        return L"";
    }
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(936, 0, value.data(), static_cast<int>(value.size()), result.data(),
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

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return L"";
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0);
    if (needed <= 0) {
        return L"";
    }
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                        needed);
    return result;
}

struct HttpEndpoint {
    std::wstring host;
    std::wstring path_prefix;
    bool https{true};
};

HttpEndpoint ParseHttpEndpoint(std::wstring endpoint, const std::wstring& fallback_host) {
    if (endpoint.empty()) {
        endpoint = L"https://" + fallback_host;
    }

    HttpEndpoint parsed;
    constexpr std::wstring_view https_prefix = L"https://";
    constexpr std::wstring_view http_prefix = L"http://";
    if (endpoint.starts_with(https_prefix)) {
        endpoint.erase(0, https_prefix.size());
        parsed.https = true;
    } else if (endpoint.starts_with(http_prefix)) {
        endpoint.erase(0, http_prefix.size());
        parsed.https = false;
    }

    const size_t slash = endpoint.find(L'/');
    parsed.host = slash == std::wstring::npos ? endpoint : endpoint.substr(0, slash);
    parsed.path_prefix = slash == std::wstring::npos ? L"" : endpoint.substr(slash);
    while (!parsed.path_prefix.empty() && parsed.path_prefix.back() == L'/') {
        parsed.path_prefix.pop_back();
    }
    if (parsed.path_prefix == L"/v2") {
        parsed.path_prefix.clear();
    }
    if (parsed.host.empty()) {
        parsed.host = fallback_host;
    }
    return parsed;
}

void RememberNetworkDateHeader(HINTERNET request) {
    SYSTEMTIME utc_time{};
    DWORD buffer_length = sizeof(utc_time);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &utc_time,
                             &buffer_length,
                             WINHTTP_NO_HEADER_INDEX)) {
        return;
    }

    FILETIME file_time{};
    if (!SystemTimeToFileTime(&utc_time, &file_time)) {
        return;
    }

    ULARGE_INTEGER combined{};
    combined.LowPart = file_time.dwLowDateTime;
    combined.HighPart = file_time.dwHighDateTime;
    std::lock_guard lock(g_network_time_mutex);
    g_network_utc_file_time = combined.QuadPart;
    g_network_utc_tick_ms = GetTickCount64();
}

struct HttpSession {
    HINTERNET handle{WinHttpOpen(L"stock_taskbar_monitor/1.0",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    HttpSession() {
        if (handle) {
            WinHttpSetTimeouts(handle, 1500, 2000, 2500, 2500);
            DWORD retries = 1;
            WinHttpSetOption(handle, WINHTTP_OPTION_CONNECT_RETRIES, &retries, sizeof(retries));
        }
    }
    ~HttpSession() { if (handle) WinHttpCloseHandle(handle); }
};

std::optional<std::string> HttpGet(const std::wstring& host,
                                   const std::wstring& path,
                                   bool https,
                                   const std::wstring& headers) {
    // Reuse connection pooling/TLS state on each sampler thread.
    thread_local HttpSession http_session;
    HINTERNET session = http_session.handle;
    if (session == nullptr) {
        return std::nullopt;
    }

    HINTERNET connect = WinHttpConnect(session, host.c_str(), https ? INTERNET_DEFAULT_HTTPS_PORT
                                                                    : INTERNET_DEFAULT_HTTP_PORT,
                                       0);
    if (connect == nullptr) {
        return std::nullopt;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           https ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr) {
        WinHttpCloseHandle(connect);
        return std::nullopt;
    }

    const ULONGLONG started = GetTickCount64();
    DWORD response_timeout = 5000;
    WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
                     &response_timeout, sizeof(response_timeout));
    bool ok = WinHttpSendRequest(request,
                                 headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                 headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
                                 WINHTTP_NO_REQUEST_DATA,
                                 0,
                                 0,
                                 0) != FALSE &&
              WinHttpReceiveResponse(request, nullptr) != FALSE;

    if (ok) {
        RememberNetworkDateHeader(request);
        DWORD status = 0;
        DWORD size = sizeof(status);
        ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                 WINHTTP_NO_HEADER_INDEX) && status >= 200 && status < 300;
    }

    std::string body;
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            ok = false;
            break;
        }
        if (available == 0) {
            break;
        }
        if (GetTickCount64() - started > 10000 || available > 1024 * 1024 - body.size()) {
            ok = false;
            break;
        }
        const size_t old_size = body.size();
        body.resize(old_size + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + old_size, available, &read)) {
            ok = false;
            break;
        }
        body.resize(old_size + read);
        if (read == 0) {
            break;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    if (!ok || body.empty()) {
        return std::nullopt;
    }
    return body;
}

std::vector<std::string> SplitCsv(const std::string& value) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        if (comma == std::string::npos) {
            fields.push_back(value.substr(start));
            break;
        }
        fields.push_back(value.substr(start, comma - start));
        start = comma + 1;
    }
    return fields;
}

std::optional<double> ParseDouble(const std::string& value) {
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<double> ReadPositivePrice(const std::vector<std::string>& fields, size_t index) {
    if (index >= fields.size()) {
        return std::nullopt;
    }
    const auto value = ParseDouble(fields[index]);
    return value && *value > 0.0 ? value : std::nullopt;
}

std::optional<int> ParseQuoteMinute(const std::string& time) {
    if (time.size() < 5 || time[2] != ':') {
        return std::nullopt;
    }
    for (size_t index : {0, 1, 3, 4}) {
        if (time[index] < '0' || time[index] > '9') {
            return std::nullopt;
        }
    }
    const int hour = (time[0] - '0') * 10 + time[1] - '0';
    const int minute = (time[3] - '0') * 10 + time[4] - '0';
    if (hour > 23 || minute > 59) {
        return std::nullopt;
    }
    return hour * 60 + minute;
}

bool IsSinaAuctionMinute(bool cn_market, int minute) {
    if (cn_market) {
        return (minute >= 9 * 60 + 15 && minute < 9 * 60 + 25) ||
               (minute >= 14 * 60 + 57 && minute < 15 * 60);
    }
    return (minute >= 9 * 60 && minute < 9 * 60 + 20) ||
           (minute >= 16 * 60 + 1 && minute < 16 * 60 + 10);
}

}  // namespace

std::optional<PriceQuote> ParseSinaResponse(const std::string& body, const StockTarget& target) {
    const size_t first_quote = body.find('"');
    const size_t second_quote = body.find('"', first_quote == std::string::npos ? 0 : first_quote + 1);
    if (first_quote == std::string::npos || second_quote == std::string::npos ||
        second_quote <= first_quote + 1) {
        return std::nullopt;
    }

    const std::vector<std::string> fields =
        SplitCsv(body.substr(first_quote + 1, second_quote - first_quote - 1));
    const std::wstring market = ToLower(target.market);
    const bool cn_market = market == L"cn";
    const size_t price_index = cn_market ? 3 : 6;
    if (fields.size() <= price_index) {
        return std::nullopt;
    }

    const auto previous_close = ReadPositivePrice(fields, cn_market ? 2 : 3);
    auto price = ReadPositivePrice(fields, price_index);
    const size_t time_index = cn_market ? 31 : 18;
    const auto minute = fields.size() > time_index ? ParseQuoteMinute(fields[time_index])
                                                  : std::nullopt;
    if (minute && IsSinaAuctionMinute(cn_market, *minute)) {
        // Sina publishes the indicative auction price in the best bid/ask fields.
        // Select by the quote's own time so a delayed response keeps its market phase.
        auto auction_price = ReadPositivePrice(fields, cn_market ? 6 : 9);
        if (!auction_price) {
            auction_price = ReadPositivePrice(fields, cn_market ? 7 : 10);
        }
        if (auction_price) {
            price = auction_price;
        }
    }
    if (!price) {
        price = previous_close;
    }
    if (!price) {
        return std::nullopt;
    }

    PriceQuote quote;
    quote.name = fields.empty() ? target.symbol : AnsiToWide(fields[0]);
    quote.price = *price;
    if (previous_close) {
        quote.change_percent = ((*price - *previous_close) / *previous_close) * 100.0;
    } else if (!cn_market && fields.size() > 8) {
        quote.change_percent = ParseDouble(fields[8]);
    }
    return quote;
}

namespace {

std::wstring SinaSymbol(const StockTarget& target) {
    if (ToLower(target.market) == L"cn") {
        return std::wstring(!target.code.empty() && target.code[0] == L'6' ? L"sh" : L"sz") + target.code;
    }
    return L"rt_hk" + target.code;
}

std::optional<double> ReadJsonNumber(const std::string& body, const std::string& key) {
    const std::string quoted_key = "\"" + key + "\"";
    const size_t key_pos = body.find(quoted_key);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }
    size_t colon_pos = body.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string::npos) {
        return std::nullopt;
    }
    ++colon_pos;
    while (colon_pos < body.size() &&
           (body[colon_pos] == ' ' || body[colon_pos] == '\t' || body[colon_pos] == '\r' ||
            body[colon_pos] == '\n')) {
        ++colon_pos;
    }
    char* end = nullptr;
    const double parsed = std::strtod(body.c_str() + colon_pos, &end);
    if (end == body.c_str() + colon_pos || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<std::string> ReadJsonString(const std::string& body, const std::string& key) {
    const std::string quoted_key = "\"" + key + "\"";
    const size_t key_pos = body.find(quoted_key);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }
    size_t colon_pos = body.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string::npos) {
        return std::nullopt;
    }
    ++colon_pos;
    while (colon_pos < body.size() &&
           (body[colon_pos] == ' ' || body[colon_pos] == '\t' || body[colon_pos] == '\r' ||
            body[colon_pos] == '\n')) {
        ++colon_pos;
    }
    if (colon_pos >= body.size() || body[colon_pos] != '"') {
        return std::nullopt;
    }
    const size_t end = body.find('"', colon_pos + 1);
    if (end == std::string::npos) {
        return std::nullopt;
    }
    return body.substr(colon_pos + 1, end - colon_pos - 1);
}

std::optional<std::string> ExtractJsonObject(const std::string& body, const std::string& key) {
    const std::string quoted_key = "\"" + key + "\"";
    size_t key_pos = body.find(quoted_key);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }
    size_t colon = body.find(':', key_pos + quoted_key.size());
    if (colon == std::string::npos) return std::nullopt;
    size_t open_pos = body.find_first_not_of(" \t\r\n", colon + 1);
    if (open_pos == std::string::npos || body[open_pos] != '{') {
        return std::nullopt;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = open_pos; i < body.size(); ++i) {
        const char ch = body[i];
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
                return body.substr(open_pos, i - open_pos + 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<PriceQuote> ParseAlpacaSnapshot(const std::string& body, const StockTarget& target) {
    const std::optional<std::string> latest_trade = ExtractJsonObject(body, "latestTrade");
    const std::optional<std::string> daily_bar = ExtractJsonObject(body, "dailyBar");
    const std::optional<std::string> previous_daily_bar = ExtractJsonObject(body, "prevDailyBar");

    auto price = latest_trade ? ReadJsonNumber(*latest_trade, "p") : std::nullopt;
    if ((!price || *price <= 0.0) && daily_bar) {
        price = ReadJsonNumber(*daily_bar, "c");
    }
    if (!price || *price <= 0.0) {
        return std::nullopt;
    }

    PriceQuote quote;
    quote.name =
        Utf8ToWide(latest_trade ? ReadJsonString(*latest_trade, "S").value_or(WideToUtf8(target.symbol))
                                : WideToUtf8(target.symbol));
    quote.price = *price;
    if (previous_daily_bar) {
        if (auto previous_close = ReadJsonNumber(*previous_daily_bar, "c");
            previous_close && *previous_close > 0.0) {
            quote.change_percent = ((*price - *previous_close) / *previous_close) * 100.0;
        }
    }
    return quote;
}

std::optional<PriceQuote> FetchSinaPrice(const StockTarget& target) {
    const std::wstring market = ToLower(target.market);
    std::wstring path;
    if (market == L"cn") {
        const bool sh = !target.code.empty() && target.code[0] == L'6';
        path = L"/list=" + std::wstring(sh ? L"sh" : L"sz") + target.code;
    } else {
        path = L"/list=rt_hk" + target.code;
    }

    const std::wstring headers =
        L"Referer: https://finance.sina.com.cn/\r\n"
        L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        L"AppleWebKit/537.36 Chrome/108.0.0.0 Safari/537.36\r\n";
    auto response = HttpGet(L"hq.sinajs.cn", path, true, headers);
    if (!response) {
        return std::nullopt;
    }
    return ParseSinaResponse(*response, target);
}

std::optional<PriceQuote> FetchAlpacaPrice(const StockTarget& target, const AppConfig& config) {
    if (config.alpaca_key_id.empty() || config.alpaca_secret_key.empty()) {
        return std::nullopt;
    }

    const std::wstring feed = !target.alpaca_feed.empty() ? target.alpaca_feed : config.alpaca_feed;
    const HttpEndpoint endpoint = ParseHttpEndpoint(config.alpaca_endpoint, L"data.alpaca.markets");
    const std::wstring path =
        endpoint.path_prefix + L"/v2/stocks/" + target.code + L"/snapshot?feed=" + feed;
    const std::wstring headers =
        L"APCA-API-KEY-ID: " + config.alpaca_key_id + L"\r\n" +
        L"APCA-API-SECRET-KEY: " + config.alpaca_secret_key + L"\r\n" +
        L"User-Agent: stock_taskbar_monitor/1.0\r\n";

    auto response = HttpGet(endpoint.host, path, endpoint.https, headers);
    if (!response) {
        return std::nullopt;
    }
    return ParseAlpacaSnapshot(*response, target);
}

}  // namespace

std::optional<PriceQuote> FetchRealtimePrice(const StockTarget& target, const AppConfig& config) {
    const std::wstring market = ToLower(target.market);
    const std::wstring source = ToLower(target.source);
    if (source == L"alpaca" || market == L"us") {
        return FetchAlpacaPrice(target, config);
    }
    return FetchSinaPrice(target);
}

std::vector<std::optional<PriceQuote>> ParseSinaBatchResponse(
    const std::string& body, const std::vector<StockTarget>& targets) {
    std::vector<std::optional<PriceQuote>> quotes;
    quotes.reserve(targets.size());
    for (const auto& target : targets) {
        const std::string key = "var hq_str_" + WideToUtf8(SinaSymbol(target)) + "=";
        const auto start = body.find(key);
        const auto end = start == std::string::npos ? start : body.find(';', start);
        quotes.push_back(start == std::string::npos ? std::nullopt :
            ParseSinaResponse(body.substr(start, end == std::string::npos ? end : end - start), target));
    }
    return quotes;
}

std::vector<std::optional<PriceQuote>> ParseAlpacaBatchResponse(
    const std::string& body, const std::vector<StockTarget>& targets) {
    std::vector<std::optional<PriceQuote>> quotes;
    quotes.reserve(targets.size());
    for (const auto& target : targets) {
        auto symbol = WideToUtf8(target.code);
        std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        const auto snapshot = ExtractJsonObject(body, symbol);
        quotes.push_back(snapshot ? ParseAlpacaSnapshot(*snapshot, target) : std::nullopt);
    }
    return quotes;
}

std::wstring EncodeQueryValue(const std::wstring& value) {
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring result;
    for (unsigned char ch : WideToUtf8(value)) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            result += static_cast<wchar_t>(ch);
        } else {
            result += L'%';
            result += hex[ch >> 4];
            result += hex[ch & 15];
        }
    }
    return result;
}

std::vector<std::optional<PriceQuote>> FetchRealtimePrices(
    const std::vector<StockTarget>& targets, const AppConfig& config,
    const std::function<bool()>& cancelled) {
    std::vector<std::optional<PriceQuote>> quotes(targets.size());
    std::vector<size_t> sina_indices;
    std::map<std::wstring, std::vector<size_t>> alpaca_indices;
    for (size_t i = 0; i < targets.size(); ++i) {
        if (ToLower(targets[i].market) != L"us" && ToLower(targets[i].source) != L"alpaca") {
            sina_indices.push_back(i);
        } else {
            const auto& feed = targets[i].alpaca_feed.empty() ? config.alpaca_feed : targets[i].alpaca_feed;
            alpaca_indices[feed].push_back(i);
        }
    }
    // Sina accepts a comma-separated watchlist. Bound the URL size for large lists.
    constexpr size_t batch_size = 50;
    for (size_t begin = 0; begin < sina_indices.size() && !cancelled(); begin += batch_size) {
        std::vector<StockTarget> batch;
        std::wstring path = L"/list=";
        const size_t end = std::min(begin + batch_size, sina_indices.size());
        for (size_t i = begin; i < end; ++i) {
            batch.push_back(targets[sina_indices[i]]);
            if (i != begin) path += L",";
            path += SinaSymbol(batch.back());
        }
        const auto body = HttpGet(L"hq.sinajs.cn", path, true,
                                 L"Referer: https://finance.sina.com.cn/\r\n");
        if (body && !cancelled()) {
            const auto batch_quotes = ParseSinaBatchResponse(*body, batch);
            for (size_t i = begin; i < end; ++i) quotes[sina_indices[i]] = batch_quotes[i - begin];
        }
    }
    if (config.alpaca_key_id.empty() || config.alpaca_secret_key.empty()) return quotes;
    const auto endpoint = ParseHttpEndpoint(config.alpaca_endpoint, L"data.alpaca.markets");
    const std::wstring headers = L"APCA-API-KEY-ID: " + config.alpaca_key_id + L"\r\n" +
                                 L"APCA-API-SECRET-KEY: " + config.alpaca_secret_key + L"\r\n";
    for (const auto& [feed, indices] : alpaca_indices) {
        for (size_t begin = 0; begin < indices.size() && !cancelled(); begin += batch_size) {
            const size_t end = std::min(begin + batch_size, indices.size());
            std::vector<StockTarget> batch;
            std::wstring path = endpoint.path_prefix + L"/v2/stocks/snapshots?symbols=";
            for (size_t i = begin; i < end; ++i) {
                batch.push_back(targets[indices[i]]);
                if (i != begin) path += L",";
                path += EncodeQueryValue(batch.back().code);
            }
            path += L"&feed=" + EncodeQueryValue(feed);
            const auto body = HttpGet(endpoint.host, path, endpoint.https, headers);
            if (body && !cancelled()) {
                const auto batch_quotes = ParseAlpacaBatchResponse(*body, batch);
                for (size_t i = begin; i < end; ++i) quotes[indices[i]] = batch_quotes[i - begin];
            }
        }
    }
    return quotes;
}

std::optional<NetworkUtcTime> GetEstimatedNetworkUtcTime() {
    std::lock_guard lock(g_network_time_mutex);
    const unsigned long long file_time = g_network_utc_file_time;
    const unsigned long long tick_ms = g_network_utc_tick_ms;
    if (file_time == 0 || tick_ms == 0) {
        return std::nullopt;
    }

    const unsigned long long elapsed_ms = GetTickCount64() - tick_ms;
    const unsigned long long estimated_file_time = file_time + elapsed_ms * 10000ull;
    ULARGE_INTEGER combined{};
    combined.QuadPart = estimated_file_time;
    FILETIME estimated{};
    estimated.dwLowDateTime = combined.LowPart;
    estimated.dwHighDateTime = combined.HighPart;

    SYSTEMTIME utc_time{};
    if (!FileTimeToSystemTime(&estimated, &utc_time)) {
        return std::nullopt;
    }

    return NetworkUtcTime{utc_time.wYear,
                          utc_time.wMonth,
                          utc_time.wDay,
                          utc_time.wDayOfWeek,
                          utc_time.wHour,
                          utc_time.wMinute,
                          utc_time.wSecond};
}

}  // namespace stock_taskbar_monitor
