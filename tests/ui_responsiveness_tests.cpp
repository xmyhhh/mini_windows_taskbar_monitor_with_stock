// Exercise the real controller/popup code with a slow data source and real Win32
// message dispatch. Keep test windows hidden and never attach them to Explorer.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

namespace test_windows {
BOOL Show(HWND window, int) { return ::ShowWindow(window, SW_HIDE); }
BOOL Position(HWND window, HWND after, int x, int y, int w, int h, UINT flags) {
    return ::SetWindowPos(window, after, x, y, w, h, flags & ~SWP_SHOWWINDOW);
}
BOOL Foreground(HWND) { return TRUE; }
HWND Activate(HWND) { return nullptr; }
HWND Focus(HWND) { return nullptr; }
}  // namespace test_windows

#define ShowWindow test_windows::Show
#define SetWindowPos test_windows::Position
#define SetForegroundWindow test_windows::Foreground
#define SetActiveWindow test_windows::Activate
#define SetFocus test_windows::Focus
#include "../src/main.cpp"
#undef ShowWindow
#undef SetWindowPos
#undef SetForegroundWindow
#undef SetActiveWindow
#undef SetFocus

using namespace std::chrono_literals;

namespace minimal_taskbar_monitor {
struct MonitorAppTestAccess {
    static void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    static LRESULT CALLBACK Controller(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* app = reinterpret_cast<MonitorApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (app && message == kStockSampleReadyMessage) {
            app->ReceiveStockSamples();
            return 0;
        }
        if (app && message == kWidgetRedrawMessage) {
            app->widget_redraw_pending_ = false;
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    template <typename Predicate>
    static bool PumpUntil(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (predicate()) return true;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    static void SavePreview(MonitorApp& app, const wchar_t* path) {
        ULONG_PTR token{};
        Gdiplus::GdiplusStartupInput input;
        Require(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok,
                "Preview graphics initialization failed");
        {
            app.UpdateHoverPopupSize();
            Gdiplus::Bitmap bitmap(app.hover_popup_size_.cx, app.hover_popup_size_.cy, PixelFormat32bppARGB);
            Gdiplus::Graphics graphics(&bitmap);
            HDC dc = graphics.GetHDC();
            RECT rect{0, 0, app.hover_popup_size_.cx, app.hover_popup_size_.cy};
            app.DrawStockHoverPopupContents(dc, rect);
            graphics.ReleaseHDC(dc);
            UINT count = 0, size = 0;
            Gdiplus::GetImageEncodersSize(&count, &size);
            std::vector<BYTE> storage(size);
            auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
            Gdiplus::GetImageEncoders(count, size, encoders);
            bool saved = false;
            for (UINT i = 0; i < count; ++i) {
                if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
                    saved = bitmap.Save(path, &encoders[i].Clsid, nullptr) == Gdiplus::Ok;
                }
            }
            Require(saved, "Could not save native popup preview");
        }
        Gdiplus::GdiplusShutdown(token);
    }

    static void Run(const wchar_t* preview_path) {
        MonitorApp app;
        app.instance_handle_ = GetModuleHandleW(nullptr);
        app.RegisterWindowClasses();
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = Controller;
        window_class.hInstance = app.instance_handle_;
        window_class.lpszClassName = L"StockResponsivenessTestController";
        RegisterClassW(&window_class);
        app.controller_window_ = CreateWindowExW(0, window_class.lpszClassName, L"", 0,
                                                 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                                 app.instance_handle_, nullptr);
        SetWindowLongPtrW(app.controller_window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));
        struct Cleanup {
            MonitorApp& app;
            ~Cleanup() {
                app.Shutdown();
                DestroyWindow(app.controller_window_);
            }
        } cleanup{app};
        Require(app.EnsureWidgetWindow(), "Could not create isolated widget");
        app.stock_mode_ = true;
        app.app_config_.popup_activation_mode = PopupActivationMode::kHover;
        app.stock_config_.popup_activation_mode = stock_taskbar_monitor::PopupActivationMode::kClick;
        stock_taskbar_monitor::StockTarget target;
        target.symbol = L"FIRST";
        target.code = L"00700";
        app.stock_config_.stock_groups = {{L"One", {target}}, {L"Two", {target}}};
        app.stock_config_.active_group = L"One";
        app.stock_config_.stocks = {target};
        app.stock_rows_ = app.BuildPlaceholderStockRows({target});
        app.RefreshFontAndSize();

        auto started = std::make_shared<std::promise<void>>();
        auto started_future = started->get_future();
        auto release = std::make_shared<std::promise<void>>();
        auto released = release->get_future().share();
        auto calls = std::make_shared<std::atomic<int>>(0);
        const HWND controller = app.controller_window_;
        app.stock_worker_.Start([started, released, calls](const StockSampleRequest& request, const auto&) {
            if (++*calls == 1) {
                started->set_value();
                released.wait_for(3s);
            }
            StockSampleResult result{request, {}};
            for (const auto& target : request.targets) {
                result.quotes.push_back(stock_taskbar_monitor::PriceQuote{target.symbol, 123.0, 2.0});
            }
            return result;
        }, [controller] { PostMessageW(controller, kStockSampleReadyMessage, 0, 0); });

        app.SampleStocks();
        Require(started_future.wait_for(2s) == std::future_status::ready, "Slow request did not start");
        const auto click_start = std::chrono::steady_clock::now();
        SendMessageW(app.widget_window_, WM_LBUTTONDOWN, 0, 0);
        SendMessageW(app.widget_window_, WM_LBUTTONUP, 0, 0);
        Require(app.hover_popup_visible_, "Click must open the stock popup during a pending request");
        Require(std::chrono::steady_clock::now() - click_start < 250ms,
                "Opening the popup blocked on the network");
        SendMessageW(app.hover_popup_window_, WM_KEYDOWN, VK_RIGHT, 0);
        Require(app.PopupStockGroupName() == L"Two", "Arrow key must switch groups while loading");
        SendMessageW(app.hover_popup_window_, WM_KEYDOWN, VK_ESCAPE, 0);
        Require(!app.hover_popup_visible_, "Escape must close the popup while loading");
        Require(app.stock_config_.active_group == L"One", "Popup tabs must not change taskbar group");

        // Reload invalidates a response already in flight, even with the same group name.
        target.symbol = L"RELOADED";
        target.code = L"09988";
        app.stock_config_.stock_groups[0].stocks = {target};
        app.stock_config_.stocks = {target};
        app.ResetStockSampling();
        app.SampleStocks();
        release->set_value();
        Require(PumpUntil([&] { return !app.stock_rows_.empty() && app.stock_rows_[0].price == 123.0; }),
                "New config result did not reach the UI");
        Require(app.stock_rows_[0].symbol == L"RELOADED" && app.stock_rows_[0].code == L"09988",
                "An old response overwrote the new config");
        Require(*calls == 2, "Duplicate popup/timer requests were not coalesced");

        app.ShowHoverPopup(true);
        SendMessageW(app.widget_window_, WM_LBUTTONDOWN, 0, 0);
        SendMessageW(app.widget_window_, WM_LBUTTONUP, 0, 0);
        Require(!app.hover_popup_visible_, "Second click must close the popup");
        const auto failure = app.BuildStockFailureRow(target, app.stock_rows_[0]);
        Require(failure.price == 123.0 && failure.stale, "Failure must preserve the last good price");
        if (preview_path) {
            app.app_config_.language = UiLanguage::kChinese;
            app.popup_stock_rows_ = {app.stock_rows_[0], failure, app.stock_rows_[0]};
            app.popup_stock_rows_[0].symbol = L"腾讯控股";
            app.popup_stock_rows_[1].symbol = L"阿里巴巴";
            app.popup_stock_rows_[2].symbol = L"贵州茅台";
            app.popup_stock_rows_[2].change_percent = -1.25;
            app.popup_stock_rows_[2].market = L"cn";
            SavePreview(app, preview_path);
        }
        std::cout << "Real UI dispatch: slow network clicks, tabs, Escape, reload isolation and cached failures passed\n";
    }
};
}  // namespace minimal_taskbar_monitor

int wmain(int argc, wchar_t** argv) {
    try {
        minimal_taskbar_monitor::MonitorAppTestAccess::Run(argc > 1 ? argv[1] : nullptr);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
