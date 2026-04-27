#include "app_config.h"
#include "process_monitor.h"
#include "resource.h"
#include "stock_config.h"
#include "stock_config_dialog.h"
#include "stock_fetcher.h"
#include "system_metrics.h"
#include "taskbar_embedder.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <set>
#include <vector>

namespace minimal_taskbar_monitor {

namespace {

constexpr wchar_t kControllerClassName[] = L"MinimalTaskbarMonitorControllerWindow";
constexpr wchar_t kWidgetClassName[] = L"MinimalTaskbarMonitorWidgetWindow";
constexpr wchar_t kHoverPopupClassName[] = L"MinimalTaskbarMonitorHoverPopupWindow";
constexpr UINT_PTR kSampleTimerId = 1;
constexpr UINT_PTR kLayoutTimerId = 2;
constexpr UINT_PTR kReattachTimerId = 3;
constexpr UINT_PTR kHoverHideTimerId = 4;
constexpr int kToggleModeHotkeyId = 1;
constexpr UINT kLayoutIntervalMs = 1000;
constexpr UINT kReattachDelayMs = 600;
constexpr UINT kHoverHideDelayMs = 180;
constexpr ULONGLONG kStockNotificationStartupSilenceMs = 10000;
constexpr UINT kTrayIconCallbackMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kExitCommandId = 1001;
constexpr UINT kAutoStartCommandId = 1002;
constexpr UINT kNetworkUnitsBitsCommandId = 1003;
constexpr UINT kNetworkUnitsBytesCommandId = 1004;
constexpr UINT kPopupModeHoverCommandId = 1005;
constexpr UINT kPopupModeClickCommandId = 1006;
constexpr UINT kSampleInterval1sCommandId = 1007;
constexpr UINT kSampleInterval2sCommandId = 1008;
constexpr UINT kSampleInterval5sCommandId = 1009;
constexpr UINT kSampleInterval10sCommandId = 1010;
constexpr UINT kModeStatusCommandId = 1011;
constexpr UINT kModeStockCommandId = 1012;
constexpr UINT kReloadStockConfigCommandId = 1013;
constexpr UINT kStockSymbols2CommandId = 1014;
constexpr UINT kStockSymbols4CommandId = 1015;
constexpr UINT kStockSymbols6CommandId = 1016;
constexpr UINT kStockSymbols8CommandId = 1017;
constexpr UINT kHelpCommandId = 1018;
constexpr UINT kHotkeyAltQCommandId = 1019;
constexpr UINT kHotkeyAltSCommandId = 1020;
constexpr UINT kHotkeyAltMCommandId = 1021;
constexpr UINT kHotkeyCtrlAltQCommandId = 1022;
constexpr UINT kStockSortConfigCommandId = 1023;
constexpr UINT kStockSortGainersCommandId = 1024;
constexpr UINT kStockSortLosersCommandId = 1025;
constexpr UINT kLanguageEnglishCommandId = 1026;
constexpr UINT kLanguageChineseCommandId = 1027;
constexpr UINT kOpenStockConfigDialogCommandId = 1028;
constexpr UINT kMetricCpuCommandId = 1101;
constexpr UINT kMetricMemoryCommandId = 1102;
constexpr UINT kMetricUploadCommandId = 1103;
constexpr UINT kMetricDownloadCommandId = 1104;
constexpr UINT kMetricGpuCommandId = 1105;
constexpr UINT kMetricDiskReadCommandId = 1106;
constexpr UINT kMetricDiskWriteCommandId = 1107;
constexpr wchar_t kRunRegistryPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"MinimalTaskbarMonitor";
constexpr int kHoverPopupMaxVisibleRows = 14;
constexpr int kHoverPopupWheelRows = 3;

bool IsSupportedSampleIntervalSeconds(unsigned int seconds) {
    return seconds == 1 || seconds == 2 || seconds == 5 || seconds == 10;
}

UINT GetSampleTimerIntervalMs(unsigned int seconds) {
    if (!IsSupportedSampleIntervalSeconds(seconds)) {
        seconds = 1;
    }
    return seconds * 1000u;
}

struct WidgetPalette {
    COLORREF background;
    COLORREF border;
    COLORREF primary_text;
    COLORREF secondary_text;
};

struct HoverPopupPalette {
    COLORREF background;
    COLORREF border;
    COLORREF title_text;
    COLORREF primary_text;
    COLORREF secondary_text;
    COLORREF warning_text;
    COLORREF danger_text;
    COLORREF accent;
    COLORREF header_fill;
    COLORREF row_highlight;
    COLORREF search_fill;
    COLORREF search_border;
    COLORREF search_active_border;
    COLORREF search_placeholder;
};

enum class HoverPopupSortMode {
    kDefault,
    kCpu,
    kMemory,
    kGpu,
    kVram,
    kIo,
    kNetwork
};

struct HoverPopupTableLayout {
    RECT search_rect{};
    RECT header_rect{};
    RECT list_rect{};
    RECT rank_rect{};
    RECT name_rect{};
    RECT cpu_rect{};
    RECT mem_rect{};
    RECT gpu_rect{};
    RECT vram_rect{};
    RECT io_rect{};
    RECT net_rect{};
};

struct BgraPixel {
    BYTE blue;
    BYTE green;
    BYTE red;
    BYTE alpha;
};

struct HotkeySpec {
    UINT modifiers;
    UINT virtual_key;
    const wchar_t* label;
};

HotkeySpec GetToggleHotkeySpec(ToggleHotkey hotkey) {
    switch (hotkey) {
    case ToggleHotkey::kAltS:
        return {MOD_ALT | MOD_NOREPEAT, 'S', L"Alt+S"};
    case ToggleHotkey::kAltM:
        return {MOD_ALT | MOD_NOREPEAT, 'M', L"Alt+M"};
    case ToggleHotkey::kCtrlAltQ:
        return {MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q', L"Ctrl+Alt+Q"};
    case ToggleHotkey::kAltQ:
    default:
        return {MOD_ALT | MOD_NOREPEAT, 'Q', L"Alt+Q"};
    }
}

int ScaleByDpi(UINT dpi, int value) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

WidgetPalette GetWidgetPalette(bool light_theme) {
    if (light_theme) {
        return {RGB(244, 246, 248), RGB(211, 216, 222), RGB(24, 28, 32), RGB(84, 91, 99)};
    }
    return {RGB(36, 39, 45), RGB(67, 72, 80), RGB(245, 247, 250), RGB(181, 188, 198)};
}

HoverPopupPalette GetHoverPopupPalette(bool light_theme) {
    if (light_theme) {
        return {RGB(251, 252, 254),
                RGB(212, 218, 226),
                RGB(17, 23, 31),
                RGB(36, 43, 51),
                RGB(102, 110, 120),
                RGB(182, 118, 0),
                RGB(196, 64, 52),
                RGB(55, 118, 206),
                RGB(240, 244, 249),
                RGB(247, 249, 252),
                RGB(214, 220, 228),
                RGB(55, 118, 206),
                RGB(132, 140, 150)};
    }

    return {RGB(28, 31, 36),
            RGB(68, 74, 83),
            RGB(246, 248, 250),
            RGB(223, 228, 233),
            RGB(152, 160, 171),
            RGB(255, 188, 84),
            RGB(255, 120, 120),
            RGB(126, 182, 255),
            RGB(38, 43, 50),
            RGB(33, 37, 43),
            RGB(72, 79, 89),
            RGB(126, 182, 255),
            RGB(131, 139, 149)};
}

enum class HoverAlertLevel {
    kNone,
    kWarning,
    kDanger
};

SIZE MeasureText(HDC dc, const std::wstring& text) {
    SIZE text_size{};
    if (!text.empty()) {
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &text_size);
    }
    return text_size;
}

std::vector<int> MeasureColumnWidths(HDC dc, const std::vector<DisplayLines::Column>& columns) {
    std::vector<int> column_widths;
    column_widths.reserve(columns.size());

    for (const auto& column : columns) {
        const SIZE top_size = MeasureText(dc, column.top_text);
        const SIZE bottom_size = MeasureText(dc, column.bottom_text);
        column_widths.push_back(std::max(top_size.cx, bottom_size.cx));
    }

    return column_widths;
}

int SumColumnWidths(const std::vector<int>& column_widths, int column_gap) {
    int total_width = 0;
    for (size_t i = 0; i < column_widths.size(); ++i) {
        if (i != 0) {
            total_width += column_gap;
        }
        total_width += column_widths[i];
    }
    return total_width;
}

std::wstring FormatRate(unsigned long long bytes_per_second) {
    double value = static_cast<double>(bytes_per_second);
    const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s"};
    size_t unit_index = 0;

    while (value >= 1024.0 && unit_index + 1 < _countof(units)) {
        value /= 1024.0;
        ++unit_index;
    }

    wchar_t buffer[64]{};
    if (value >= 100.0 || unit_index == 0) {
        swprintf_s(buffer, L"%.0f%ls", value, units[unit_index]);
    } else {
        swprintf_s(buffer, L"%.1f%ls", value, units[unit_index]);
    }
    return buffer;
}

std::wstring FormatNetworkRate(unsigned long long bytes_per_second,
                               NetworkDisplayUnit network_display_unit) {
    return FormatNetworkRateForDisplay(bytes_per_second, network_display_unit);
}

std::wstring FormatBytes(unsigned long long byte_count) {
    double value = static_cast<double>(byte_count);
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    size_t unit_index = 0;

    while (value >= 1024.0 && unit_index + 1 < _countof(units)) {
        value /= 1024.0;
        ++unit_index;
    }

    wchar_t buffer[64]{};
    if (value >= 100.0 || unit_index == 0) {
        swprintf_s(buffer, L"%.0f%ls", value, units[unit_index]);
    } else {
        swprintf_s(buffer, L"%.1f%ls", value, units[unit_index]);
    }
    return buffer;
}

std::wstring FormatPercentValue(double percent) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%.0f%%", std::max(percent, 0.0));
    return buffer;
}

std::wstring FormatSignedPercentValue(double percent) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%+.2f%%", percent);
    return buffer;
}

std::wstring FormatPriceValue(double value) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.2f", value);
    return buffer;
}

std::wstring FormatClockTime() {
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02u:%02u:%02u", local_time.wHour, local_time.wMinute, local_time.wSecond);
    return buffer;
}

std::wstring ToLowerCopy(const std::wstring& text) {
    std::wstring lower = text;
    std::transform(lower.begin(),
                   lower.end(),
                   lower.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return lower;
}

bool ContainsCaseInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }

    return ToLowerCopy(haystack).find(ToLowerCopy(needle)) != std::wstring::npos;
}

HoverAlertLevel GetUsageAlertLevel(double value, double warning_threshold, double danger_threshold) {
    if (value >= danger_threshold) {
        return HoverAlertLevel::kDanger;
    }
    if (value >= warning_threshold) {
        return HoverAlertLevel::kWarning;
    }
    return HoverAlertLevel::kNone;
}

COLORREF ResolveAlertColor(const HoverPopupPalette& palette,
                           HoverAlertLevel level,
                           COLORREF default_color) {
    switch (level) {
    case HoverAlertLevel::kDanger:
        return palette.danger_text;
    case HoverAlertLevel::kWarning:
        return palette.warning_text;
    case HoverAlertLevel::kNone:
    default:
        return default_color;
    }
}

std::wstring FormatUptime(ULONGLONG uptime_ms) {
    unsigned long long total_minutes = uptime_ms / 60000ULL;
    const unsigned long long days = total_minutes / (24ULL * 60ULL);
    total_minutes %= (24ULL * 60ULL);
    const unsigned long long hours = total_minutes / 60ULL;
    const unsigned long long minutes = total_minutes % 60ULL;

    wchar_t buffer[64]{};
    if (days > 0) {
        swprintf_s(buffer, L"%llud %02lluh %02llum", days, hours, minutes);
    } else {
        swprintf_s(buffer, L"%02lluh %02llum", hours, minutes);
    }
    return buffer;
}

bool IsPointInWindowRect(HWND window_handle, const POINT& screen_point) {
    if (window_handle == nullptr || !IsWindow(window_handle)) {
        return false;
    }

    RECT window_rect{};
    return GetWindowRect(window_handle, &window_rect) != FALSE &&
           PtInRect(&window_rect, screen_point) != FALSE;
}

bool IsNearlyWhite(const Gdiplus::Color& color) {
    return color.GetAlpha() > 0 && color.GetRed() >= 245 && color.GetGreen() >= 245 &&
           color.GetBlue() >= 245;
}

bool FindContentBounds(Gdiplus::Bitmap& bitmap, Gdiplus::Rect& bounds) {
    const UINT width = bitmap.GetWidth();
    const UINT height = bitmap.GetHeight();
    int min_x = static_cast<int>(width);
    int min_y = static_cast<int>(height);
    int max_x = -1;
    int max_y = -1;

    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            Gdiplus::Color color;
            if (bitmap.GetPixel(x, y, &color) != Gdiplus::Ok || IsNearlyWhite(color)) {
                continue;
            }

            min_x = std::min(min_x, static_cast<int>(x));
            min_y = std::min(min_y, static_cast<int>(y));
            max_x = std::max(max_x, static_cast<int>(x));
            max_y = std::max(max_y, static_cast<int>(y));
        }
    }

    if (max_x < min_x || max_y < min_y) {
        return false;
    }

    bounds = Gdiplus::Rect(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
    return true;
}

HFONT CreatePreferredUiFont(UINT dpi, int point_size) {
    const LOGFONTW font_template{
        .lfHeight = -ScaleByDpi(dpi, point_size),
        .lfWidth = 0,
        .lfEscapement = 0,
        .lfOrientation = 0,
        .lfWeight = FW_NORMAL,
        .lfItalic = FALSE,
        .lfUnderline = FALSE,
        .lfStrikeOut = FALSE,
        .lfCharSet = DEFAULT_CHARSET,
        .lfOutPrecision = OUT_DEFAULT_PRECIS,
        .lfClipPrecision = CLIP_DEFAULT_PRECIS,
        .lfQuality = CLEARTYPE_NATURAL_QUALITY,
        .lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE,
    };

    const std::array<const wchar_t*, 4> preferred_faces = {
        L"Segoe UI Variable Text", L"Bahnschrift", L"Microsoft YaHei UI", L"Segoe UI"};

    HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) {
        return CreateFontW(font_template.lfHeight,
                           0,
                           0,
                           0,
                           FW_NORMAL,
                           FALSE,
                           FALSE,
                           FALSE,
                           DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_NATURAL_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE,
                           L"Segoe UI");
    }

    for (const wchar_t* face_name : preferred_faces) {
        LOGFONTW requested_font = font_template;
        wcscpy_s(requested_font.lfFaceName, face_name);

        HFONT font_handle = CreateFontIndirectW(&requested_font);
        if (font_handle == nullptr) {
            continue;
        }

        HGDIOBJ previous_font = SelectObject(screen_dc, font_handle);
        wchar_t selected_face[LF_FACESIZE]{};
        const int face_length = GetTextFaceW(screen_dc, LF_FACESIZE, selected_face);
        SelectObject(screen_dc, previous_font);

        if (face_length > 0 && _wcsicmp(selected_face, face_name) == 0) {
            ReleaseDC(nullptr, screen_dc);
            return font_handle;
        }

        DeleteObject(font_handle);
    }

    ReleaseDC(nullptr, screen_dc);
    return CreateFontW(font_template.lfHeight,
                       0,
                       0,
                       0,
                       FW_NORMAL,
                       FALSE,
                       FALSE,
                       FALSE,
                       DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_NATURAL_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");
}

int CountVisibleMetrics(const MetricVisibility& visibility) {
    return static_cast<int>(visibility.show_cpu) + static_cast<int>(visibility.show_memory) +
           static_cast<int>(visibility.show_upload) +
           static_cast<int>(visibility.show_download) + static_cast<int>(visibility.show_gpu) +
           static_cast<int>(visibility.show_disk_read) +
           static_cast<int>(visibility.show_disk_write);
}

std::wstring GetExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return L"";
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring GetAutoStartCommand() {
    const std::wstring executable_path = GetExecutablePath();
    if (executable_path.empty()) {
        return L"";
    }
    return L"\"" + executable_path + L"\"";
}

bool QueryAutoStartValue(std::wstring& value) {
    DWORD bytes = 0;
    LONG result =
        RegGetValueW(HKEY_CURRENT_USER, kRunRegistryPath, kRunValueName, RRF_RT_REG_SZ, nullptr,
                     nullptr, &bytes);
    if (result != ERROR_SUCCESS || bytes == 0) {
        value.clear();
        return false;
    }

    value.resize(bytes / sizeof(wchar_t));
    result = RegGetValueW(HKEY_CURRENT_USER,
                          kRunRegistryPath,
                          kRunValueName,
                          RRF_RT_REG_SZ,
                          nullptr,
                          value.data(),
                          &bytes);
    if (result != ERROR_SUCCESS) {
        value.clear();
        return false;
    }

    if (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return true;
}

bool IsAutoStartEnabled() {
    std::wstring stored_value;
    if (!QueryAutoStartValue(stored_value)) {
        return false;
    }

    const std::wstring current_command = GetAutoStartCommand();
    if (current_command.empty()) {
        return !stored_value.empty();
    }

    return CompareStringOrdinal(stored_value.c_str(), -1, current_command.c_str(), -1, TRUE) ==
           CSTR_EQUAL;
}

bool SetAutoStartEnabled(bool enabled) {
    HKEY run_key = nullptr;
    const LONG open_result = RegCreateKeyExW(HKEY_CURRENT_USER,
                                             kRunRegistryPath,
                                             0,
                                             nullptr,
                                             0,
                                             KEY_SET_VALUE,
                                             nullptr,
                                             &run_key,
                                             nullptr);
    if (open_result != ERROR_SUCCESS) {
        return false;
    }

    bool success = false;
    if (enabled) {
        const std::wstring command = GetAutoStartCommand();
        if (!command.empty()) {
            const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
            success = RegSetValueExW(run_key,
                                     kRunValueName,
                                     0,
                                     REG_SZ,
                                     reinterpret_cast<const BYTE*>(command.c_str()),
                                     bytes) == ERROR_SUCCESS;
        }
    } else {
        const LONG delete_result = RegDeleteValueW(run_key, kRunValueName);
        success = delete_result == ERROR_SUCCESS || delete_result == ERROR_FILE_NOT_FOUND;
    }

    RegCloseKey(run_key);
    return success;
}

}  // namespace

struct StockRow {
    std::wstring symbol;
    std::wstring price_text;
    std::wstring taskbar_price_text;
    std::wstring market;
    double price{0.0};
    std::optional<double> change_percent;
};

class MonitorApp {
public:
    int Run(HINSTANCE instance_handle, int) {
        instance_handle_ = instance_handle;
        taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
        app_config_ = LoadAppConfig();
        SaveAppConfig(app_config_);

        RegisterWindowClasses();

        controller_window_ = CreateWindowExW(0,
                                             kControllerClassName,
                                             L"Minimal Taskbar Monitor Controller",
                                             WS_OVERLAPPEDWINDOW,
                                             CW_USEDEFAULT,
                                             CW_USEDEFAULT,
                                             CW_USEDEFAULT,
                                             CW_USEDEFAULT,
                                             nullptr,
                                             nullptr,
                                             instance_handle_,
                                             this);
        if (controller_window_ == nullptr) {
            return 1;
        }

        ShowWindow(controller_window_, SW_HIDE);
        UpdateWindow(controller_window_);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        return static_cast<int>(message.wParam);
    }

private:
    void RegisterWindowClasses() const {
        HICON app_icon = LoadIconW(instance_handle_, MAKEINTRESOURCEW(IDI_APP_ICON));

        WNDCLASSEXW controller_class{};
        controller_class.cbSize = sizeof(controller_class);
        controller_class.lpfnWndProc = &MonitorApp::ControllerWindowProc;
        controller_class.hInstance = instance_handle_;
        controller_class.lpszClassName = kControllerClassName;
        controller_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        controller_class.hIcon = app_icon;
        controller_class.hIconSm = app_icon;
        RegisterClassExW(&controller_class);

        WNDCLASSEXW widget_class{};
        widget_class.cbSize = sizeof(widget_class);
        widget_class.lpfnWndProc = &MonitorApp::WidgetWindowProc;
        widget_class.hInstance = instance_handle_;
        widget_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        widget_class.hbrBackground = nullptr;
        widget_class.lpszClassName = kWidgetClassName;
        widget_class.hIcon = app_icon;
        widget_class.hIconSm = app_icon;
        RegisterClassExW(&widget_class);

        WNDCLASSEXW popup_class{};
        popup_class.cbSize = sizeof(popup_class);
        popup_class.lpfnWndProc = &MonitorApp::HoverPopupWindowProc;
        popup_class.hInstance = instance_handle_;
        popup_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        popup_class.hbrBackground = nullptr;
        popup_class.lpszClassName = kHoverPopupClassName;
        popup_class.hIcon = app_icon;
        popup_class.hIconSm = app_icon;
        RegisterClassExW(&popup_class);
    }

    bool Initialize() {
        stock_config_ = stock_taskbar_monitor::LoadOrCreateConfig();
        last_snapshot_ = metrics_.Sample();
        UpdateDisplayLines(last_snapshot_);
        if (!EnsureWidgetWindow()) {
            return false;
        }
        RefreshFontAndSize();

        ReattachWidget();
        EnsureTrayIcon();

        SetTimer(controller_window_,
                 kSampleTimerId,
                 GetSampleTimerIntervalMs(app_config_.sample_interval_seconds),
                 nullptr);
        SetTimer(controller_window_, kLayoutTimerId, kLayoutIntervalMs, nullptr);
        RegisterToggleHotkey();
        return true;
    }

    bool EnsureWidgetWindow() {
        if (widget_window_ != nullptr && IsWindow(widget_window_)) {
            return true;
        }

        widget_window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                         kWidgetClassName,
                                         L"Minimal Taskbar Monitor",
                                         WS_POPUP,
                                         0,
                                         0,
                                         0,
                                         0,
                                         nullptr,
                                         nullptr,
                                         instance_handle_,
                                         this);
        return widget_window_ != nullptr;
    }

    bool EnsureHoverPopupWindow() {
        if (hover_popup_window_ != nullptr && IsWindow(hover_popup_window_)) {
            return true;
        }

        hover_popup_window_ =
            CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                            kHoverPopupClassName,
                            L"Minimal Taskbar Monitor Popup",
                            WS_POPUP | WS_VSCROLL,
                            0,
                            0,
                            0,
                            0,
                            nullptr,
                            nullptr,
                            instance_handle_,
                            this);
        return hover_popup_window_ != nullptr;
    }

    bool EnsureTrayIcon() {
        if (tray_icon_added_ || controller_window_ == nullptr || !IsWindow(controller_window_)) {
            return tray_icon_added_;
        }

        if (tray_icon_handle_ == nullptr) {
            tray_icon_handle_ = LoadTrayIcon();
        }

        NOTIFYICONDATAW notify_icon{};
        notify_icon.cbSize = sizeof(notify_icon);
        notify_icon.hWnd = controller_window_;
        notify_icon.uID = kTrayIconId;
        notify_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        notify_icon.uCallbackMessage = kTrayIconCallbackMessage;
        notify_icon.hIcon = tray_icon_handle_;
        wcscpy_s(notify_icon.szTip, AppTitle());

        tray_icon_added_ = Shell_NotifyIconW(NIM_ADD, &notify_icon) != FALSE;
        return tray_icon_added_;
    }

    void UpdateTrayIconTip() {
        if (!tray_icon_added_ || controller_window_ == nullptr || !IsWindow(controller_window_)) {
            return;
        }

        NOTIFYICONDATAW notify_icon{};
        notify_icon.cbSize = sizeof(notify_icon);
        notify_icon.hWnd = controller_window_;
        notify_icon.uID = kTrayIconId;
        notify_icon.uFlags = NIF_TIP;
        wcscpy_s(notify_icon.szTip, AppTitle());
        Shell_NotifyIconW(NIM_MODIFY, &notify_icon);
    }

    void ShowTrayBalloon(const std::wstring& title, const std::wstring& message) {
        if (!tray_icon_added_ && !EnsureTrayIcon()) {
            return;
        }
        if (GetTickCount64() - app_start_tick_ms_ < kStockNotificationStartupSilenceMs) {
            return;
        }

        NOTIFYICONDATAW notify_icon{};
        notify_icon.cbSize = sizeof(notify_icon);
        notify_icon.hWnd = controller_window_;
        notify_icon.uID = kTrayIconId;
        notify_icon.uFlags = NIF_INFO;
        notify_icon.dwInfoFlags = NIIF_WARNING | NIIF_NOSOUND;
        wcsncpy_s(notify_icon.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(notify_icon.szInfo, message.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &notify_icon);
    }

    void RemoveTrayIcon() {
        if (!tray_icon_added_) {
            return;
        }

        NOTIFYICONDATAW notify_icon{};
        notify_icon.cbSize = sizeof(notify_icon);
        notify_icon.hWnd = controller_window_;
        notify_icon.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &notify_icon);
        tray_icon_added_ = false;
    }

    void Shutdown() {
        is_shutting_down_ = true;
        KillTimer(controller_window_, kSampleTimerId);
        KillTimer(controller_window_, kLayoutTimerId);
        KillTimer(controller_window_, kReattachTimerId);
        KillTimer(controller_window_, kHoverHideTimerId);
        UnregisterHotKey(controller_window_, kToggleModeHotkeyId);
        HideHoverPopup();
        RemoveTrayIcon();

        embedder_.Detach(widget_window_);

        if (tray_icon_handle_ != nullptr) {
            DestroyIcon(tray_icon_handle_);
            tray_icon_handle_ = nullptr;
        }

        ShutdownGdiplus();

        if (font_ != nullptr) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        if (popup_font_ != nullptr) {
            DeleteObject(popup_font_);
            popup_font_ = nullptr;
        }
        if (popup_title_font_ != nullptr) {
            DeleteObject(popup_title_font_);
            popup_title_font_ = nullptr;
        }

        if (widget_window_ != nullptr && IsWindow(widget_window_)) {
            DestroyWindow(widget_window_);
            widget_window_ = nullptr;
        }
        if (hover_popup_window_ != nullptr && IsWindow(hover_popup_window_)) {
            DestroyWindow(hover_popup_window_);
            hover_popup_window_ = nullptr;
        }
    }

    void ReattachWidget() {
        HideHoverPopup();
        if (!EnsureWidgetWindow()) {
            SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
            return;
        }

        embedder_.Detach(widget_window_);
        if (!embedder_.Attach(widget_window_)) {
            ShowWindow(widget_window_, SW_HIDE);
            SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
            return;
        }

        RefreshFontAndSize();
        if (!embedder_.RefreshLayout(widget_window_, widget_size_)) {
            ShowWindow(widget_window_, SW_HIDE);
            SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
            return;
        }
        ShowWindow(widget_window_, SW_SHOWNOACTIVATE);
        RequestWidgetRedraw();
    }

    void RefreshFontAndSize() {
        const UINT dpi = embedder_.CurrentDpi();
        if (dpi != current_dpi_ || font_ == nullptr || popup_font_ == nullptr ||
            popup_title_font_ == nullptr) {
            current_dpi_ = dpi;
            if (font_ != nullptr) {
                DeleteObject(font_);
                font_ = nullptr;
            }
            if (popup_font_ != nullptr) {
                DeleteObject(popup_font_);
                popup_font_ = nullptr;
            }
            if (popup_title_font_ != nullptr) {
                DeleteObject(popup_title_font_);
                popup_title_font_ = nullptr;
            }

            font_ = CreatePreferredUiFont(current_dpi_, 12);
            popup_font_ = CreatePreferredUiFont(current_dpi_, 13);
            popup_title_font_ = CreatePreferredUiFont(current_dpi_, 15);
        }

        HDC screen_dc = GetDC(nullptr);
        HFONT active_font =
            font_ != nullptr ? font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(screen_dc, active_font);
        const DisplayLines sample_lines =
            GetMetricsSampleLines(app_config_.visible_metrics,
                                  app_config_.network_display_unit,
                                  IsChinese());
        const int column_gap = ScaleByDpi(current_dpi_, 8);
        column_widths_ = MeasureColumnWidths(screen_dc, sample_lines.columns);
        TEXTMETRICW text_metrics{};
        GetTextMetricsW(screen_dc, &text_metrics);

        HFONT popup_font =
            popup_font_ != nullptr ? popup_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SelectObject(screen_dc, popup_font);
        TEXTMETRICW popup_text_metrics{};
        GetTextMetricsW(screen_dc, &popup_text_metrics);

        HFONT popup_title_font =
            popup_title_font_ != nullptr ? popup_title_font_
                                         : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SelectObject(screen_dc, popup_title_font);
        TEXTMETRICW popup_title_metrics{};
        GetTextMetricsW(screen_dc, &popup_title_metrics);

        if (stock_mode_) {
            SelectObject(screen_dc, active_font);
            const SIZE line1_size = MeasureText(screen_dc, line1_text_);
            const SIZE line2_size = MeasureText(screen_dc, line2_text_);
            SelectObject(screen_dc, old_font);
            ReleaseDC(nullptr, screen_dc);

            text_line_height_ = text_metrics.tmHeight;
            popup_text_line_height_ = popup_text_metrics.tmHeight;
            popup_title_line_height_ = popup_title_metrics.tmHeight;
            const int horizontal_padding = ScaleByDpi(current_dpi_, 8);
            const int vertical_padding = ScaleByDpi(current_dpi_, 5);
            const int line_gap = ScaleByDpi(current_dpi_, 2);
            const bool has_second_line = !line2_text_.empty();
            const int content_width = std::max<int>(line1_size.cx, line2_size.cx);
            widget_size_.cx =
                std::max<int>(ScaleByDpi(current_dpi_, 140), content_width + horizontal_padding * 2);
            widget_size_.cy =
                std::max<int>((has_second_line ? text_line_height_ * 2 + line_gap
                                                : text_line_height_) +
                                  vertical_padding * 2,
                              ScaleByDpi(current_dpi_, has_second_line ? 32 : 24));
            UpdateHoverPopupSize();
            return;
        }

        SelectObject(screen_dc, old_font);
        ReleaseDC(nullptr, screen_dc);

        text_line_height_ = text_metrics.tmHeight;
        popup_text_line_height_ = popup_text_metrics.tmHeight;
        popup_title_line_height_ = popup_title_metrics.tmHeight;
        const int horizontal_padding = ScaleByDpi(current_dpi_, 8);
        const int vertical_padding = ScaleByDpi(current_dpi_, 5);
        const int line_gap = ScaleByDpi(current_dpi_, 2);
        const bool has_second_line = !sample_lines.line2.empty();
        const int content_width = SumColumnWidths(column_widths_, column_gap);

        widget_size_.cx = content_width + horizontal_padding * 2;
        widget_size_.cy =
            std::max<int>((has_second_line ? text_line_height_ * 2 + line_gap : text_line_height_) +
                              vertical_padding * 2,
                         ScaleByDpi(current_dpi_, has_second_line ? 32 : 24));

        UpdateHoverPopupSize();
    }

    void UpdateDisplayLines(const MetricsSnapshot& snapshot) {
        const DisplayLines lines = FormatMetricsLines(
            snapshot,
            app_config_.visible_metrics,
            app_config_.network_display_unit,
            IsChinese());
        line1_text_ = lines.line1;
        line2_text_ = lines.line2;
        display_columns_ = lines.columns;
        has_second_line_ = !line2_text_.empty();
    }

    void UpdateStockDisplayLines() {
        display_columns_.clear();
        line1_text_.clear();
        line2_text_.clear();

        if (stock_rows_.empty()) {
            line1_text_ = Text(L"Stocks", L"股票");
            line2_text_ = Text(L"loading", L"加载中");
            has_second_line_ = true;
            return;
        }

        const size_t max_symbols =
            std::min<size_t>(stock_rows_.size(),
                             static_cast<size_t>(stock_config_.taskbar_symbol_count));
        const size_t first_line_count =
            std::min<size_t>(max_symbols, max_symbols <= 2 ? max_symbols : max_symbols / 2);
        for (size_t i = 0; i < max_symbols; ++i) {
            std::wstring& target_line = i < first_line_count ? line1_text_ : line2_text_;
            if (!target_line.empty()) {
                target_line += L"  ";
            }
            target_line += stock_rows_[i].symbol + L" " + stock_rows_[i].taskbar_price_text;
        }
        has_second_line_ = !line2_text_.empty();
    }

    void EvaluateStockPriceNotification(const stock_taskbar_monitor::StockTarget& target,
                                        double price) {
        std::wstring alert_key;
        std::wstring message;
        if (target.min_price && price < *target.min_price) {
            alert_key = target.symbol + L":below";
            message = target.symbol + L" " + FormatPriceValue(price) +
                      Text(L" is below ", L" 低于 ") + FormatPriceValue(*target.min_price);
        } else if (target.max_price && price > *target.max_price) {
            alert_key = target.symbol + L":above";
            message = target.symbol + L" " + FormatPriceValue(price) +
                      Text(L" is above ", L" 高于 ") + FormatPriceValue(*target.max_price);
        } else {
            stock_active_alerts_.erase(target.symbol + L":below");
            stock_active_alerts_.erase(target.symbol + L":above");
            return;
        }

        if (GetTickCount64() - app_start_tick_ms_ < kStockNotificationStartupSilenceMs) {
            return;
        }
        if (stock_active_alerts_.insert(alert_key).second) {
            ShowTrayBalloon(Text(L"Stock Price Alert", L"股票价格提醒"), message);
        }
    }

    void SampleStocks() {
        stock_rows_.clear();
        for (const auto& target : stock_config_.stocks) {
            StockRow row;
            row.symbol = target.symbol;
            row.market = target.market;
            const auto quote = stock_taskbar_monitor::FetchRealtimePrice(target, stock_config_);
            if (!quote) {
                row.price_text = L"(null)";
                row.taskbar_price_text = row.price_text;
                stock_rows_.push_back(row);
                continue;
            }

            row.price = quote->price;
            EvaluateStockPriceNotification(target, quote->price);
            row.change_percent = quote->change_percent;
            row.taskbar_price_text = FormatPriceValue(quote->price);
            row.price_text = row.taskbar_price_text;
            if (target.show_usd && _wcsicmp(target.market.c_str(), L"hk") == 0 &&
                stock_config_.usd_hkd_rate > 0.0) {
                row.price_text += L"/" +
                                  FormatPriceValue((quote->price / stock_config_.usd_hkd_rate) *
                                                   target.adr_factor);
            }
            stock_rows_.push_back(row);
        }
        SortStockRows();
        stock_last_update_time_ = FormatClockTime();
        UpdateStockDisplayLines();
    }

    void SortStockRows() {
        if (stock_config_.sort_mode == stock_taskbar_monitor::StockSortMode::kConfigOrder) {
            return;
        }

        const bool top_gainers =
            stock_config_.sort_mode == stock_taskbar_monitor::StockSortMode::kTopGainers;
        std::stable_sort(stock_rows_.begin(), stock_rows_.end(), [top_gainers](const StockRow& a,
                                                                               const StockRow& b) {
            if (a.change_percent && !b.change_percent) {
                return true;
            }
            if (!a.change_percent && b.change_percent) {
                return false;
            }
            if (!a.change_percent && !b.change_percent) {
                return false;
            }
            return top_gainers ? *a.change_percent > *b.change_percent
                               : *a.change_percent < *b.change_percent;
        });
    }

    void DrawWidgetContents(HDC dc, const RECT& client_rect) {
        const bool light_theme = IsLightTaskbarTheme();
        const WidgetPalette palette = GetWidgetPalette(light_theme);
        HBRUSH background_brush = CreateSolidBrush(palette.background);
        FillRect(dc, &client_rect, background_brush);
        DeleteObject(background_brush);
        HBRUSH border_brush = CreateSolidBrush(palette.border);
        FrameRect(dc, &client_rect, border_brush);
        DeleteObject(border_brush);

        SetBkMode(dc, TRANSPARENT);
        HFONT active_font =
            font_ != nullptr ? font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(dc, active_font);
        const int horizontal_padding = ScaleByDpi(current_dpi_, 8);
        const int vertical_padding = ScaleByDpi(current_dpi_, 4);
        const int line_gap = ScaleByDpi(current_dpi_, 2);
        const int column_gap = ScaleByDpi(current_dpi_, 8);
        const int centered_offset = std::max<int>(
            0, ((client_rect.bottom - client_rect.top) - text_line_height_) / 2);
        const int line1_top =
            has_second_line_ ? client_rect.top + vertical_padding : client_rect.top + centered_offset;

        RECT line1_rect{client_rect.left + horizontal_padding,
                        line1_top,
                        client_rect.right - horizontal_padding,
                        line1_top + text_line_height_};
        RECT line2_rect{client_rect.left + horizontal_padding,
                        client_rect.top + vertical_padding + text_line_height_ + line_gap,
                        client_rect.right - horizontal_padding,
                        client_rect.top + vertical_padding + text_line_height_ * 2 + line_gap};

        if (!display_columns_.empty()) {
            std::vector<int> active_column_widths = column_widths_;
            if (active_column_widths.size() != display_columns_.size()) {
                active_column_widths = MeasureColumnWidths(dc, display_columns_);
            }

            int current_x = client_rect.left + horizontal_padding;
            for (size_t i = 0; i < display_columns_.size(); ++i) {
                const int column_width = active_column_widths[i];
                RECT column_line1_rect{current_x,
                                       line1_rect.top,
                                       current_x + column_width,
                                       line1_rect.bottom};
                RECT column_line2_rect{current_x,
                                       line2_rect.top,
                                       current_x + column_width,
                                       line2_rect.bottom};

                if (!display_columns_[i].top_text.empty()) {
                    SetTextColor(dc, palette.primary_text);
                    DrawTextW(dc,
                              display_columns_[i].top_text.c_str(),
                              static_cast<int>(display_columns_[i].top_text.size()),
                              &column_line1_rect,
                              DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
                }

                if (has_second_line_ && !display_columns_[i].bottom_text.empty()) {
                    SetTextColor(dc, palette.secondary_text);
                    DrawTextW(dc,
                              display_columns_[i].bottom_text.c_str(),
                              static_cast<int>(display_columns_[i].bottom_text.size()),
                              &column_line2_rect,
                              DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
                }

                current_x += column_width + column_gap;
            }
        } else {
            SetTextColor(dc, palette.primary_text);
            DrawTextW(dc,
                      line1_text_.c_str(),
                      static_cast<int>(line1_text_.size()),
                      &line1_rect,
                      DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

            if (has_second_line_) {
                SetTextColor(dc, palette.secondary_text);
                DrawTextW(dc,
                          line2_text_.c_str(),
                          static_cast<int>(line2_text_.size()),
                          &line2_rect,
                          DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
        }

        SelectObject(dc, old_font);
    }

    int GetHoverPopupRowHeight() const {
        return popup_text_line_height_ + ScaleByDpi(current_dpi_, 8);
    }

    int GetHoverPopupSearchHeight() const {
        return popup_text_line_height_ + ScaleByDpi(current_dpi_, 10);
    }

    int GetHoverPopupVisibleRowCount() const {
        return std::max(1,
                        std::min(kHoverPopupMaxVisibleRows,
                                 static_cast<int>(hover_popup_snapshot_.top_processes.size())));
    }

    int GetHoverPopupMaxScrollOffset() const {
        return std::max(0,
                        static_cast<int>(hover_popup_snapshot_.top_processes.size()) -
                            GetHoverPopupVisibleRowCount());
    }

    void ClampHoverPopupScrollOffset() {
        hover_popup_scroll_offset_ =
            std::clamp(hover_popup_scroll_offset_, 0, GetHoverPopupMaxScrollOffset());
    }

    void UpdateHoverPopupScrollBar() {
        if (hover_popup_window_ == nullptr || !IsWindow(hover_popup_window_)) {
            return;
        }

        ClampHoverPopupScrollOffset();
        SCROLLINFO scroll_info{};
        scroll_info.cbSize = sizeof(scroll_info);
        scroll_info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        scroll_info.nMin = 0;
        scroll_info.nMax =
            std::max(0, static_cast<int>(hover_popup_snapshot_.top_processes.size()) - 1);
        scroll_info.nPage = static_cast<UINT>(GetHoverPopupVisibleRowCount());
        scroll_info.nPos = hover_popup_scroll_offset_;
        SetScrollInfo(hover_popup_window_, SB_VERT, &scroll_info, TRUE);
        ShowScrollBar(hover_popup_window_,
                      SB_VERT,
                      hover_popup_snapshot_.top_processes.size() >
                          static_cast<size_t>(GetHoverPopupVisibleRowCount()));
    }

    void SetHoverPopupScrollOffset(int offset) {
        hover_popup_scroll_offset_ = offset;
        ClampHoverPopupScrollOffset();
        UpdateHoverPopupScrollBar();
        RequestHoverPopupRedraw();
    }

    void ScrollHoverPopupBy(int delta_rows) {
        if (delta_rows == 0) {
            return;
        }
        SetHoverPopupScrollOffset(hover_popup_scroll_offset_ + delta_rows);
    }

    void RefreshHoverPopupView(bool reposition = true) {
        ApplyHoverPopupSort();
        UpdateHoverPopupSize();
        UpdateHoverPopupScrollBar();
        if (reposition) {
            PositionHoverPopup();
        }
        RequestHoverPopupRedraw();
    }

    HoverPopupTableLayout ComputeHoverPopupTableLayout(const RECT& client_rect) const {
        const int padding_left = ScaleByDpi(current_dpi_, 16);
        const int padding_right = ScaleByDpi(current_dpi_, 30);
        const int gap = ScaleByDpi(current_dpi_, 8);
        const int line_gap = ScaleByDpi(current_dpi_, 4);
        const int inner_padding = ScaleByDpi(current_dpi_, 6);
        const int row_gap = ScaleByDpi(current_dpi_, 6);
        const int scroll_bar_width = GetSystemMetrics(SM_CXVSCROLL);
        const int content_left = client_rect.left + padding_left;
        const int content_right = client_rect.right - padding_right - scroll_bar_width;
        const int content_top =
            client_rect.top + padding_left + popup_title_line_height_ + gap;
        const int summary_bottom = content_top + popup_text_line_height_ * 3 + line_gap * 2;
        const int search_height = GetHoverPopupSearchHeight();
        const int row_height = GetHoverPopupRowHeight();
        const int visible_rows = GetHoverPopupVisibleRowCount();
        const int search_top = summary_bottom + gap;

        HoverPopupTableLayout layout{};
        layout.search_rect = {content_left, search_top, content_right, search_top + search_height};
        const int table_top = layout.search_rect.bottom + gap;
        layout.header_rect = {content_left,
                              table_top,
                              content_right,
                              table_top + popup_text_line_height_ + ScaleByDpi(current_dpi_, 8)};
        layout.list_rect = {content_left,
                            layout.header_rect.bottom + row_gap,
                            content_right,
                            layout.header_rect.bottom + row_gap + visible_rows * row_height};

        const int rank_width = ScaleByDpi(current_dpi_, 24);
        const int cpu_width = ScaleByDpi(current_dpi_, 56);
        const int mem_width = ScaleByDpi(current_dpi_, 190);
        const int gpu_width = ScaleByDpi(current_dpi_, 54);
        const int vram_width = ScaleByDpi(current_dpi_, 82);
        const int io_width = ScaleByDpi(current_dpi_, 92);
        const int net_width = ScaleByDpi(current_dpi_, 92);
        const int table_gap = ScaleByDpi(current_dpi_, 10);
        const int table_left = layout.header_rect.left + inner_padding;
        const int table_right =
            std::max(table_left, static_cast<int>(layout.header_rect.right) - inner_padding);
        const int available_width = table_right - table_left;
        const int fixed_width = rank_width + cpu_width + mem_width + gpu_width + vram_width +
                                io_width + net_width + table_gap * 6;
        const int name_width = std::max(0, available_width - fixed_width);

        int x = table_left;
        layout.rank_rect = {x, layout.header_rect.top, x + rank_width, layout.header_rect.bottom};
        x = layout.rank_rect.right + table_gap;
        layout.name_rect = {x, layout.header_rect.top, x + name_width, layout.header_rect.bottom};
        x = layout.name_rect.right + table_gap;
        layout.cpu_rect = {x, layout.header_rect.top, x + cpu_width, layout.header_rect.bottom};
        x = layout.cpu_rect.right + table_gap;
        layout.mem_rect = {x, layout.header_rect.top, x + mem_width, layout.header_rect.bottom};
        x = layout.mem_rect.right + table_gap;
        layout.gpu_rect = {x, layout.header_rect.top, x + gpu_width, layout.header_rect.bottom};
        x = layout.gpu_rect.right + table_gap;
        layout.vram_rect = {x, layout.header_rect.top, x + vram_width, layout.header_rect.bottom};
        x = layout.vram_rect.right + table_gap;
        layout.io_rect = {x, layout.header_rect.top, x + io_width, layout.header_rect.bottom};
        x = layout.io_rect.right + table_gap;
        layout.net_rect = {x, layout.header_rect.top, x + net_width, layout.header_rect.bottom};
        return layout;
    }

    std::wstring GetHoverPopupHeaderText(const wchar_t* label, HoverPopupSortMode mode) const {
        std::wstring text = label;
        if (hover_popup_sort_mode_ == mode && mode != HoverPopupSortMode::kDefault) {
            text += L" ↓";
        }
        return text;
    }

    std::wstring GetHoverPopupSubtitle() const {
        const size_t visible_count = hover_popup_snapshot_.top_processes.size();
        const int total_count = hover_popup_base_snapshot_.total_process_count;
        const std::wstring count_suffix =
            hover_popup_search_text_.empty()
                ? (total_count > 0 ? L"  (" + std::to_wstring(total_count) +
                                          std::wstring(Text(L" shown)", L" 个)"))
                                   : L"")
                : (L"  (" + std::to_wstring(visible_count) + L"/" +
                   std::to_wstring(std::max(total_count, 0)) + L")");
        switch (hover_popup_sort_mode_) {
        case HoverPopupSortMode::kCpu:
            return std::wstring(Text(L"Processes sorted by CPU", L"按 CPU 排序的进程")) + count_suffix;
        case HoverPopupSortMode::kMemory:
            return std::wstring(Text(L"Processes sorted by memory (USS -> RSS -> VMS)",
                                     L"按内存排序的进程 (USS -> RSS -> VMS)")) +
                   count_suffix;
        case HoverPopupSortMode::kGpu:
            return std::wstring(Text(L"Processes sorted by GPU", L"按 GPU 排序的进程")) + count_suffix;
        case HoverPopupSortMode::kVram:
            return std::wstring(Text(L"Processes sorted by VRAM", L"按 VRAM 排序的进程")) + count_suffix;
        case HoverPopupSortMode::kIo:
            return std::wstring(Text(L"Processes sorted by IO", L"按 IO 排序的进程")) + count_suffix;
        case HoverPopupSortMode::kNetwork:
            return std::wstring(Text(L"Processes sorted by network", L"按网络排序的进程")) + count_suffix;
        case HoverPopupSortMode::kDefault:
        default:
            return std::wstring(Text(L"Processes by blended pressure score",
                                     L"按综合压力评分排序的进程")) +
                   count_suffix;
        }
    }

    void ApplyHoverPopupSort() {
        hover_popup_snapshot_ = hover_popup_base_snapshot_;
        if (!hover_popup_search_text_.empty()) {
            const std::wstring needle = ToLowerCopy(hover_popup_search_text_);
            std::erase_if(hover_popup_snapshot_.top_processes,
                          [&needle](const ProcessPopupItem& item) {
                              return !ContainsCaseInsensitive(item.name, needle) &&
                                     std::to_wstring(item.pid).find(needle) == std::wstring::npos;
                          });
        }

        if (hover_popup_sort_mode_ == HoverPopupSortMode::kDefault) {
            ClampHoverPopupScrollOffset();
            return;
        }

        auto default_compare = [](const ProcessPopupItem& left, const ProcessPopupItem& right) {
            if (std::abs(left.score - right.score) > 0.01) {
                return left.score > right.score;
            }
            if (std::abs(left.cpu_percent - right.cpu_percent) > 0.01) {
                return left.cpu_percent > right.cpu_percent;
            }
            if (left.uss_bytes != right.uss_bytes) {
                return left.uss_bytes > right.uss_bytes;
            }
            if (left.rss_bytes != right.rss_bytes) {
                return left.rss_bytes > right.rss_bytes;
            }
            if (left.vms_bytes != right.vms_bytes) {
                return left.vms_bytes > right.vms_bytes;
            }
            if (std::abs(left.gpu_percent - right.gpu_percent) > 0.01) {
                return left.gpu_percent > right.gpu_percent;
            }
            if (left.vram_bytes != right.vram_bytes) {
                return left.vram_bytes > right.vram_bytes;
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
        };

        const auto compare_with_tie_break =
            [&default_compare](auto metric_compare) {
                return [default_compare, metric_compare](const ProcessPopupItem& left,
                                                         const ProcessPopupItem& right) {
                    if (metric_compare(left, right)) {
                        return true;
                    }
                    if (metric_compare(right, left)) {
                        return false;
                    }
                    return default_compare(left, right);
                };
            };

        switch (hover_popup_sort_mode_) {
        case HoverPopupSortMode::kCpu:
            std::stable_sort(hover_popup_snapshot_.top_processes.begin(),
                             hover_popup_snapshot_.top_processes.end(),
                             compare_with_tie_break([](const ProcessPopupItem& left,
                                                       const ProcessPopupItem& right) {
                                 return std::abs(left.cpu_percent - right.cpu_percent) > 0.01 &&
                                        left.cpu_percent > right.cpu_percent;
                             }));
            break;
        case HoverPopupSortMode::kMemory:
            std::stable_sort(hover_popup_snapshot_.top_processes.begin(),
                             hover_popup_snapshot_.top_processes.end(),
                             compare_with_tie_break([](const ProcessPopupItem& left,
                                                       const ProcessPopupItem& right) {
                                 if (left.uss_bytes != right.uss_bytes) {
                                     return left.uss_bytes > right.uss_bytes;
                                 }
                                 if (left.rss_bytes != right.rss_bytes) {
                                     return left.rss_bytes > right.rss_bytes;
                                 }
                                 if (left.vms_bytes != right.vms_bytes) {
                                     return left.vms_bytes > right.vms_bytes;
                                 }
                                 return false;
                             }));
            break;
        case HoverPopupSortMode::kGpu:
            std::stable_sort(hover_popup_snapshot_.top_processes.begin(),
                             hover_popup_snapshot_.top_processes.end(),
                             compare_with_tie_break([](const ProcessPopupItem& left,
                                                       const ProcessPopupItem& right) {
                                 if (std::abs(left.gpu_percent - right.gpu_percent) > 0.01) {
                                     return left.gpu_percent > right.gpu_percent;
                                 }
                                 if (left.vram_bytes != right.vram_bytes) {
                                     return left.vram_bytes > right.vram_bytes;
                                 }
                                 return false;
                             }));
            break;
        case HoverPopupSortMode::kVram:
            std::stable_sort(hover_popup_snapshot_.top_processes.begin(),
                             hover_popup_snapshot_.top_processes.end(),
                             compare_with_tie_break([](const ProcessPopupItem& left,
                                                       const ProcessPopupItem& right) {
                                 return left.vram_bytes != right.vram_bytes &&
                                        left.vram_bytes > right.vram_bytes;
                             }));
            break;
        case HoverPopupSortMode::kIo:
            std::stable_sort(hover_popup_snapshot_.top_processes.begin(),
                             hover_popup_snapshot_.top_processes.end(),
                             compare_with_tie_break([](const ProcessPopupItem& left,
                                                       const ProcessPopupItem& right) {
                                 const unsigned long long left_io =
                                     left.io_read_bytes_per_second + left.io_write_bytes_per_second;
                                 const unsigned long long right_io =
                                     right.io_read_bytes_per_second + right.io_write_bytes_per_second;
                                 return left_io != right_io && left_io > right_io;
                             }));
            break;
        case HoverPopupSortMode::kNetwork:
            std::stable_sort(hover_popup_snapshot_.top_processes.begin(),
                             hover_popup_snapshot_.top_processes.end(),
                             compare_with_tie_break([](const ProcessPopupItem& left,
                                                       const ProcessPopupItem& right) {
                                 return left.network_bytes_per_second != right.network_bytes_per_second &&
                                        left.network_bytes_per_second >
                                            right.network_bytes_per_second;
                             }));
            break;
        case HoverPopupSortMode::kDefault:
        default:
            break;
        }

        ClampHoverPopupScrollOffset();
    }

    HoverPopupSortMode HitTestHoverPopupSortMode(const POINT& client_point) const {
        if (hover_popup_window_ == nullptr || !IsWindow(hover_popup_window_)) {
            return HoverPopupSortMode::kDefault;
        }

        RECT client_rect{};
        GetClientRect(hover_popup_window_, &client_rect);
        const HoverPopupTableLayout layout = ComputeHoverPopupTableLayout(client_rect);
        if (PtInRect(&layout.cpu_rect, client_point)) {
            return HoverPopupSortMode::kCpu;
        }
        if (PtInRect(&layout.mem_rect, client_point)) {
            return HoverPopupSortMode::kMemory;
        }
        if (PtInRect(&layout.gpu_rect, client_point)) {
            return HoverPopupSortMode::kGpu;
        }
        if (PtInRect(&layout.vram_rect, client_point)) {
            return HoverPopupSortMode::kVram;
        }
        if (PtInRect(&layout.io_rect, client_point)) {
            return HoverPopupSortMode::kIo;
        }
        if (PtInRect(&layout.net_rect, client_point)) {
            return HoverPopupSortMode::kNetwork;
        }
        return HoverPopupSortMode::kDefault;
    }

    bool IsHoverPopupSearchHit(const POINT& client_point) const {
        if (hover_popup_window_ == nullptr || !IsWindow(hover_popup_window_)) {
            return false;
        }

        RECT client_rect{};
        GetClientRect(hover_popup_window_, &client_rect);
        const HoverPopupTableLayout layout = ComputeHoverPopupTableLayout(client_rect);
        return PtInRect(&layout.search_rect, client_point) != FALSE;
    }

    void SetHoverPopupSearchActive(bool active) {
        if (hover_popup_search_active_ == active) {
            return;
        }

        hover_popup_search_active_ = active;
        RequestHoverPopupRedraw();
    }

    void SetHoverPopupSearchText(const std::wstring& text) {
        if (hover_popup_search_text_ == text) {
            return;
        }

        hover_popup_search_text_ = text;
        hover_popup_scroll_offset_ = 0;
        RefreshHoverPopupView();
    }

    void AppendHoverPopupSearchCharacter(wchar_t ch) {
        if (hover_popup_search_text_.size() >= 64) {
            return;
        }

        std::wstring updated_text = hover_popup_search_text_;
        updated_text.push_back(ch);
        SetHoverPopupSearchText(updated_text);
    }

    void RemoveHoverPopupSearchCharacter() {
        if (hover_popup_search_text_.empty()) {
            return;
        }

        std::wstring updated_text = hover_popup_search_text_;
        updated_text.pop_back();
        SetHoverPopupSearchText(updated_text);
    }

    bool HandleHoverPopupSearchKeyDown(WPARAM key) {
        switch (key) {
        case VK_UP:
            ScrollHoverPopupBy(-1);
            return true;
        case VK_DOWN:
            ScrollHoverPopupBy(1);
            return true;
        case VK_PRIOR:
            ScrollHoverPopupBy(-GetHoverPopupVisibleRowCount());
            return true;
        case VK_NEXT:
            ScrollHoverPopupBy(GetHoverPopupVisibleRowCount());
            return true;
        case VK_HOME:
            SetHoverPopupScrollOffset(0);
            return true;
        case VK_END:
            SetHoverPopupScrollOffset(GetHoverPopupMaxScrollOffset());
            return true;
        case VK_ESCAPE:
            if (!hover_popup_search_text_.empty()) {
                SetHoverPopupSearchText(L"");
            } else {
                SetHoverPopupSearchActive(false);
            }
            return true;
        default:
            return false;
        }
    }

    bool HandleHoverPopupSearchChar(WPARAM key) {
        if (key == VK_BACK) {
            RemoveHoverPopupSearchCharacter();
            return true;
        }

        if (key < 32 || key == 127) {
            return false;
        }

        SetHoverPopupSearchActive(true);
        AppendHoverPopupSearchCharacter(static_cast<wchar_t>(key));
        return true;
    }

    void ToggleHoverPopupSort(HoverPopupSortMode mode) {
        if (mode == HoverPopupSortMode::kDefault) {
            return;
        }

        hover_popup_sort_mode_ =
            (hover_popup_sort_mode_ == mode) ? HoverPopupSortMode::kDefault : mode;
        hover_popup_scroll_offset_ = 0;
        RefreshHoverPopupView();
    }

    void HandleHoverPopupClick(const POINT& client_point) {
        SetFocus(hover_popup_window_);
        if (IsHoverPopupSearchHit(client_point)) {
            SetHoverPopupSearchActive(true);
            return;
        }

        SetHoverPopupSearchActive(false);
        ToggleHoverPopupSort(HitTestHoverPopupSortMode(client_point));
    }

    void HandleHoverPopupVScroll(WPARAM scroll_code, int thumb_position) {
        switch (scroll_code) {
        case SB_LINEUP:
            ScrollHoverPopupBy(-1);
            break;
        case SB_LINEDOWN:
            ScrollHoverPopupBy(1);
            break;
        case SB_PAGEUP:
            ScrollHoverPopupBy(-GetHoverPopupVisibleRowCount());
            break;
        case SB_PAGEDOWN:
            ScrollHoverPopupBy(GetHoverPopupVisibleRowCount());
            break;
        case SB_TOP:
            SetHoverPopupScrollOffset(0);
            break;
        case SB_BOTTOM:
            SetHoverPopupScrollOffset(GetHoverPopupMaxScrollOffset());
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            SetHoverPopupScrollOffset(thumb_position);
            break;
        default:
            break;
        }
    }

    void HandleHoverPopupMouseWheel(short delta) {
        const int notches = static_cast<int>(delta / WHEEL_DELTA);
        if (notches != 0) {
            ScrollHoverPopupBy(-notches * kHoverPopupWheelRows);
        }
    }

    void UpdateHoverPopupSize() {
        if (stock_mode_) {
            const int visible_rows =
                std::max<int>(1, std::min<int>(static_cast<int>(stock_rows_.size()), 10));
            hover_popup_size_.cx = ScaleByDpi(current_dpi_, 560);
            hover_popup_size_.cy = ScaleByDpi(current_dpi_, 96) + popup_title_line_height_ +
                                   GetHoverPopupRowHeight() * visible_rows +
                                   ScaleByDpi(current_dpi_, 24);
            return;
        }

        const int padding = ScaleByDpi(current_dpi_, 16);
        const int title_gap = ScaleByDpi(current_dpi_, 8);
        const int summary_gap = ScaleByDpi(current_dpi_, 4);
        const int section_gap = ScaleByDpi(current_dpi_, 10);
        const int search_height = GetHoverPopupSearchHeight();
        const int header_height = popup_text_line_height_ + ScaleByDpi(current_dpi_, 8);
        const int row_gap = ScaleByDpi(current_dpi_, 6);
        const int row_height = GetHoverPopupRowHeight();
        const int footer_height =
            popup_text_line_height_ * 2 + ScaleByDpi(current_dpi_, 10);
        const int row_count = GetHoverPopupVisibleRowCount();

        hover_popup_size_.cx = ScaleByDpi(current_dpi_, 980);
        hover_popup_size_.cy = padding + popup_title_line_height_ + title_gap +
                               (popup_text_line_height_ + summary_gap) * 3 + section_gap +
                               search_height + section_gap + header_height + row_gap +
                               row_count * row_height + section_gap + footer_height + padding;
    }

    void DrawStockHoverPopupContents(HDC dc, const RECT& client_rect) {
        const bool light_theme = IsLightTaskbarTheme();
        const HoverPopupPalette palette = GetHoverPopupPalette(light_theme);

        HBRUSH background_brush = CreateSolidBrush(palette.background);
        FillRect(dc, &client_rect, background_brush);
        DeleteObject(background_brush);

        HBRUSH border_brush = CreateSolidBrush(palette.border);
        FrameRect(dc, &client_rect, border_brush);
        DeleteObject(border_brush);

        RECT accent_rect{client_rect.left, client_rect.top, client_rect.right,
                         client_rect.top + ScaleByDpi(current_dpi_, 3)};
        HBRUSH accent_brush = CreateSolidBrush(palette.accent);
        FillRect(dc, &accent_rect, accent_brush);
        DeleteObject(accent_brush);

        const int padding = ScaleByDpi(current_dpi_, 16);
        const int gap = ScaleByDpi(current_dpi_, 8);
        const int cell_pad = ScaleByDpi(current_dpi_, 8);
        const int content_left = client_rect.left + padding;
        const int content_right = client_rect.right - padding;

        SetBkMode(dc, TRANSPARENT);
        HFONT title_font =
            popup_title_font_ != nullptr ? popup_title_font_
                                         : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HFONT body_font =
            popup_font_ != nullptr ? popup_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(dc, title_font);

        RECT title_rect{content_left,
                        client_rect.top + padding,
                        content_right,
                        client_rect.top + padding + popup_title_line_height_};
        SetTextColor(dc, palette.title_text);
        DrawTextW(dc,
                  Text(L"Stock Snapshot", L"股票快照"),
                  -1,
                  &title_rect,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

        const std::wstring subtitle = IsChinese()
                                          ? std::to_wstring(stock_rows_.size()) + L" 个股票"
                                          : std::to_wstring(stock_rows_.size()) + L" symbols shown";
        SetTextColor(dc, palette.secondary_text);
        DrawTextW(dc,
                  subtitle.c_str(),
                  -1,
                  &title_rect,
                  DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX | DT_END_ELLIPSIS);

        SelectObject(dc, body_font);
        RECT summary_rect{content_left,
                          title_rect.bottom + gap,
                          content_right,
                          title_rect.bottom + gap + popup_text_line_height_};
        const std::wstring summary =
            std::wstring(Text(L"Updated ", L"更新时间 ")) + stock_last_update_time_ + L"   USD/HKD " +
            FormatPriceValue(stock_config_.usd_hkd_rate) + L"   Alpaca " +
            stock_config_.alpaca_feed;
        SetTextColor(dc, palette.secondary_text);
        DrawTextW(dc,
                  summary.c_str(),
                  -1,
                  &summary_rect,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

        RECT header_rect{content_left,
                         summary_rect.bottom + gap,
                         content_right,
                         summary_rect.bottom + gap + popup_text_line_height_ +
                             ScaleByDpi(current_dpi_, 8)};
        HBRUSH header_brush = CreateSolidBrush(palette.header_fill);
        FillRect(dc, &header_rect, header_brush);
        DeleteObject(header_brush);

        RECT rank_header{header_rect.left + cell_pad,
                         header_rect.top,
                         header_rect.left + ScaleByDpi(current_dpi_, 48),
                         header_rect.bottom};
        RECT symbol_header{rank_header.right,
                           header_rect.top,
                           header_rect.left + ScaleByDpi(current_dpi_, 170),
                           header_rect.bottom};
        RECT price_header{symbol_header.right,
                          header_rect.top,
                          header_rect.left + ScaleByDpi(current_dpi_, 330),
                          header_rect.bottom};
        RECT change_header{price_header.right,
                           header_rect.top,
                           header_rect.left + ScaleByDpi(current_dpi_, 430),
                           header_rect.bottom};
        RECT market_header{change_header.right,
                           header_rect.top,
                           header_rect.right - cell_pad,
                           header_rect.bottom};

        SetTextColor(dc, palette.secondary_text);
        DrawTextW(dc, L"#", -1, &rank_header,
                  DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);
        DrawTextW(dc, Text(L"Symbol", L"股票"), -1, &symbol_header,
                  DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);
        DrawTextW(dc, Text(L"Price", L"价格"), -1, &price_header,
                  DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX);
        DrawTextW(dc, Text(L"Change", L"涨跌幅"), -1, &change_header,
                  DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX);
        DrawTextW(dc, Text(L"Market", L"市场"), -1, &market_header,
                  DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX);

        const int row_height = GetHoverPopupRowHeight();
        int row_top = header_rect.bottom;
        const int end_index = std::min<int>(static_cast<int>(stock_rows_.size()), 10);
        for (int index = 0; index < end_index; ++index) {
            const StockRow& row = stock_rows_[index];
            RECT row_rect{content_left, row_top, content_right, row_top + row_height};
            if ((index % 2) == 0) {
                HBRUSH row_brush = CreateSolidBrush(palette.row_highlight);
                FillRect(dc, &row_rect, row_brush);
                DeleteObject(row_brush);
            }
            if (index < 3) {
                RECT marker_rect{row_rect.left, row_rect.top,
                                 row_rect.left + ScaleByDpi(current_dpi_, 3), row_rect.bottom};
                HBRUSH marker_brush = CreateSolidBrush(palette.accent);
                FillRect(dc, &marker_rect, marker_brush);
                DeleteObject(marker_brush);
            }

            RECT rank_rect{rank_header.left, row_rect.top, rank_header.right, row_rect.bottom};
            RECT symbol_rect{symbol_header.left, row_rect.top, symbol_header.right, row_rect.bottom};
            RECT price_rect{price_header.left, row_rect.top, price_header.right, row_rect.bottom};
            RECT change_rect{change_header.left, row_rect.top, change_header.right, row_rect.bottom};
            RECT market_rect{market_header.left, row_rect.top, market_header.right, row_rect.bottom};

            const std::wstring rank = std::to_wstring(index + 1);
            SetTextColor(dc, palette.secondary_text);
            DrawTextW(dc, rank.c_str(), -1, &rank_rect,
                      DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            SetTextColor(dc, palette.primary_text);
            DrawTextW(dc, row.symbol.c_str(), -1, &symbol_rect,
                      DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            DrawTextW(dc, row.price_text.c_str(), -1, &price_rect,
                      DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            const std::wstring change =
                row.change_percent ? FormatSignedPercentValue(*row.change_percent) : L"--";
            DrawTextW(dc, change.c_str(), -1, &change_rect,
                      DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            SetTextColor(dc, palette.secondary_text);
            DrawTextW(dc, row.market.c_str(), -1, &market_rect,
                      DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            row_top += row_height;
        }

        RECT footer_rect{content_left,
                         row_top + gap,
                         content_right,
                         row_top + gap + popup_text_line_height_};
        SetTextColor(dc, palette.secondary_text);
        DrawTextW(dc,
                  Text(L"* Change is calculated against previous close when available.",
                       L"* 涨跌幅在可用时基于昨收计算。"),
                  -1,
                  &footer_rect,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

        SelectObject(dc, old_font);
    }

    void DrawHoverPopupContents(HDC dc, const RECT& client_rect) {
        if (stock_mode_) {
            DrawStockHoverPopupContents(dc, client_rect);
            return;
        }

        const bool light_theme = IsLightTaskbarTheme();
        const HoverPopupPalette palette = GetHoverPopupPalette(light_theme);

        HBRUSH background_brush = CreateSolidBrush(palette.background);
        FillRect(dc, &client_rect, background_brush);
        DeleteObject(background_brush);

        HBRUSH border_brush = CreateSolidBrush(palette.border);
        FrameRect(dc, &client_rect, border_brush);
        DeleteObject(border_brush);

        RECT accent_rect{client_rect.left, client_rect.top, client_rect.right,
                         client_rect.top + ScaleByDpi(current_dpi_, 3)};
        HBRUSH accent_brush = CreateSolidBrush(palette.accent);
        FillRect(dc, &accent_rect, accent_brush);
        DeleteObject(accent_brush);

        const int padding_left = ScaleByDpi(current_dpi_, 16);
        const int padding_right = ScaleByDpi(current_dpi_, 30);
        const int gap = ScaleByDpi(current_dpi_, 8);
        const int line_gap = ScaleByDpi(current_dpi_, 4);
        const int row_gap = ScaleByDpi(current_dpi_, 6);
        const int scroll_bar_width = GetSystemMetrics(SM_CXVSCROLL);
        const int content_left = client_rect.left + padding_left;
        const int content_right = client_rect.right - padding_right - scroll_bar_width;

        SetBkMode(dc, TRANSPARENT);

        HFONT title_font =
            popup_title_font_ != nullptr ? popup_title_font_
                                         : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HFONT body_font =
            popup_font_ != nullptr ? popup_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(dc, title_font);

        RECT title_rect{content_left,
                        client_rect.top + padding_left,
                        content_right,
                        client_rect.top + padding_left + popup_title_line_height_};
        SetTextColor(dc, palette.title_text);
        DrawTextW(dc,
                  Text(L"Performance Snapshot", L"性能快照"),
                  -1,
                  &title_rect,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

        RECT subtitle_rect{content_left,
                           client_rect.top + padding_left,
                           content_right,
                           client_rect.top + padding_left + popup_title_line_height_};
        SetTextColor(dc, palette.secondary_text);
        DrawTextW(dc,
                  GetHoverPopupSubtitle().c_str(),
                  -1,
                  &subtitle_rect,
                  DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX | DT_END_ELLIPSIS);

        SelectObject(dc, body_font);

        int content_top = title_rect.bottom + gap;
        RECT line_rect{content_left, content_top, content_right,
                       content_top + popup_text_line_height_};

        auto draw_inline_segment = [&](RECT& rect, const std::wstring& text, COLORREF color) {
            if (text.empty()) {
                return;
            }
            SetTextColor(dc, color);
            RECT draw_rect = rect;
            DrawTextW(dc,
                      text.c_str(),
                      -1,
                      &draw_rect,
                      DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
            rect.left += MeasureText(dc, text).cx;
        };

        RECT summary_line1_rect = line_rect;
        const int total_process_count =
            std::max(hover_popup_base_snapshot_.total_process_count,
                     hover_popup_snapshot_.total_process_count);
        const std::wstring proc_summary =
            hover_popup_search_text_.empty()
                ? std::to_wstring(std::max(total_process_count, 0))
                : (std::to_wstring(hover_popup_snapshot_.top_processes.size()) + L"/" +
                   std::to_wstring(std::max(total_process_count, 0)));
        const std::wstring gpu_summary =
            last_snapshot_.gpu_percent >= 0 ? FormatPercentValue(last_snapshot_.gpu_percent) : L"--";
        draw_inline_segment(summary_line1_rect, L"CPU ", palette.primary_text);
        draw_inline_segment(summary_line1_rect,
                            FormatPercentValue(std::max(last_snapshot_.cpu_percent, 0)),
                            ResolveAlertColor(palette,
                                              GetUsageAlertLevel(last_snapshot_.cpu_percent, 80.0, 95.0),
                                              palette.primary_text));
        draw_inline_segment(summary_line1_rect, Text(L"   MEM ", L"   内存 "), palette.primary_text);
        draw_inline_segment(summary_line1_rect,
                            FormatPercentValue(std::max(last_snapshot_.memory_percent, 0)),
                            ResolveAlertColor(palette,
                                              GetUsageAlertLevel(last_snapshot_.memory_percent,
                                                                 85.0,
                                                                 95.0),
                                              palette.primary_text));
        draw_inline_segment(summary_line1_rect, L"   GPU ", palette.primary_text);
        draw_inline_segment(summary_line1_rect,
                            gpu_summary,
                            ResolveAlertColor(palette,
                                              GetUsageAlertLevel(last_snapshot_.gpu_percent, 90.0, 98.0),
                                              palette.primary_text));
        draw_inline_segment(summary_line1_rect, Text(L"   PROC ", L"   进程 "), palette.primary_text);
        draw_inline_segment(summary_line1_rect, proc_summary, palette.secondary_text);

        const std::wstring summary_line2 =
            std::wstring(Text(L"NET \u2191 ", L"网络 \u2191 ")) +
            FormatNetworkRate(last_snapshot_.upload_bytes_per_second,
                              app_config_.network_display_unit) +
            L"   \u2193 " +
            FormatNetworkRate(last_snapshot_.download_bytes_per_second,
                              app_config_.network_display_unit) +
            std::wstring(Text(L"   DISK R ", L"   磁盘读 ")) +
            FormatRate(last_snapshot_.disk_read_bytes_per_second) +
            std::wstring(Text(L"   W ", L"   磁盘写 ")) +
            FormatRate(last_snapshot_.disk_write_bytes_per_second);
        const std::wstring summary_line3 =
            std::wstring(Text(L"Uptime ", L"运行时间 ")) +
            FormatUptime(hover_popup_snapshot_.uptime_ms) +
            Text(L"   MEM = RSS / USS / VMS(commit)   VRAM = dedicated GPU memory",
                 L"   内存 = RSS / USS / VMS(commit)   VRAM = 专用 GPU 内存");

        OffsetRect(&line_rect, 0, popup_text_line_height_ + line_gap);
        SetTextColor(dc, palette.primary_text);
        DrawTextW(dc,
                  summary_line2.c_str(),
                  -1,
                  &line_rect,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        OffsetRect(&line_rect, 0, popup_text_line_height_ + line_gap);
        SetTextColor(dc, palette.secondary_text);
        DrawTextW(dc,
                  summary_line3.c_str(),
                  -1,
                  &line_rect,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

        const HoverPopupTableLayout table_layout = ComputeHoverPopupTableLayout(client_rect);
        HBRUSH search_fill_brush = CreateSolidBrush(palette.search_fill);
        FillRect(dc, &table_layout.search_rect, search_fill_brush);
        DeleteObject(search_fill_brush);

        HBRUSH search_border_brush = CreateSolidBrush(hover_popup_search_active_
                                                          ? palette.search_active_border
                                                          : palette.search_border);
        FrameRect(dc, &table_layout.search_rect, search_border_brush);
        DeleteObject(search_border_brush);

        RECT search_text_rect = table_layout.search_rect;
        InflateRect(&search_text_rect, -ScaleByDpi(current_dpi_, 8), 0);
        std::wstring search_text;
        COLORREF search_text_color = palette.primary_text;
        if (!hover_popup_search_text_.empty()) {
            search_text = hover_popup_search_text_;
            if (hover_popup_search_active_) {
                search_text += L" |";
            }
        } else if (hover_popup_search_active_) {
            search_text = L"|";
        } else {
            search_text = Text(L"Search processes...  (type to filter)",
                               L"搜索进程...  (输入以过滤)");
            search_text_color = palette.search_placeholder;
        }
        SetTextColor(dc, search_text_color);
        DrawTextW(dc,
                  search_text.c_str(),
                  -1,
                  &search_text_rect,
                  DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

        const RECT& header_rect = table_layout.header_rect;
        HBRUSH header_brush = CreateSolidBrush(palette.header_fill);
        FillRect(dc, &header_rect, header_brush);
        DeleteObject(header_brush);

        auto draw_header = [&](const std::wstring& text,
                               const RECT& rect,
                               UINT format,
                               HoverPopupSortMode mode = HoverPopupSortMode::kDefault) {
            SetTextColor(dc,
                         hover_popup_sort_mode_ == mode && mode != HoverPopupSortMode::kDefault
                             ? palette.title_text
                             : palette.secondary_text);
            RECT draw_rect = rect;
            DrawTextW(dc,
                      text.c_str(),
                      -1,
                      &draw_rect,
                      format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        };

        draw_header(L"#", table_layout.rank_rect, DT_LEFT);
        draw_header(Text(L"Process", L"进程"), table_layout.name_rect, DT_LEFT);
        draw_header(GetHoverPopupHeaderText(L"CPU", HoverPopupSortMode::kCpu),
                    table_layout.cpu_rect,
                    DT_RIGHT,
                    HoverPopupSortMode::kCpu);
        draw_header(GetHoverPopupHeaderText(L"MEM (R/U/V)", HoverPopupSortMode::kMemory),
                    table_layout.mem_rect,
                    DT_RIGHT,
                    HoverPopupSortMode::kMemory);
        draw_header(GetHoverPopupHeaderText(L"GPU", HoverPopupSortMode::kGpu),
                    table_layout.gpu_rect,
                    DT_RIGHT,
                    HoverPopupSortMode::kGpu);
        draw_header(GetHoverPopupHeaderText(L"VRAM", HoverPopupSortMode::kVram),
                    table_layout.vram_rect,
                    DT_RIGHT,
                    HoverPopupSortMode::kVram);
        draw_header(GetHoverPopupHeaderText(L"IO", HoverPopupSortMode::kIo),
                    table_layout.io_rect,
                    DT_RIGHT,
                    HoverPopupSortMode::kIo);
        draw_header(GetHoverPopupHeaderText(hover_popup_snapshot_.network_is_estimated ? L"NET*" : L"NET",
                                            HoverPopupSortMode::kNetwork),
                    table_layout.net_rect,
                    DT_RIGHT,
                    HoverPopupSortMode::kNetwork);

        const int row_height = GetHoverPopupRowHeight();
        int row_top = table_layout.list_rect.top;
        const int start_index =
            std::min<int>(hover_popup_scroll_offset_,
                          std::max<int>(0,
                                        static_cast<int>(hover_popup_snapshot_.top_processes.size()) - 1));
        const int end_index =
            std::min<int>(start_index + GetHoverPopupVisibleRowCount(),
                          static_cast<int>(hover_popup_snapshot_.top_processes.size()));
        if (hover_popup_snapshot_.top_processes.empty()) {
            RECT empty_rect{content_left,
                            row_top,
                            content_right,
                            row_top + row_height};
            SetTextColor(dc, palette.secondary_text);
            DrawTextW(dc,
                      hover_popup_base_snapshot_.total_process_count > 0
                          ? Text(L"No matching processes. Press Esc to clear search.",
                                 L"没有匹配的进程。按 Esc 清除搜索。")
                          : Text(L"Process data is warming up. Keep the popup open for a second.",
                                 L"进程数据正在预热，请保持弹窗打开一会儿。"),
                      -1,
                      &empty_rect,
                      DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        } else {
            for (int index = start_index; index < end_index; ++index) {
                const ProcessPopupItem& item = hover_popup_snapshot_.top_processes[index];
                RECT row_rect{content_left, row_top, content_right,
                              row_top + row_height};

                if ((index % 2) == 0) {
                    HBRUSH row_brush = CreateSolidBrush(palette.row_highlight);
                    FillRect(dc, &row_rect, row_brush);
                    DeleteObject(row_brush);
                }
                if (index < 3) {
                    RECT marker_rect{row_rect.left, row_rect.top, row_rect.left + ScaleByDpi(current_dpi_, 3),
                                     row_rect.bottom};
                    HBRUSH marker_brush = CreateSolidBrush(palette.accent);
                    FillRect(dc, &marker_rect, marker_brush);
                    DeleteObject(marker_brush);
                }

                RECT current_rank_rect{table_layout.rank_rect.left,
                                       row_rect.top,
                                       table_layout.rank_rect.right,
                                       row_rect.bottom};
                RECT current_name_rect{table_layout.name_rect.left,
                                       row_rect.top,
                                       table_layout.name_rect.right,
                                       row_rect.bottom};
                RECT current_cpu_rect{table_layout.cpu_rect.left,
                                      row_rect.top,
                                      table_layout.cpu_rect.right,
                                      row_rect.bottom};
                RECT current_mem_rect{table_layout.mem_rect.left,
                                      row_rect.top,
                                      table_layout.mem_rect.right,
                                      row_rect.bottom};
                RECT current_gpu_rect{table_layout.gpu_rect.left,
                                      row_rect.top,
                                      table_layout.gpu_rect.right,
                                      row_rect.bottom};
                RECT current_vram_rect{table_layout.vram_rect.left,
                                       row_rect.top,
                                       table_layout.vram_rect.right,
                                       row_rect.bottom};
                RECT current_io_rect{table_layout.io_rect.left,
                                     row_rect.top,
                                     table_layout.io_rect.right,
                                     row_rect.bottom};
                RECT current_net_rect{table_layout.net_rect.left,
                                      row_rect.top,
                                      table_layout.net_rect.right,
                                      row_rect.bottom};

                const std::wstring rank_text = std::to_wstring(index + 1);
                const std::wstring cpu_text = FormatPercentValue(item.cpu_percent);
                const std::wstring mem_text =
                    FormatBytes(item.rss_bytes) + L" / " + FormatBytes(item.uss_bytes) + L" / " +
                    FormatBytes(item.vms_bytes);
                const std::wstring gpu_text =
                    last_snapshot_.gpu_percent >= 0 ? FormatPercentValue(item.gpu_percent) : L"--";
                const std::wstring vram_text = FormatBytes(item.vram_bytes);
                const std::wstring io_text =
                    FormatRate(item.io_read_bytes_per_second + item.io_write_bytes_per_second);
                const std::wstring net_text = hover_popup_snapshot_.network_metric_available
                                                  ? FormatNetworkRate(
                                                        item.network_bytes_per_second,
                                                        app_config_.network_display_unit)
                                                  : L"--";
                const COLORREF cpu_color =
                    ResolveAlertColor(palette,
                                      GetUsageAlertLevel(item.cpu_percent, 80.0, 95.0),
                                      palette.primary_text);
                const COLORREF gpu_color =
                    last_snapshot_.gpu_percent >= 0
                        ? ResolveAlertColor(palette,
                                            GetUsageAlertLevel(item.gpu_percent, 90.0, 98.0),
                                            palette.primary_text)
                        : palette.secondary_text;

                SetTextColor(dc, palette.secondary_text);
                DrawTextW(dc,
                          rank_text.c_str(),
                          -1,
                          &current_rank_rect,
                          DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SetTextColor(dc, palette.primary_text);
                DrawTextW(dc,
                          item.name.c_str(),
                          -1,
                          &current_name_rect,
                          DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SetTextColor(dc, cpu_color);
                DrawTextW(dc,
                          cpu_text.c_str(),
                          -1,
                          &current_cpu_rect,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SetTextColor(dc, palette.primary_text);
                DrawTextW(dc,
                          mem_text.c_str(),
                          -1,
                          &current_mem_rect,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SetTextColor(dc, gpu_color);
                DrawTextW(dc,
                          gpu_text.c_str(),
                          -1,
                          &current_gpu_rect,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SetTextColor(dc, palette.primary_text);
                DrawTextW(dc,
                          vram_text.c_str(),
                          -1,
                          &current_vram_rect,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                DrawTextW(dc,
                          io_text.c_str(),
                          -1,
                          &current_io_rect,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                DrawTextW(dc,
                          net_text.c_str(),
                          -1,
                          &current_net_rect,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

                row_top += row_height;
            }
        }

        RECT footer_rect1{content_left,
                          table_layout.list_rect.bottom + gap,
                          content_right,
                          table_layout.list_rect.bottom + gap + popup_text_line_height_};
        RECT footer_rect2{content_left,
                          footer_rect1.bottom + line_gap,
                          content_right,
                          footer_rect1.bottom + line_gap + popup_text_line_height_};
        SetTextColor(dc, palette.secondary_text);
        DrawTextW(dc,
                  Text(L"* MEM shows RSS / USS / VMS(commit, psutil/taskmgr-style). PSS is not exposed directly yet.",
                       L"* MEM 显示 RSS / USS / VMS(commit，类似 psutil/任务管理器)。暂不直接显示 PSS。"),
                  -1,
                  &footer_rect1,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        DrawTextW(dc,
                  GetNetworkUnitFooterText(),
                  -1,
                  &footer_rect2,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

        SelectObject(dc, old_font);
    }

    void PaintHoverPopup() {
        PAINTSTRUCT paint_struct{};
        HDC dc = BeginPaint(hover_popup_window_, &paint_struct);

        RECT client_rect{};
        GetClientRect(hover_popup_window_, &client_rect);
        DrawHoverPopupContents(dc, client_rect);
        EndPaint(hover_popup_window_, &paint_struct);
    }

    void RequestHoverPopupRedraw() {
        if (hover_popup_window_ == nullptr || !IsWindow(hover_popup_window_) || !hover_popup_visible_) {
            return;
        }

        RedrawWindow(hover_popup_window_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }

    bool IsCursorOverWidgetOrPopup() const {
        POINT cursor_pos{};
        GetCursorPos(&cursor_pos);
        return IsPointInWindowRect(widget_window_, cursor_pos) ||
               (hover_popup_visible_ && IsPointInWindowRect(hover_popup_window_, cursor_pos));
    }

    void RefreshHoverPopupData() {
        if (stock_mode_) {
            UpdateHoverPopupSize();
            return;
        }

        hover_popup_base_snapshot_ = process_monitor_.Sample(last_snapshot_, 0);
        ApplyHoverPopupSort();
        UpdateHoverPopupSize();
        UpdateHoverPopupScrollBar();
    }

    void PositionHoverPopup() {
        if (hover_popup_window_ == nullptr || !IsWindow(hover_popup_window_) || widget_window_ == nullptr ||
            !IsWindow(widget_window_)) {
            return;
        }

        RECT widget_rect{};
        if (!GetWindowRect(widget_window_, &widget_rect)) {
            return;
        }

        HMONITOR monitor_handle = MonitorFromRect(&widget_rect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        if (!GetMonitorInfoW(monitor_handle, &monitor_info)) {
            return;
        }

        const RECT bounds = monitor_info.rcMonitor;
        const int gap = ScaleByDpi(current_dpi_, 8);

        const int min_x = static_cast<int>(bounds.left) + gap;
        const int max_x =
            std::max<int>(min_x, static_cast<int>(bounds.right) - hover_popup_size_.cx - gap);
        const int min_y = static_cast<int>(bounds.top) + gap;
        const int max_y =
            std::max<int>(min_y, static_cast<int>(bounds.bottom) - hover_popup_size_.cy - gap);

        const int space_above = static_cast<int>(widget_rect.top - bounds.top);
        const int space_below = static_cast<int>(bounds.bottom - widget_rect.bottom);
        const int space_left = static_cast<int>(widget_rect.left - bounds.left);
        const int space_right = static_cast<int>(bounds.right - widget_rect.right);

        int x = std::clamp<int>(static_cast<int>(widget_rect.right) - hover_popup_size_.cx, min_x, max_x);
        int y = 0;

        const bool prefer_vertical =
            std::max(space_above, space_below) >= std::max(space_left, space_right);
        if (prefer_vertical) {
            if (space_above >= hover_popup_size_.cy + gap || space_above >= space_below) {
                y = widget_rect.top - hover_popup_size_.cy - gap;
            } else {
                y = widget_rect.bottom + gap;
            }
            y = std::clamp<int>(y, min_y, max_y);
        } else {
            if (space_left >= hover_popup_size_.cx + gap || space_left >= space_right) {
                x = static_cast<int>(widget_rect.left) - hover_popup_size_.cx - gap;
            } else {
                x = static_cast<int>(widget_rect.right) + gap;
            }
            x = std::clamp<int>(x, min_x, max_x);
            y = std::clamp<int>(static_cast<int>(widget_rect.bottom) - hover_popup_size_.cy,
                                min_y,
                                max_y);
        }

        SetWindowPos(hover_popup_window_,
                     HWND_TOPMOST,
                     x,
                     y,
                     hover_popup_size_.cx,
                     hover_popup_size_.cy,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void ShowHoverPopup(bool activate) {
        if (!EnsureHoverPopupWindow()) {
            return;
        }

        KillTimer(controller_window_, kHoverHideTimerId);
        RefreshHoverPopupData();
        PositionHoverPopup();
        ShowWindow(hover_popup_window_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
        hover_popup_visible_ = true;
        UpdateHoverPopupScrollBar();
        RequestHoverPopupRedraw();
        if (activate) {
            SetForegroundWindow(hover_popup_window_);
            SetActiveWindow(hover_popup_window_);
            SetFocus(hover_popup_window_);
        }
    }

    void HideHoverPopup() {
        KillTimer(controller_window_, kHoverHideTimerId);
        hover_popup_visible_ = false;
        hover_popup_search_active_ = false;
        if (hover_popup_window_ != nullptr && IsWindow(hover_popup_window_)) {
            ShowWindow(hover_popup_window_, SW_HIDE);
        }
    }

    void ArmHoverHideTimer() {
        if (app_config_.popup_activation_mode != PopupActivationMode::kHover) {
            return;
        }
        if (controller_window_ == nullptr || !IsWindow(controller_window_)) {
            return;
        }
        SetTimer(controller_window_, kHoverHideTimerId, kHoverHideDelayMs, nullptr);
    }

    void HandleHoverMove(HWND source_window) {
        if (app_config_.popup_activation_mode != PopupActivationMode::kHover) {
            return;
        }
        TRACKMOUSEEVENT track_event{};
        track_event.cbSize = sizeof(track_event);
        track_event.dwFlags = TME_LEAVE;
        track_event.hwndTrack = source_window;
        TrackMouseEvent(&track_event);

        KillTimer(controller_window_, kHoverHideTimerId);
        if (!hover_popup_visible_) {
            ShowHoverPopup(false);
        }
    }

    bool RenderLayeredWidget() {
        if (widget_window_ == nullptr || !IsWindow(widget_window_)) {
            return false;
        }

        RECT window_rect{};
        if (!GetWindowRect(widget_window_, &window_rect)) {
            return false;
        }

        const int width = window_rect.right - window_rect.left;
        const int height = window_rect.bottom - window_rect.top;
        if (width <= 0 || height <= 0) {
            return false;
        }

        HDC screen_dc = GetDC(nullptr);
        HDC memory_dc = CreateCompatibleDC(screen_dc);

        BITMAPINFO bitmap_info{};
        bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
        bitmap_info.bmiHeader.biWidth = width;
        bitmap_info.bmiHeader.biHeight = -height;
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;

        void* bitmap_bits = nullptr;
        HBITMAP bitmap =
            CreateDIBSection(screen_dc, &bitmap_info, DIB_RGB_COLORS, &bitmap_bits, nullptr, 0);
        if (bitmap == nullptr || bitmap_bits == nullptr) {
            if (bitmap != nullptr) {
                DeleteObject(bitmap);
            }
            DeleteDC(memory_dc);
            ReleaseDC(nullptr, screen_dc);
            return false;
        }

        HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
        RECT client_rect{0, 0, width, height};
        DrawWidgetContents(memory_dc, client_rect);

        auto* pixels = static_cast<BgraPixel*>(bitmap_bits);
        const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
        for (size_t i = 0; i < pixel_count; ++i) {
            pixels[i].alpha = 255;
        }

        POINT source_point{0, 0};
        POINT destination_point{window_rect.left, window_rect.top};
        HWND parent_window = GetParent(widget_window_);
        if (parent_window != nullptr) {
            ScreenToClient(parent_window, &destination_point);
        }
        SIZE window_size{width, height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const BOOL update_ok =
            UpdateLayeredWindow(widget_window_, screen_dc, &destination_point, &window_size,
                                memory_dc, &source_point, 0, &blend, ULW_ALPHA);

        SelectObject(memory_dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return update_ok != FALSE;
    }

    void RequestWidgetRedraw() {
        if (widget_window_ == nullptr || !IsWindow(widget_window_)) {
            return;
        }

        if (RenderLayeredWidget()) {
            return;
        }

        RedrawWindow(widget_window_,
                     nullptr,
                     nullptr,
                     RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    void SampleAndRefresh() {
        if (stock_mode_) {
            SampleStocks();
            RefreshFontAndSize();
            RequestWidgetRedraw();
            if (hover_popup_visible_) {
                PositionHoverPopup();
                RequestHoverPopupRedraw();
            }
            return;
        }

        last_snapshot_ = metrics_.Sample();
        UpdateDisplayLines(last_snapshot_);
        RequestWidgetRedraw();
        if (hover_popup_visible_) {
            RefreshHoverPopupData();
            PositionHoverPopup();
            RequestHoverPopupRedraw();
        }
    }

    void ToggleMonitorMode() {
        stock_mode_ = !stock_mode_;
        ApplyMonitorModeAfterChange();
    }

    void SetMonitorMode(bool stock_mode) {
        if (stock_mode_ == stock_mode) {
            return;
        }
        stock_mode_ = stock_mode;
        ApplyMonitorModeAfterChange();
    }

    void ApplyMonitorModeAfterChange() {
        HideHoverPopup();
        hover_popup_scroll_offset_ = 0;
        hover_popup_search_text_.clear();

        if (stock_mode_) {
            SetTimer(controller_window_,
                     kSampleTimerId,
                     std::max(1u, stock_config_.sample_interval_seconds) * 1000u,
                     nullptr);
            SampleStocks();
        } else {
            SetTimer(controller_window_,
                     kSampleTimerId,
                     GetSampleTimerIntervalMs(app_config_.sample_interval_seconds),
                     nullptr);
            UpdateDisplayLines(last_snapshot_);
        }

        RefreshFontAndSize();
        if (embedder_.IsAttached()) {
            if (!embedder_.RefreshLayout(widget_window_, widget_size_)) {
                ShowWindow(widget_window_, SW_HIDE);
                SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
                return;
            }
        }
        RequestWidgetRedraw();
    }

    void ReloadStockConfig() {
        stock_config_ = stock_taskbar_monitor::LoadOrCreateConfig();
        stock_active_alerts_.clear();
        if (stock_mode_) {
            SampleStocks();
            RefreshFontAndSize();
            RequestWidgetRedraw();
            if (hover_popup_visible_) {
                PositionHoverPopup();
                RequestHoverPopupRedraw();
            }
        }
    }

    void ShowStockConfigDialog() {
        if (!stock_taskbar_monitor::ShowStockConfigDialog(controller_window_,
                                                          instance_handle_,
                                                          IsChinese(),
                                                          &stock_config_)) {
            return;
        }
        stock_active_alerts_.clear();
        if (stock_mode_) {
            SampleStocks();
            RefreshFontAndSize();
            RequestWidgetRedraw();
            if (hover_popup_visible_) {
                PositionHoverPopup();
                RequestHoverPopupRedraw();
            }
            if (embedder_.IsAttached()) {
                embedder_.RefreshLayout(widget_window_, widget_size_);
            }
        }
    }

    void SetStockTaskbarSymbolCount(unsigned int count) {
        stock_config_.taskbar_symbol_count = count;
        stock_taskbar_monitor::SaveConfig(stock_config_);
        if (stock_mode_) {
            UpdateStockDisplayLines();
            RefreshFontAndSize();
            RequestWidgetRedraw();
            if (embedder_.IsAttached()) {
                embedder_.RefreshLayout(widget_window_, widget_size_);
            }
        }
    }

    void SetStockSortMode(stock_taskbar_monitor::StockSortMode sort_mode) {
        stock_config_.sort_mode = sort_mode;
        stock_taskbar_monitor::SaveConfig(stock_config_);
        SortStockRows();
        if (stock_mode_) {
            UpdateStockDisplayLines();
            RefreshFontAndSize();
            RequestWidgetRedraw();
            if (hover_popup_visible_) {
                PositionHoverPopup();
                RequestHoverPopupRedraw();
            }
            if (embedder_.IsAttached()) {
                embedder_.RefreshLayout(widget_window_, widget_size_);
            }
        }
    }

    bool IsChinese() const {
        return app_config_.language == UiLanguage::kChinese;
    }

    const wchar_t* Text(const wchar_t* english, const wchar_t* chinese) const {
        return IsChinese() ? chinese : english;
    }

    const wchar_t* AppTitle() const {
        return Text(L"Minimal Taskbar Monitor", L"任务栏监视器");
    }

    const wchar_t* ConfigSaveErrorText() const {
        return Text(L"Unable to save the local config file.", L"无法保存本地配置文件。");
    }

    const wchar_t* GetNetworkUnitFooterText() const {
        switch (app_config_.network_display_unit) {
        case NetworkDisplayUnit::kBytesPerSecond:
            return Text(
                L"* VRAM shows dedicated GPU memory. NET is shown in B/s and estimated from IO-other activity.",
                L"* VRAM 显示专用 GPU 内存。NET 以 B/s 显示，并基于 IO-other 活动估算。");
        case NetworkDisplayUnit::kBitsPerSecond:
        default:
            return Text(
                L"* VRAM shows dedicated GPU memory. NET is shown in bit/s and estimated from IO-other activity.",
                L"* VRAM 显示专用 GPU 内存。NET 以 bit/s 显示，并基于 IO-other 活动估算。");
        }
    }

    bool SetLanguage(UiLanguage language) {
        if (app_config_.language == language) {
            return true;
        }
        const UiLanguage previous_language = app_config_.language;
        app_config_.language = language;
        if (!SaveConfig()) {
            app_config_.language = previous_language;
            MessageBoxW(controller_window_,
                        ConfigSaveErrorText(),
                        AppTitle(),
                        MB_OK | MB_ICONERROR);
            return false;
        }
        if (stock_mode_) {
            UpdateStockDisplayLines();
        } else {
            UpdateDisplayLines(last_snapshot_);
        }
        RefreshFontAndSize();
        if (embedder_.IsAttached()) {
            if (!embedder_.RefreshLayout(widget_window_, widget_size_)) {
                ShowWindow(widget_window_, SW_HIDE);
                SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
            }
        }
        UpdateTrayIconTip();
        RequestWidgetRedraw();
        if (hover_popup_visible_) {
            PositionHoverPopup();
            RequestHoverPopupRedraw();
        }
        return true;
    }

    bool RegisterToggleHotkey() {
        if (controller_window_ == nullptr || !IsWindow(controller_window_)) {
            return false;
        }
        UnregisterHotKey(controller_window_, kToggleModeHotkeyId);
        const HotkeySpec spec = GetToggleHotkeySpec(app_config_.toggle_hotkey);
        return RegisterHotKey(controller_window_,
                              kToggleModeHotkeyId,
                              spec.modifiers,
                              spec.virtual_key) != FALSE;
    }

    bool SetToggleHotkey(ToggleHotkey hotkey) {
        if (app_config_.toggle_hotkey == hotkey) {
            return true;
        }

        const ToggleHotkey previous_hotkey = app_config_.toggle_hotkey;
        app_config_.toggle_hotkey = hotkey;
        if (!SaveConfig()) {
            app_config_.toggle_hotkey = previous_hotkey;
            MessageBoxW(controller_window_,
                        ConfigSaveErrorText(),
                        AppTitle(),
                        MB_OK | MB_ICONERROR);
            return false;
        }

        if (!RegisterToggleHotkey()) {
            const HotkeySpec spec = GetToggleHotkeySpec(app_config_.toggle_hotkey);
            std::wstring message = Text(L"Unable to register ", L"无法注册 ");
            message += spec.label;
            message += Text(L". Another app may already be using it.",
                            L"。可能已有其他程序在使用。");
            MessageBoxW(controller_window_,
                        message.c_str(),
                        AppTitle(),
                        MB_OK | MB_ICONWARNING);
        }
        return true;
    }

    void ShowHelpDialog() {
        const HotkeySpec spec = GetToggleHotkeySpec(app_config_.toggle_hotkey);
        std::wstring message;
        if (IsChinese()) {
            message += L"Minimal Taskbar Monitor\n\n";
            message += L"状态模式显示 CPU、内存、网络、GPU、磁盘和进程悬浮窗。\n";
            message += L"股票模式在任务栏显示配置的股价，并在悬浮窗显示股票详情。\n\n";
            message += L"快捷键：\n  ";
            message += spec.label;
            message += L" 在状态模式和股票模式之间切换。\n\n";
            message += L"菜单：\n";
            message += L"  状态：性能监视器设置。\n";
            message += L"  股票：股票模式、重载股票配置、任务栏显示数量和排序。\n";
            message += L"  快捷键：选择模式切换快捷键。\n";
            message += L"  语言：切换界面语言。\n\n";
            message += L"配置文件位于 exe 同目录：\n";
            message += L"  config.json：程序和状态监视设置。\n";
            message += L"  stocks_config.json：股票列表和 Alpaca key。\n\n";
            message += L"股票配置格式：\n";
            message += L"  港股：market=\"hk\"，code 写 5 位港股代码，例如 09988。\n";
            message += L"  A 股：market=\"cn\"，code 写 6 位代码；6 开头自动走上交所 sh，0/3 开头走深交所 sz。\n";
            message += L"  美股：market=\"us\" 或 source=\"alpaca\"，code 写美股 ticker，例如 NVDA。\n";
            message += L"  show_usd 只用于港股 ADR 折算；adr_factor 是 ADR 换算倍数。\n\n";
            message += L"示例：\n";
            message += L"  \"PINGAN\": { \"code\": \"000001\", \"market\": \"cn\" }\n";
            message += L"  \"BABA\": { \"code\": \"09988\", \"market\": \"hk\", \"show_usd\": true, \"adr_factor\": 8 }\n";
            message += L"  \"NVDA\": { \"code\": \"NVDA\", \"market\": \"us\", \"source\": \"alpaca\" }";
        } else {
            message += L"Minimal Taskbar Monitor\n\n";
            message += L"Status mode shows CPU, memory, network, GPU, disk, and the process popup.\n";
            message += L"Stock mode shows configured stock prices in the taskbar and stock details in the popup.\n\n";
            message += L"Shortcut:\n  ";
            message += spec.label;
            message += L" toggles between Status and Stock mode.\n\n";
            message += L"Menus:\n";
            message += L"  Status: performance monitor settings.\n";
            message += L"  Stock: stock mode, stock config reload, taskbar symbol count, and sorting.\n";
            message += L"  Hotkey: choose the mode-toggle shortcut.\n";
            message += L"  Language: switch UI language.\n\n";
            message += L"Config files are stored next to the exe:\n";
            message += L"  config.json for app/status settings.\n";
            message += L"  stocks_config.json for stock targets and Alpaca keys.\n\n";
            message += L"Stock config basics:\n";
            message += L"  HK: market=\"hk\", code is the 5-digit HK code, for example 09988.\n";
            message += L"  CN A-shares: market=\"cn\", code is the 6-digit code; 6-prefix uses Shanghai sh, 0/3-prefix uses Shenzhen sz.\n";
            message += L"  US: market=\"us\" or source=\"alpaca\", code is the US ticker, for example NVDA.\n";
            message += L"  show_usd only applies to HK ADR conversion; adr_factor is the ADR ratio.\n\n";
            message += L"Examples:\n";
            message += L"  \"PINGAN\": { \"code\": \"000001\", \"market\": \"cn\" }\n";
            message += L"  \"BABA\": { \"code\": \"09988\", \"market\": \"hk\", \"show_usd\": true, \"adr_factor\": 8 }\n";
            message += L"  \"NVDA\": { \"code\": \"NVDA\", \"market\": \"us\", \"source\": \"alpaca\" }";
        }
        MessageBoxW(controller_window_,
                    message.c_str(),
                    Text(L"Help", L"帮助"),
                    MB_OK | MB_ICONINFORMATION);
    }

    bool SaveConfig() const {
        return SaveAppConfig(app_config_);
    }

    void ApplyMetricVisibilityChange() {
        UpdateDisplayLines(last_snapshot_);
        RefreshFontAndSize();
        if (embedder_.IsAttached()) {
            if (!embedder_.RefreshLayout(widget_window_, widget_size_)) {
                ShowWindow(widget_window_, SW_HIDE);
                SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
                HideHoverPopup();
                return;
            }
        }
        RequestWidgetRedraw();
        if (hover_popup_visible_) {
            PositionHoverPopup();
            RequestHoverPopupRedraw();
        }
    }

    bool ToggleMetricVisibility(UINT command_id) {
        bool* target = nullptr;
        switch (command_id) {
        case kMetricCpuCommandId:
            target = &app_config_.visible_metrics.show_cpu;
            break;
        case kMetricMemoryCommandId:
            target = &app_config_.visible_metrics.show_memory;
            break;
        case kMetricUploadCommandId:
            target = &app_config_.visible_metrics.show_upload;
            break;
        case kMetricDownloadCommandId:
            target = &app_config_.visible_metrics.show_download;
            break;
        case kMetricGpuCommandId:
            target = &app_config_.visible_metrics.show_gpu;
            break;
        case kMetricDiskReadCommandId:
            target = &app_config_.visible_metrics.show_disk_read;
            break;
        case kMetricDiskWriteCommandId:
            target = &app_config_.visible_metrics.show_disk_write;
            break;
        default:
            return false;
        }

        if (*target && CountVisibleMetrics(app_config_.visible_metrics) <= 1) {
            MessageBoxW(controller_window_,
                        Text(L"Keep at least one metric visible.", L"请至少保留一个可见指标。"),
                        AppTitle(),
                        MB_OK | MB_ICONINFORMATION);
            return true;
        }

        *target = !*target;
        if (!SaveConfig()) {
            *target = !*target;
            MessageBoxW(controller_window_,
                        ConfigSaveErrorText(),
                        AppTitle(),
                        MB_OK | MB_ICONERROR);
            return true;
        }

        ApplyMetricVisibilityChange();
        return true;
    }

    bool SetNetworkDisplayUnit(NetworkDisplayUnit network_display_unit) {
        if (app_config_.network_display_unit == network_display_unit) {
            return true;
        }

        const NetworkDisplayUnit previous_unit = app_config_.network_display_unit;
        app_config_.network_display_unit = network_display_unit;
        if (!SaveConfig()) {
            app_config_.network_display_unit = previous_unit;
            MessageBoxW(controller_window_,
                        ConfigSaveErrorText(),
                        AppTitle(),
                        MB_OK | MB_ICONERROR);
            return true;
        }

        ApplyMetricVisibilityChange();
        return true;
    }

    bool SetPopupActivationMode(PopupActivationMode popup_activation_mode) {
        if (app_config_.popup_activation_mode == popup_activation_mode) {
            return true;
        }

        const PopupActivationMode previous_mode = app_config_.popup_activation_mode;
        app_config_.popup_activation_mode = popup_activation_mode;
        if (!SaveConfig()) {
            app_config_.popup_activation_mode = previous_mode;
            MessageBoxW(controller_window_,
                        ConfigSaveErrorText(),
                        AppTitle(),
                        MB_OK | MB_ICONERROR);
            return true;
        }

        HideHoverPopup();
        return true;
    }

    bool SetSampleIntervalSeconds(unsigned int sample_interval_seconds) {
        if (!IsSupportedSampleIntervalSeconds(sample_interval_seconds)) {
            sample_interval_seconds = 1;
        }
        if (app_config_.sample_interval_seconds == sample_interval_seconds) {
            return true;
        }

        const unsigned int previous_interval = app_config_.sample_interval_seconds;
        app_config_.sample_interval_seconds = sample_interval_seconds;
        if (!SaveConfig()) {
            app_config_.sample_interval_seconds = previous_interval;
            MessageBoxW(controller_window_,
                        ConfigSaveErrorText(),
                        AppTitle(),
                        MB_OK | MB_ICONERROR);
            return true;
        }

        SetTimer(controller_window_,
                 kSampleTimerId,
                 GetSampleTimerIntervalMs(app_config_.sample_interval_seconds),
                 nullptr);
        SampleAndRefresh();
        return true;
    }

    void HandleWidgetLeftButtonDown() {
        click_popup_started_from_widget_ =
            app_config_.popup_activation_mode == PopupActivationMode::kClick && hover_popup_visible_;
    }

    void HandleWidgetLeftButtonUp() {
        if (app_config_.popup_activation_mode != PopupActivationMode::kClick) {
            click_popup_started_from_widget_ = false;
            return;
        }

        if (click_popup_started_from_widget_) {
            click_popup_started_from_widget_ = false;
            return;
        }

        if (hover_popup_visible_) {
            HideHoverPopup();
            return;
        }

        ShowHoverPopup(true);
    }

    void UpdateLayout() {
        if (!EnsureWidgetWindow()) {
            return;
        }

        if (!embedder_.IsAttached()) {
            ReattachWidget();
            return;
        }

        const UINT previous_dpi = current_dpi_;
        RefreshFontAndSize();
        if (current_dpi_ != previous_dpi) {
            RequestWidgetRedraw();
            if (hover_popup_visible_) {
                PositionHoverPopup();
                RequestHoverPopupRedraw();
            }
        }
        if (!embedder_.RefreshLayout(widget_window_, widget_size_)) {
            ShowWindow(widget_window_, SW_HIDE);
            SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
            HideHoverPopup();
            return;
        }

        if (hover_popup_visible_) {
            PositionHoverPopup();
        }
    }

    void PaintWidget() {
        PAINTSTRUCT paint_struct{};
        HDC dc = BeginPaint(widget_window_, &paint_struct);

        RECT client_rect{};
        GetClientRect(widget_window_, &client_rect);
        DrawWidgetContents(dc, client_rect);
        EndPaint(widget_window_, &paint_struct);
    }

    void ToggleAutoStart() {
        const bool enabled = IsAutoStartEnabled();
        if (!SetAutoStartEnabled(!enabled)) {
            MessageBoxW(controller_window_,
                        Text(L"Unable to update the startup setting.", L"无法更新开机启动设置。"),
                        AppTitle(),
                        MB_OK | MB_ICONERROR);
        }
    }

    void ShowContextMenu(POINT screen_point) {
        HideHoverPopup();
        HMENU menu = CreatePopupMenu();
        HMENU status_menu = CreatePopupMenu();
        HMENU stock_menu = CreatePopupMenu();
        HMENU metrics_menu = CreatePopupMenu();
        HMENU network_units_menu = CreatePopupMenu();
        HMENU popup_mode_menu = CreatePopupMenu();
        HMENU refresh_interval_menu = CreatePopupMenu();
        HMENU stock_symbol_count_menu = CreatePopupMenu();
        HMENU stock_sort_menu = CreatePopupMenu();
        HMENU hotkey_menu = CreatePopupMenu();
        HMENU language_menu = CreatePopupMenu();
        AppendMenuW(metrics_menu,
                    MF_STRING | (app_config_.visible_metrics.show_cpu ? MF_CHECKED : MF_UNCHECKED),
                    kMetricCpuCommandId,
                    Text(L"CPU Usage", L"CPU 占用"));
        AppendMenuW(metrics_menu,
                    MF_STRING |
                        (app_config_.visible_metrics.show_memory ? MF_CHECKED : MF_UNCHECKED),
                    kMetricMemoryCommandId,
                    Text(L"Memory", L"内存"));
        AppendMenuW(metrics_menu,
                    MF_STRING |
                        (app_config_.visible_metrics.show_upload ? MF_CHECKED : MF_UNCHECKED),
                    kMetricUploadCommandId,
                    Text(L"Upload Speed", L"上传速度"));
        AppendMenuW(metrics_menu,
                    MF_STRING |
                        (app_config_.visible_metrics.show_download ? MF_CHECKED : MF_UNCHECKED),
                    kMetricDownloadCommandId,
                    Text(L"Download Speed", L"下载速度"));
        AppendMenuW(metrics_menu,
                    MF_STRING | (app_config_.visible_metrics.show_gpu ? MF_CHECKED : MF_UNCHECKED),
                    kMetricGpuCommandId,
                    Text(L"GPU Usage", L"GPU 占用"));
        AppendMenuW(metrics_menu,
                    MF_STRING |
                        (app_config_.visible_metrics.show_disk_read ? MF_CHECKED : MF_UNCHECKED),
                    kMetricDiskReadCommandId,
                    Text(L"Disk Read", L"磁盘读取"));
        AppendMenuW(metrics_menu,
                    MF_STRING |
                        (app_config_.visible_metrics.show_disk_write ? MF_CHECKED : MF_UNCHECKED),
                    kMetricDiskWriteCommandId,
                    Text(L"Disk Write", L"磁盘写入"));
        AppendMenuW(network_units_menu,
                    MF_STRING |
                        (app_config_.network_display_unit == NetworkDisplayUnit::kBitsPerSecond
                             ? MF_CHECKED
                             : MF_UNCHECKED),
                    kNetworkUnitsBitsCommandId,
                    Text(L"Bits/sec (Task Manager style)", L"位/秒（任务管理器风格）"));
        AppendMenuW(network_units_menu,
                    MF_STRING |
                        (app_config_.network_display_unit == NetworkDisplayUnit::kBytesPerSecond
                             ? MF_CHECKED
                             : MF_UNCHECKED),
                    kNetworkUnitsBytesCommandId,
                    Text(L"Bytes/sec (KB/s, MB/s)", L"字节/秒（KB/s, MB/s）"));
        AppendMenuW(popup_mode_menu,
                    MF_STRING |
                        (app_config_.popup_activation_mode == PopupActivationMode::kHover
                             ? MF_CHECKED
                             : MF_UNCHECKED),
                    kPopupModeHoverCommandId,
                    Text(L"Hover popup", L"悬停弹窗"));
        AppendMenuW(popup_mode_menu,
                    MF_STRING |
                        (app_config_.popup_activation_mode == PopupActivationMode::kClick
                             ? MF_CHECKED
                             : MF_UNCHECKED),
                    kPopupModeClickCommandId,
                    Text(L"Click popup", L"点击弹窗"));
        AppendMenuW(refresh_interval_menu,
                    MF_STRING |
                        (app_config_.sample_interval_seconds == 1 ? MF_CHECKED : MF_UNCHECKED),
                    kSampleInterval1sCommandId,
                    Text(L"1 second", L"1 秒"));
        AppendMenuW(refresh_interval_menu,
                    MF_STRING |
                        (app_config_.sample_interval_seconds == 2 ? MF_CHECKED : MF_UNCHECKED),
                    kSampleInterval2sCommandId,
                    Text(L"2 seconds", L"2 秒"));
        AppendMenuW(refresh_interval_menu,
                    MF_STRING |
                        (app_config_.sample_interval_seconds == 5 ? MF_CHECKED : MF_UNCHECKED),
                    kSampleInterval5sCommandId,
                    Text(L"5 seconds", L"5 秒"));
        AppendMenuW(refresh_interval_menu,
                    MF_STRING |
                        (app_config_.sample_interval_seconds == 10 ? MF_CHECKED : MF_UNCHECKED),
                    kSampleInterval10sCommandId,
                    Text(L"10 seconds", L"10 秒"));

        AppendMenuW(stock_symbol_count_menu,
                    MF_STRING |
                        (stock_config_.taskbar_symbol_count == 2 ? MF_CHECKED : MF_UNCHECKED),
                    kStockSymbols2CommandId,
                    Text(L"2 symbols", L"2 个股票"));
        AppendMenuW(stock_symbol_count_menu,
                    MF_STRING |
                        (stock_config_.taskbar_symbol_count == 4 ? MF_CHECKED : MF_UNCHECKED),
                    kStockSymbols4CommandId,
                    Text(L"4 symbols", L"4 个股票"));
        AppendMenuW(stock_symbol_count_menu,
                    MF_STRING |
                        (stock_config_.taskbar_symbol_count == 6 ? MF_CHECKED : MF_UNCHECKED),
                    kStockSymbols6CommandId,
                    Text(L"6 symbols", L"6 个股票"));
        AppendMenuW(stock_symbol_count_menu,
                    MF_STRING |
                        (stock_config_.taskbar_symbol_count == 8 ? MF_CHECKED : MF_UNCHECKED),
                    kStockSymbols8CommandId,
                    Text(L"8 symbols", L"8 个股票"));

        AppendMenuW(stock_sort_menu,
                    MF_STRING |
                        (stock_config_.sort_mode ==
                                 stock_taskbar_monitor::StockSortMode::kConfigOrder
                             ? MF_CHECKED
                             : MF_UNCHECKED),
                    kStockSortConfigCommandId,
                    Text(L"Config order", L"配置顺序"));
        AppendMenuW(stock_sort_menu,
                    MF_STRING |
                        (stock_config_.sort_mode ==
                                 stock_taskbar_monitor::StockSortMode::kTopGainers
                             ? MF_CHECKED
                             : MF_UNCHECKED),
                    kStockSortGainersCommandId,
                    Text(L"Top gainers", L"涨幅优先"));
        AppendMenuW(stock_sort_menu,
                    MF_STRING |
                        (stock_config_.sort_mode ==
                                 stock_taskbar_monitor::StockSortMode::kTopLosers
                             ? MF_CHECKED
                             : MF_UNCHECKED),
                    kStockSortLosersCommandId,
                    Text(L"Top losers", L"跌幅优先"));

        AppendMenuW(hotkey_menu,
                    MF_STRING |
                        (app_config_.toggle_hotkey == ToggleHotkey::kAltQ ? MF_CHECKED
                                                                           : MF_UNCHECKED),
                    kHotkeyAltQCommandId,
                    L"Alt+Q");
        AppendMenuW(hotkey_menu,
                    MF_STRING |
                        (app_config_.toggle_hotkey == ToggleHotkey::kAltS ? MF_CHECKED
                                                                           : MF_UNCHECKED),
                    kHotkeyAltSCommandId,
                    L"Alt+S");
        AppendMenuW(hotkey_menu,
                    MF_STRING |
                        (app_config_.toggle_hotkey == ToggleHotkey::kAltM ? MF_CHECKED
                                                                           : MF_UNCHECKED),
                    kHotkeyAltMCommandId,
                    L"Alt+M");
        AppendMenuW(hotkey_menu,
                    MF_STRING |
                        (app_config_.toggle_hotkey == ToggleHotkey::kCtrlAltQ ? MF_CHECKED
                                                                               : MF_UNCHECKED),
                    kHotkeyCtrlAltQCommandId,
                    L"Ctrl+Alt+Q");

        AppendMenuW(language_menu,
                    MF_STRING |
                        (app_config_.language == UiLanguage::kEnglish ? MF_CHECKED : MF_UNCHECKED),
                    kLanguageEnglishCommandId,
                    L"English");
        AppendMenuW(language_menu,
                    MF_STRING |
                        (app_config_.language == UiLanguage::kChinese ? MF_CHECKED : MF_UNCHECKED),
                    kLanguageChineseCommandId,
                    L"中文");

        AppendMenuW(status_menu,
                    MF_STRING | (!stock_mode_ ? MF_CHECKED : MF_UNCHECKED),
                    kModeStatusCommandId,
                    Text(L"Show status monitor", L"显示状态监视器"));
        AppendMenuW(status_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(status_menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(metrics_menu),
                    Text(L"Visible Metrics", L"显示指标"));
        AppendMenuW(status_menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(network_units_menu),
                    Text(L"Network Units", L"网络单位"));
        AppendMenuW(status_menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(popup_mode_menu),
                    Text(L"Popup Mode", L"弹窗模式"));
        AppendMenuW(status_menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(refresh_interval_menu),
                    Text(L"Refresh Interval", L"刷新间隔"));

        AppendMenuW(stock_menu,
                    MF_STRING | (stock_mode_ ? MF_CHECKED : MF_UNCHECKED),
                    kModeStockCommandId,
                    Text(L"Show stock monitor", L"显示股票监视器"));
        AppendMenuW(stock_menu, MF_STRING, kReloadStockConfigCommandId,
                    Text(L"Reload Stock Config", L"重载股票配置"));
        AppendMenuW(stock_menu, MF_STRING, kOpenStockConfigDialogCommandId,
                    Text(L"Configure Stocks...", L"配置股票..."));
        AppendMenuW(stock_menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(stock_symbol_count_menu),
                    Text(L"Taskbar Symbols", L"任务栏股票数量"));
        AppendMenuW(stock_menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(stock_sort_menu),
                    Text(L"Sort", L"排序"));

        AppendMenuW(menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(status_menu),
                    Text(L"Status", L"状态"));
        AppendMenuW(menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(stock_menu),
                    Text(L"Stock", L"股票"));
        AppendMenuW(menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(hotkey_menu),
                    Text(L"Hotkey", L"快捷键"));
        AppendMenuW(menu,
                    MF_POPUP,
                    reinterpret_cast<UINT_PTR>(language_menu),
                    Text(L"Language", L"语言"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const UINT auto_start_flags =
            MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED);
        AppendMenuW(menu, auto_start_flags, kAutoStartCommandId,
                    Text(L"Launch at startup", L"开机启动"));
        AppendMenuW(menu, MF_STRING, kHelpCommandId, Text(L"Help", L"帮助"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kExitCommandId, Text(L"Exit", L"退出"));

        SetForegroundWindow(controller_window_ != nullptr ? controller_window_ : widget_window_);
        const UINT command =
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_point.x, screen_point.y, 0,
                           controller_window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(controller_window_, WM_NULL, 0, 0);

        if (ToggleMetricVisibility(command)) {
            return;
        }
        if (command == kModeStatusCommandId) {
            SetMonitorMode(false);
            return;
        }
        if (command == kModeStockCommandId) {
            SetMonitorMode(true);
            return;
        }
        if (command == kReloadStockConfigCommandId) {
            ReloadStockConfig();
            return;
        }
        if (command == kOpenStockConfigDialogCommandId) {
            ShowStockConfigDialog();
            return;
        }
        if (command == kStockSymbols2CommandId) {
            SetStockTaskbarSymbolCount(2);
            return;
        }
        if (command == kStockSymbols4CommandId) {
            SetStockTaskbarSymbolCount(4);
            return;
        }
        if (command == kStockSymbols6CommandId) {
            SetStockTaskbarSymbolCount(6);
            return;
        }
        if (command == kStockSymbols8CommandId) {
            SetStockTaskbarSymbolCount(8);
            return;
        }
        if (command == kStockSortConfigCommandId) {
            SetStockSortMode(stock_taskbar_monitor::StockSortMode::kConfigOrder);
            return;
        }
        if (command == kStockSortGainersCommandId) {
            SetStockSortMode(stock_taskbar_monitor::StockSortMode::kTopGainers);
            return;
        }
        if (command == kStockSortLosersCommandId) {
            SetStockSortMode(stock_taskbar_monitor::StockSortMode::kTopLosers);
            return;
        }
        if (command == kHotkeyAltQCommandId) {
            SetToggleHotkey(ToggleHotkey::kAltQ);
            return;
        }
        if (command == kHotkeyAltSCommandId) {
            SetToggleHotkey(ToggleHotkey::kAltS);
            return;
        }
        if (command == kHotkeyAltMCommandId) {
            SetToggleHotkey(ToggleHotkey::kAltM);
            return;
        }
        if (command == kHotkeyCtrlAltQCommandId) {
            SetToggleHotkey(ToggleHotkey::kCtrlAltQ);
            return;
        }
        if (command == kLanguageEnglishCommandId) {
            SetLanguage(UiLanguage::kEnglish);
            return;
        }
        if (command == kLanguageChineseCommandId) {
            SetLanguage(UiLanguage::kChinese);
            return;
        }
        if (command == kNetworkUnitsBitsCommandId) {
            SetNetworkDisplayUnit(NetworkDisplayUnit::kBitsPerSecond);
            return;
        }
        if (command == kNetworkUnitsBytesCommandId) {
            SetNetworkDisplayUnit(NetworkDisplayUnit::kBytesPerSecond);
            return;
        }
        if (command == kPopupModeHoverCommandId) {
            SetPopupActivationMode(PopupActivationMode::kHover);
            return;
        }
        if (command == kPopupModeClickCommandId) {
            SetPopupActivationMode(PopupActivationMode::kClick);
            return;
        }
        if (command == kSampleInterval1sCommandId) {
            SetSampleIntervalSeconds(1);
            return;
        }
        if (command == kSampleInterval2sCommandId) {
            SetSampleIntervalSeconds(2);
            return;
        }
        if (command == kSampleInterval5sCommandId) {
            SetSampleIntervalSeconds(5);
            return;
        }
        if (command == kSampleInterval10sCommandId) {
            SetSampleIntervalSeconds(10);
            return;
        }
        if (command == kAutoStartCommandId) {
            ToggleAutoStart();
            return;
        }
        if (command == kHelpCommandId) {
            ShowHelpDialog();
            return;
        }
        if (command == kExitCommandId) {
            PostMessageW(controller_window_, WM_CLOSE, 0, 0);
        }
    }

    bool EnsureGdiplus() {
        if (gdiplus_started_) {
            return true;
        }

        Gdiplus::GdiplusStartupInput startup_input;
        const Gdiplus::Status status =
            Gdiplus::GdiplusStartup(&gdiplus_token_, &startup_input, nullptr);
        gdiplus_started_ = status == Gdiplus::Ok;
        if (!gdiplus_started_) {
            gdiplus_token_ = 0;
        }
        return gdiplus_started_;
    }

    void ShutdownGdiplus() {
        if (!gdiplus_started_) {
            return;
        }

        Gdiplus::GdiplusShutdown(gdiplus_token_);
        gdiplus_started_ = false;
        gdiplus_token_ = 0;
    }

    std::unique_ptr<Gdiplus::Bitmap> LoadLogoBitmapFromResource() {
        if (!EnsureGdiplus()) {
            return nullptr;
        }

        HRSRC resource_info =
            FindResourceW(instance_handle_, MAKEINTRESOURCEW(IDR_APP_LOGO_PNG), RT_RCDATA);
        if (resource_info == nullptr) {
            return nullptr;
        }

        const DWORD resource_size = SizeofResource(instance_handle_, resource_info);
        if (resource_size == 0) {
            return nullptr;
        }

        HGLOBAL loaded_resource = LoadResource(instance_handle_, resource_info);
        if (loaded_resource == nullptr) {
            return nullptr;
        }

        const void* resource_data = LockResource(loaded_resource);
        if (resource_data == nullptr) {
            return nullptr;
        }

        HGLOBAL resource_copy = GlobalAlloc(GMEM_MOVEABLE, resource_size);
        if (resource_copy == nullptr) {
            return nullptr;
        }

        void* copy_data = GlobalLock(resource_copy);
        if (copy_data == nullptr) {
            GlobalFree(resource_copy);
            return nullptr;
        }

        memcpy(copy_data, resource_data, resource_size);
        GlobalUnlock(resource_copy);

        IStream* stream = nullptr;
        if (CreateStreamOnHGlobal(resource_copy, TRUE, &stream) != S_OK) {
            GlobalFree(resource_copy);
            return nullptr;
        }

        std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromStream(stream, FALSE));
        stream->Release();
        if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
            return nullptr;
        }

        return bitmap;
    }

    HICON CreateIconFromLogo() {
        std::unique_ptr<Gdiplus::Bitmap> source_bitmap = LoadLogoBitmapFromResource();
        if (!source_bitmap) {
            return nullptr;
        }

        Gdiplus::Rect content_bounds{};
        if (!FindContentBounds(*source_bitmap, content_bounds)) {
            content_bounds =
                Gdiplus::Rect(0, 0, static_cast<INT>(source_bitmap->GetWidth()),
                              static_cast<INT>(source_bitmap->GetHeight()));
        }

        const int icon_size = std::max({GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 32});
        const int padding = std::max(2, icon_size / 12);
        const int available_width = std::max(1, icon_size - padding * 2);
        const int available_height = std::max(1, icon_size - padding * 2);
        const double scale =
            std::min(static_cast<double>(available_width) / std::max(content_bounds.Width, 1),
                     static_cast<double>(available_height) / std::max(content_bounds.Height, 1));
        const int draw_width = std::max(1, static_cast<int>(content_bounds.Width * scale));
        const int draw_height = std::max(1, static_cast<int>(content_bounds.Height * scale));
        const int offset_x = (icon_size - draw_width) / 2;
        const int offset_y = (icon_size - draw_height) / 2;

        Gdiplus::Bitmap icon_bitmap(icon_size, icon_size, PixelFormat32bppARGB);
        Gdiplus::Graphics graphics(&icon_bitmap);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

        Gdiplus::ImageAttributes image_attributes;
        image_attributes.SetColorKey(Gdiplus::Color(245, 245, 245),
                                     Gdiplus::Color(255, 255, 255),
                                     Gdiplus::ColorAdjustTypeBitmap);

        const Gdiplus::Rect destination_rect(offset_x, offset_y, draw_width, draw_height);
        if (graphics.DrawImage(source_bitmap.get(),
                               destination_rect,
                               content_bounds.X,
                               content_bounds.Y,
                               content_bounds.Width,
                               content_bounds.Height,
                               Gdiplus::UnitPixel,
                               &image_attributes) != Gdiplus::Ok) {
            return nullptr;
        }

        HICON icon_handle = nullptr;
        if (icon_bitmap.GetHICON(&icon_handle) != Gdiplus::Ok) {
            return nullptr;
        }

        return icon_handle;
    }

    HICON LoadTrayIcon() {
        HICON icon_handle = CreateIconFromLogo();
        if (icon_handle != nullptr) {
            return icon_handle;
        }

        HICON fallback_icon = LoadIconW(nullptr, IDI_APPLICATION);
        return fallback_icon != nullptr ? CopyIcon(fallback_icon) : nullptr;
    }

    static MonitorApp* FromWindow(HWND window_handle) {
        return reinterpret_cast<MonitorApp*>(GetWindowLongPtrW(window_handle, GWLP_USERDATA));
    }

    static LRESULT CALLBACK ControllerWindowProc(HWND window_handle,
                                                 UINT message,
                                                 WPARAM w_param,
                                                 LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->controller_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }

        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        if (message == app->taskbar_created_message_) {
            app->HideHoverPopup();
            app->embedder_.Detach(app->widget_window_);
            app->tray_icon_added_ = false;
            SetTimer(window_handle, kReattachTimerId, kReattachDelayMs, nullptr);
            return 0;
        }

        switch (message) {
        case WM_CREATE:
            return app->Initialize() ? 0 : -1;
        case WM_HOTKEY:
            if (w_param == kToggleModeHotkeyId) {
                app->ToggleMonitorMode();
                return 0;
            }
            break;
        case WM_TIMER:
            if (w_param == kSampleTimerId) {
                app->SampleAndRefresh();
                return 0;
            }
            if (w_param == kLayoutTimerId) {
                app->UpdateLayout();
                return 0;
            }
            if (w_param == kReattachTimerId) {
                KillTimer(window_handle, kReattachTimerId);
                app->EnsureTrayIcon();
                app->ReattachWidget();
                return 0;
            }
            if (w_param == kHoverHideTimerId) {
                KillTimer(window_handle, kHoverHideTimerId);
                if (!app->IsCursorOverWidgetOrPopup()) {
                    app->HideHoverPopup();
                }
                return 0;
            }
            break;
        case kTrayIconCallbackMessage:
            if (l_param == WM_RBUTTONUP || l_param == WM_CONTEXTMENU) {
                POINT point{};
                GetCursorPos(&point);
                app->ShowContextMenu(point);
                return 0;
            }
            break;
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            app->RefreshFontAndSize();
            if (app->widget_window_ != nullptr && IsWindow(app->widget_window_)) {
                app->RequestWidgetRedraw();
            }
            if (app->hover_popup_visible_) {
                app->PositionHoverPopup();
                app->RequestHoverPopupRedraw();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(window_handle);
            return 0;
        case WM_DESTROY:
            app->Shutdown();
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }

        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    static LRESULT CALLBACK WidgetWindowProc(HWND window_handle,
                                             UINT message,
                                             WPARAM w_param,
                                             LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->widget_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }

        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            app->PaintWidget();
            return 0;
        case WM_MOUSEMOVE:
            app->HandleHoverMove(window_handle);
            return 0;
        case WM_MOUSELEAVE:
            app->ArmHoverHideTimer();
            return 0;
        case WM_LBUTTONDOWN:
            app->HandleWidgetLeftButtonDown();
            return 0;
        case WM_LBUTTONUP:
            app->HandleWidgetLeftButtonUp();
            return 0;
        case WM_RBUTTONUP: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ClientToScreen(window_handle, &point);
            app->ShowContextMenu(point);
            return 0;
        }
        case WM_CONTEXTMENU: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            if (point.x == -1 && point.y == -1) {
                RECT rect{};
                GetWindowRect(window_handle, &rect);
                point.x = rect.left;
                point.y = rect.bottom;
            }
            app->ShowContextMenu(point);
            return 0;
        }
        case WM_NCDESTROY:
            if (app->widget_window_ == window_handle) {
                app->HideHoverPopup();
                app->widget_window_ = nullptr;
                if (!app->is_shutting_down_ && app->controller_window_ != nullptr &&
                    IsWindow(app->controller_window_)) {
                    SetTimer(app->controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
                }
            }
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }

        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    static LRESULT CALLBACK HoverPopupWindowProc(HWND window_handle,
                                                 UINT message,
                                                 WPARAM w_param,
                                                 LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->hover_popup_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }

        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            app->PaintHoverPopup();
            return 0;
        case WM_VSCROLL:
            app->HandleHoverPopupVScroll(LOWORD(w_param), HIWORD(w_param));
            return 0;
        case WM_MOUSEWHEEL:
            app->HandleHoverPopupMouseWheel(GET_WHEEL_DELTA_WPARAM(w_param));
            return 0;
        case WM_MOUSEMOVE:
            app->HandleHoverMove(window_handle);
            return 0;
        case WM_MOUSELEAVE:
            app->ArmHoverHideTimer();
            return 0;
        case WM_KEYDOWN:
            if (app->stock_mode_) {
                return 0;
            }
            if (app->HandleHoverPopupSearchKeyDown(w_param)) {
                return 0;
            }
            break;
        case WM_CHAR:
            if (app->stock_mode_) {
                return 0;
            }
            if (app->HandleHoverPopupSearchChar(w_param)) {
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            app->SetHoverPopupSearchActive(false);
            if (app->app_config_.popup_activation_mode == PopupActivationMode::kClick) {
                app->HideHoverPopup();
            } else {
                app->ArmHoverHideTimer();
            }
            return 0;
        case WM_LBUTTONUP: {
            if (app->stock_mode_) {
                return 0;
            }
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            app->HandleHoverPopupClick(point);
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ClientToScreen(window_handle, &point);
            app->ShowContextMenu(point);
            return 0;
        }
        case WM_CONTEXTMENU: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            if (point.x == -1 && point.y == -1) {
                RECT rect{};
                GetWindowRect(window_handle, &rect);
                point.x = rect.left;
                point.y = rect.bottom;
            }
            app->ShowContextMenu(point);
            return 0;
        }
        case WM_NCDESTROY:
            if (app->hover_popup_window_ == window_handle) {
                app->hover_popup_window_ = nullptr;
                app->hover_popup_visible_ = false;
            }
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }

        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    HINSTANCE instance_handle_{nullptr};
    HWND controller_window_{nullptr};
    HWND widget_window_{nullptr};
    HWND hover_popup_window_{nullptr};
    HFONT font_{nullptr};
    HFONT popup_font_{nullptr};
    HFONT popup_title_font_{nullptr};
    UINT current_dpi_{96};
    UINT taskbar_created_message_{0};
    SIZE widget_size_{};
    SIZE hover_popup_size_{};
    int text_line_height_{0};
    int popup_text_line_height_{0};
    int popup_title_line_height_{0};
    bool has_second_line_{true};
    bool is_shutting_down_{false};
    bool hover_popup_visible_{false};
    bool hover_popup_search_active_{false};
    bool click_popup_started_from_widget_{false};
    bool tray_icon_added_{false};
    HICON tray_icon_handle_{nullptr};
    ULONG_PTR gdiplus_token_{0};
    bool gdiplus_started_{false};
    ULONGLONG app_start_tick_ms_{GetTickCount64()};
    AppConfig app_config_{};
    bool stock_mode_{false};
    stock_taskbar_monitor::AppConfig stock_config_{};
    MetricsSnapshot last_snapshot_{};
    ProcessPopupSnapshot hover_popup_base_snapshot_{};
    ProcessPopupSnapshot hover_popup_snapshot_{};
    HoverPopupSortMode hover_popup_sort_mode_{HoverPopupSortMode::kDefault};
    int hover_popup_scroll_offset_{0};
    std::wstring hover_popup_search_text_{};
    std::wstring line1_text_{L"CPU 0%  MEM 0%"};
    std::wstring line2_text_{L"\u2191 0bps  \u2193 0bps  R 0B/s  W 0B/s"};
    std::wstring stock_last_update_time_{L"--:--:--"};
    std::vector<StockRow> stock_rows_{};
    std::set<std::wstring> stock_active_alerts_{};
    std::vector<DisplayLines::Column> display_columns_{};
    std::vector<int> column_widths_{};
    ProcessMonitor process_monitor_{};
    SystemMetrics metrics_{};
    TaskbarEmbedder embedder_{};
};

}  // namespace minimal_taskbar_monitor

int APIENTRY wWinMain(HINSTANCE instance_handle,
                      HINSTANCE,
                      LPWSTR,
                      int show_command) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    minimal_taskbar_monitor::MonitorApp app;
    return app.Run(instance_handle, show_command);
}
