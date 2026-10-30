#include "../src/stock_config.h"
#include "../src/stock_fetcher.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string SinaBody(bool cn, const std::string& time, const std::string& last,
                     const std::string& bid, const std::string& ask,
                     const std::string& previous_close = "100") {
    std::vector<std::string> fields(cn ? 33 : 19, "0");
    fields[0] = "TEST";
    fields[cn ? 2 : 3] = previous_close;
    fields[cn ? 3 : 6] = last;
    fields[cn ? 6 : 9] = bid;
    fields[cn ? 7 : 10] = ask;
    if (!cn) {
        fields[8] = "-99";  // Must recompute the change for the selected auction price.
    }
    fields[cn ? 30 : 17] = cn ? "2026-10-26" : "2026/10/26";
    fields[cn ? 31 : 18] = time;
    std::string result = "var hq_str_test=\"";
    for (size_t index = 0; index < fields.size(); ++index) {
        result += (index == 0 ? "" : ",") + fields[index];
    }
    return result + "\";";
}

void TestQuotes() {
    struct Case {
        bool cn;
        const char* time;
        const char* last;
        const char* bid;
        const char* ask;
        double expected;
    };
    const Case cases[] = {
        {true, "09:14:59", "99", "102", "103", 99},
        {true, "09:15:00", "0", "102", "103", 102},
        {true, "09:20:00", "100", "103", "104", 103},
        {true, "09:24:59", "0", "0", "104", 104},
        {true, "09:25:00", "105", "99", "106", 105},
        {true, "09:30:00", "105", "104", "106", 105},
        {true, "14:56:59", "105", "104", "106", 105},
        {true, "14:57:00", "105", "106", "107", 106},
        {true, "14:59:59", "105", "107", "108", 107},
        {true, "15:00:00", "108", "107", "109", 108},
        {false, "08:59:59", "99", "102", "103", 99},
        {false, "09:00:00", "99", "102", "103", 102},
        {false, "09:19:59", "0", "103", "104", 103},
        {false, "09:19:59", "99", "0", "104", 104},
        {false, "09:20:00", "105", "99", "106", 105},
        {false, "09:30:00", "105", "104", "106", 105},
        {false, "16:00:59", "105", "99", "106", 105},
        {false, "16:01:00", "105", "106", "107", 106},
        {false, "16:09:59", "105", "107", "108", 107},
        {false, "16:10:00", "108", "107", "109", 108},
        {true, "09:20:00", "101", "0", "0", 101},
        {true, "09:20:00", "0", "0", "0", 100},
        {false, "09:10:00", "0", "0", "0", 100},
        {true, "09:20:00", "nan", "inf", "104", 104},
        {false, "bad-time", "105", "99", "106", 105},
    };
    for (const auto& test : cases) {
        stock_taskbar_monitor::StockTarget target;
        target.market = test.cn ? L"CN" : L"HK";
        const auto quote = stock_taskbar_monitor::ParseSinaResponse(
            SinaBody(test.cn, test.time, test.last, test.bid, test.ask), target);
        Require(quote.has_value(), "Expected a valid quote");
        if (std::abs(quote->price - test.expected) > 1e-9) {
            std::cerr << (test.cn ? "CN " : "HK ") << test.time << '\n';
            throw std::runtime_error("Wrong price at market phase boundary");
        }
        Require(quote->change_percent.has_value() &&
                    std::abs(*quote->change_percent - (test.expected - 100.0)) < 1e-9,
                "Change percentage must agree with displayed price");
    }
    const stock_taskbar_monitor::StockTarget target;
    for (const std::string body : {"", "no quote", "var q=\"\";", "var q=\"TEST,0\";"}) {
        Require(!stock_taskbar_monitor::ParseSinaResponse(body, target),
                "Malformed responses must be rejected");
    }
    Require(!stock_taskbar_monitor::ParseSinaResponse(
                SinaBody(false, "09:10:00", "0", "0", "0", "0"), target),
            "A quote with no positive price must be rejected");
    std::cout << "Quote parsing: 30 cases passed\n";
}

void TestConfig() {
    const std::filesystem::path path(stock_taskbar_monitor::GetConfigPath());
    Require(!std::filesystem::exists(path), "Test config directory must be isolated");
    for (unsigned int count : {2u, 4u, 6u, 8u, 10u, 12u}) {
        stock_taskbar_monitor::AppConfig config;
        stock_taskbar_monitor::StockTarget target;
        target.symbol = L"TEST";
        target.code = L"00700";
        config.stocks = {target};
        config.taskbar_symbol_count = count;
        Require(stock_taskbar_monitor::SaveConfig(config), "Cannot save test config");
        const auto loaded = stock_taskbar_monitor::LoadOrCreateConfig();
        Require(loaded.taskbar_symbol_count == (count == 12 ? 4 : count),
                "Stock display count did not survive saving/loading");
        std::filesystem::remove(path);
    }
    std::cout << "Config save/load: 2/4/6/8/10 and invalid-count fallback passed\n";
}

void TestBatchedQuotes() {
    using namespace stock_taskbar_monitor;
    StockTarget hk;
    hk.code = L"00700";
    StockTarget cn;
    cn.market = L"cn";
    cn.code = L"600000";
    StockTarget missing = hk;
    missing.code = L"09988";
    auto body_for = [](std::string body, const std::string& symbol) {
        body.replace(body.find("test"), 4, symbol);
        return body;
    };
    const auto body = body_for(SinaBody(true, "10:00:00", "107", "0", "0"), "sh600000") +
                      body_for(SinaBody(false, "10:00:00", "103", "0", "0"), "rt_hk00700");
    const auto quotes = ParseSinaBatchResponse(body, {hk, missing, cn, hk});
    Require(quotes.size() == 4 && quotes[0] && quotes[0]->price == 103 && !quotes[1] &&
            quotes[2] && quotes[2]->price == 107 && quotes[3] && quotes[3]->price == 103,
            "Batch responses must match symbols, not response order; missing symbols stay empty");

    StockTarget a, b, c, d;
    a.code = L"aapl"; b.code = L"MSFT"; c.code = L"MISSING"; d.code = L"INVALID";
    const auto alpaca = ParseAlpacaBatchResponse(
        R"({"MSFT":{"latestTrade":null,"dailyBar":{"c":210},"prevDailyBar":{"c":200}},
            "AAPL":{"latestTrade":{"p":105},"prevDailyBar":{"c":100}},
            "MISSING":null,"INVALID":{"latestTrade":{"p":1e999}}})", {a, b, c, d});
    Require(alpaca.size() == 4 && alpaca[0] && alpaca[0]->price == 105 &&
            alpaca[1] && alpaca[1]->price == 210 && !alpaca[2] && !alpaca[3],
            "Alpaca batch: reorder, lowercase symbol, null snapshot and nonfinite prices");
    Require(alpaca[1]->change_percent && std::abs(*alpaca[1]->change_percent - 5.0) < 1e-9,
            "Missing trade must use its own daily bar, not another JSON object");
    std::cout << "Batched quotes: symbol mapping, missing/null data and fallback passed\n";
}

}  // namespace

int main() {
    try {
        TestQuotes();
        TestBatchedQuotes();
        TestConfig();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
