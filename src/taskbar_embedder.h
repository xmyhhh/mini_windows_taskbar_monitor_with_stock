#pragma once

#include <windows.h>

#include <vector>

namespace minimal_taskbar_monitor {

struct TaskbarDisplayInfo {
    unsigned int index{};
    RECT rect{};
    bool primary{};
    bool has_taskbar{};
};

class TaskbarEmbedder {
public:
    TaskbarEmbedder() = default;
    ~TaskbarEmbedder() = default;

    void SetTargetMonitorIndex(unsigned int monitor_index);
    unsigned int TargetMonitorIndex() const;
    static std::vector<TaskbarDisplayInfo> EnumerateDisplays();

    bool Attach(HWND widget_window);
    void Detach(HWND widget_window);
    bool RefreshLayout(HWND widget_window, const SIZE& desired_size);
    UINT CurrentDpi() const;
    bool IsAttached() const;

private:
    enum class Mode {
        kNone,
        kWin10Classic,
        kWin11
    };

    bool ResolveHandles();
    bool ResolveTaskbarForTargetMonitor();
    bool ResolveClassicHandles();
    bool ResolveWin11Handles();
    bool FindTaskbarForMonitor(HMONITOR monitor);
    bool IsPrimaryTaskbar() const;
    bool LayoutClassic(HWND widget_window, const SIZE& desired_size, UINT taskbar_edge);
    bool LayoutNearTray(HWND widget_window, const SIZE& desired_size);
    void RestoreClassicReservation();
    bool IsHandleAlive(HWND window_handle) const;
    UINT QueryTaskbarEdge() const;

    Mode mode_{Mode::kNone};
    unsigned int target_monitor_index_{0};
    HWND taskbar_window_{nullptr};
    HWND parent_window_{nullptr};
    HWND task_list_window_{nullptr};
    HWND tray_notify_window_{nullptr};
    HWND start_button_window_{nullptr};
    RECT original_task_list_rect_{};
    int classic_left_space_{0};
    int classic_top_space_{0};
    int last_task_list_width_{0};
    int last_task_list_height_{0};
    SIZE last_desired_size_{};
};

}  // namespace minimal_taskbar_monitor
