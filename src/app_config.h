#pragma once

#include "system_metrics.h"

#include <string>

namespace minimal_taskbar_monitor {

enum class PopupActivationMode {
    kHover,
    kClick,
};

enum class ToggleHotkey {
    kAltQ,
    kAltS,
    kAltM,
    kCtrlAltQ,
};

enum class UiLanguage {
    kEnglish,
    kChinese,
};

struct AppConfig {
    MetricVisibility visible_metrics{};
    NetworkDisplayUnit network_display_unit{NetworkDisplayUnit::kBitsPerSecond};
    PopupActivationMode popup_activation_mode{PopupActivationMode::kHover};
    ToggleHotkey toggle_hotkey{ToggleHotkey::kAltQ};
    UiLanguage language{UiLanguage::kEnglish};
    unsigned int sample_interval_seconds{1};
    unsigned int taskbar_monitor_index{0};
};

AppConfig LoadAppConfig();
bool SaveAppConfig(const AppConfig& config);
std::wstring GetAppConfigPath();

}  // namespace minimal_taskbar_monitor
