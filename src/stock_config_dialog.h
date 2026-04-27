#pragma once

#include "stock_config.h"

#include <windows.h>

namespace stock_taskbar_monitor {

bool ShowStockConfigDialog(HWND owner,
                           HINSTANCE instance_handle,
                           bool chinese,
                           AppConfig* config);

}  // namespace stock_taskbar_monitor
