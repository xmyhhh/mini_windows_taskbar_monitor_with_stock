#include "../src/taskbar_embedder.h"

#include <shellapi.h>
#include <winternl.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>

// Simulate display and Explorer changes without touching the user's desktop.
namespace fake {

struct Display {
    HMONITOR handle;
    RECT rect;
    std::wstring device;
    bool primary;
};
struct Window {
    std::wstring class_name;
    RECT rect;
    HWND parent{};
    LONG_PTR style{WS_POPUP};
};
std::vector<Display> displays;
std::map<HWND, Window> windows;

BOOL EnumMonitors(HDC, LPCRECT, MONITORENUMPROC callback, LPARAM data) {
    for (const auto& display : displays) {
        RECT rect = display.rect;
        if (!callback(display.handle, nullptr, &rect, data)) {
            break;
        }
    }
    return TRUE;
}
BOOL MonitorInfo(HMONITOR monitor, LPMONITORINFO info) {
    for (const auto& display : displays) {
        if (monitor == display.handle) {
            info->rcMonitor = info->rcWork = display.rect;
            info->dwFlags = display.primary ? MONITORINFOF_PRIMARY : 0;
            if (info->cbSize == sizeof(MONITORINFOEXW)) {
                wcscpy_s(reinterpret_cast<MONITORINFOEXW*>(info)->szDevice, display.device.c_str());
            }
            return TRUE;
        }
    }
    return FALSE;
}
HMONITOR FromRect(LPCRECT rect, DWORD flags) {
    for (const auto& display : displays) {
        RECT overlap{};
        if (IntersectRect(&overlap, rect, &display.rect)) {
            return display.handle;
        }
    }
    if (flags != MONITOR_DEFAULTTONULL && !displays.empty()) {
        return displays.front().handle;
    }
    return nullptr;
}
BOOL WindowRect(HWND window, LPRECT rect) {
    const auto found = windows.find(window);
    if (found == windows.end()) {
        return FALSE;
    }
    *rect = found->second.rect;
    return TRUE;
}
BOOL Alive(HWND window) { return windows.contains(window); }
HWND Find(LPCWSTR class_name, LPCWSTR) {
    for (const auto& [handle, window] : windows) {
        if (window.class_name == class_name) {
            return handle;
        }
    }
    return nullptr;
}
HWND FindEx(HWND parent, HWND after, LPCWSTR class_name, LPCWSTR) {
    if (parent != nullptr) {
        return nullptr;
    }
    auto next = after == nullptr ? windows.begin() : windows.upper_bound(after);
    for (; next != windows.end(); ++next) {
        if (next->second.class_name == class_name) {
            return next->first;
        }
    }
    return nullptr;
}
HWND Parent(HWND window) { return windows.at(window).parent; }
HWND Reparent(HWND window, HWND parent) {
    HWND old = windows.at(window).parent;
    windows.at(window).parent = parent;
    return old;
}
LONG_PTR Style(HWND window, int) { return windows.at(window).style; }
LONG_PTR SetStyle(HWND window, int, LONG_PTR style) {
    LONG_PTR old = windows.at(window).style;
    windows.at(window).style = style;
    return old;
}
BOOL Position(HWND window, HWND, int x, int y, int width, int height, UINT flags) {
    if (!Alive(window)) {
        return FALSE;
    }
    auto& state = windows.at(window);
    if ((flags & SWP_NOMOVE) == 0) {
        OffsetRect(&state.rect, x - state.rect.left, y - state.rect.top);
    }
    if ((flags & SWP_NOSIZE) == 0) {
        state.rect.right = state.rect.left + width;
        state.rect.bottom = state.rect.top + height;
    }
    return TRUE;
}
BOOL Move(HWND window, int x, int y, int width, int height, BOOL) {
    return Position(window, nullptr, x, y, width, height, 0);
}
UINT Dpi(HWND) { return 96; }
UINT_PTR AppBar(DWORD, PAPPBARDATA) { return 0; }
LONG WINAPI Version(PRTL_OSVERSIONINFOW version) {
    version->dwMajorVersion = 10;
    version->dwBuildNumber = 22000;
    return 0;
}
FARPROC Proc(HMODULE, LPCSTR) { return reinterpret_cast<FARPROC>(&Version); }

}  // namespace fake

// Compile the implementation with simulated Win32 calls to exercise real recovery logic.
#define EnumDisplayMonitors fake::EnumMonitors
#define GetMonitorInfoW fake::MonitorInfo
#define MonitorFromRect fake::FromRect
#define GetWindowRect fake::WindowRect
#define IsWindow fake::Alive
#define FindWindowW fake::Find
#define FindWindowExW fake::FindEx
#define GetParent fake::Parent
#define SetParent fake::Reparent
#define GetWindowLongPtrW fake::Style
#define SetWindowLongPtrW fake::SetStyle
#define SetWindowPos fake::Position
#define MoveWindow fake::Move
#define GetDpiForWindow fake::Dpi
#define SHAppBarMessage fake::AppBar
#define GetProcAddress fake::Proc
#include "../src/taskbar_embedder.cpp"

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        const auto monitor1 = reinterpret_cast<HMONITOR>(101);
        const auto monitor2 = reinterpret_cast<HMONITOR>(102);
        const auto monitor3 = reinterpret_cast<HMONITOR>(103);
        const auto taskbar1 = reinterpret_cast<HWND>(1);
        const auto taskbar2 = reinterpret_cast<HWND>(2);
        const auto taskbar3 = reinterpret_cast<HWND>(3);
        const auto widget = reinterpret_cast<HWND>(201);
        const auto new_widget = reinterpret_cast<HWND>(202);
        const auto new_taskbar = reinterpret_cast<HWND>(4);
        const SIZE size{200, 40};
        const fake::Display first{monitor1, {0, 0, 1920, 1080}, L"DISPLAY1", true};
        const fake::Display second{monitor2, {1920, 0, 3840, 1080}, L"DISPLAY2", false};
        const fake::Display third{monitor3, {3840, 0, 5760, 1080}, L"DISPLAY3", false};
        fake::displays = {first, second, third};
        fake::windows[taskbar1] = {L"Shell_TrayWnd", {0, 1040, 1920, 1080}};
        fake::windows[taskbar2] = {L"Shell_SecondaryTrayWnd", {1920, 1040, 3840, 1080}};
        fake::windows[taskbar3] = {L"Shell_SecondaryTrayWnd", {3840, 1040, 5760, 1080}};
        fake::windows[widget] = {L"Widget", {0, 0, 200, 40}};

        minimal_taskbar_monitor::TaskbarEmbedder embedder;
        embedder.SetTargetMonitorIndex(2);
        Require(embedder.RefreshLayout(widget, size) && fake::Parent(widget) == taskbar3,
                "Cannot attach to the third display");

        // Removing a different display changes enumeration indices, not the chosen device.
        fake::displays = {first, third};
        Require(embedder.RefreshLayout(widget, size) && fake::Parent(widget) == taskbar3 &&
                    embedder.TargetMonitorIndex() == 1,
                "Display renumbering changed the target monitor");

        // Keep the old taskbar handle alive and off-screen, as Explorer can do during hotplug.
        fake::displays = {first, second};
        Require(embedder.RefreshLayout(widget, size) && fake::Parent(widget) == taskbar1 &&
                    embedder.TargetMonitorIndex() == 0,
                "Disconnecting the selected display did not fall back to the primary taskbar");

        fake::displays = {first, second, third};
        Require(embedder.RefreshLayout(widget, size) && fake::Parent(widget) == taskbar3,
                "Reconnecting the selected display did not restore its taskbar");

        fake::windows.erase(widget);
        fake::windows[new_widget] = {L"Widget", {0, 0, 200, 40}};
        Require(embedder.RefreshLayout(new_widget, size) && fake::Parent(new_widget) == taskbar3,
                "A recreated widget was left unparented while cached taskbar handles were alive");

        fake::windows.erase(taskbar3);
        Require(embedder.RefreshLayout(new_widget, size) && fake::Parent(new_widget) == taskbar1,
                "Missing secondary taskbar did not trigger fallback");
        fake::windows[new_taskbar] = {L"Shell_SecondaryTrayWnd", {3840, 1040, 5760, 1080}};
        Require(embedder.RefreshLayout(new_widget, size) && fake::Parent(new_widget) == new_taskbar,
                "Delayed taskbar recreation did not restore attachment");

        Require(!embedder.RefreshLayout(nullptr, size), "Invalid widget was accepted");
        embedder.Detach(new_widget);
        Require(!embedder.IsAttached() && fake::Parent(new_widget) == nullptr,
                "Detach did not clear attachment state");
        std::cout << "Display disconnect/reconnect, renumbering, stale handles and widget/taskbar recovery passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
