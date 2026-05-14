#include "stock_config_dialog.h"

#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace stock_taskbar_monitor {
namespace {

constexpr wchar_t kDialogClassName[] = L"StockTaskbarConfigDialog";
constexpr const wchar_t* kAlpacaFeeds[] = {L"sip", L"iex", L"delayed_sip", L"boats", L"overnight"};

enum ControlId {
    kEndpointEditId = 7101,
    kKeyEditId = 7102,
    kSecretEditId = 7103,
    kFeedEditId = 7104,
    kGroupComboId = 7105,
    kGroupNameEditId = 7106,
    kAddGroupButtonId = 7107,
    kRemoveGroupButtonId = 7108,
    kSortModeComboId = 7109,
    kStockListId = 7110,
    kSymbolEditId = 7111,
    kCodeEditId = 7112,
    kMarketComboId = 7113,
    kShowUsdCheckId = 7114,
    kAddStockButtonId = 7115,
    kRemoveStockButtonId = 7116,
    kMinPriceEditId = 7117,
    kMaxPriceEditId = 7118,
    kMinPriceEnableId = 7119,
    kMaxPriceEnableId = 7120,
    kMoveStockUpButtonId = 7121,
    kMoveStockDownButtonId = 7122,
    kSaveButtonId = 7201,
    kCancelButtonId = 7202,
};

struct DialogState {
    HWND window{};
    HWND endpoint{};
    HWND key{};
    HWND secret{};
    HWND feed{};
    HWND group_combo{};
    HWND group_name{};
    HWND add_group{};
    HWND remove_group{};
    HWND sort_mode{};
    HWND stock_list{};
    HWND symbol{};
    HWND code{};
    HWND market{};
    HWND show_usd{};
    HWND min_price_enabled{};
    HWND min_price{};
    HWND max_price_enabled{};
    HWND max_price{};
    HWND add_stock{};
    HWND remove_stock{};
    HWND move_stock_up{};
    HWND move_stock_down{};
    HWND save{};
    HWND cancel{};
    HFONT font{};
    bool chinese{};
    bool saved{};
    bool syncing{};
    bool editor_has_invalid_number{};
    int selected_group{-1};
    int selected_stock{-1};
    AppConfig* config{};
    std::vector<StockGroup> groups;
    std::vector<StockTarget> stocks;
};

const wchar_t* Text(bool chinese, const wchar_t* english, const wchar_t* chinese_text) {
    return chinese ? chinese_text : english;
}

int Scale(HWND window, int value) {
    const UINT dpi = GetDpiForWindow(window);
    return MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

HWND CreateLabel(DialogState& state, const wchar_t* text, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0,
                                   L"STATIC",
                                   text,
                                   WS_CHILD | WS_VISIBLE,
                                   Scale(state.window, x),
                                   Scale(state.window, y),
                                   Scale(state.window, w),
                                   Scale(state.window, h),
                                   state.window,
                                   nullptr,
                                   nullptr,
                                   nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    return control;
}

HWND CreateGroupBox(DialogState& state, const wchar_t* text, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0,
                                   L"BUTTON",
                                   text,
                                   WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                   Scale(state.window, x),
                                   Scale(state.window, y),
                                   Scale(state.window, w),
                                   Scale(state.window, h),
                                   state.window,
                                   nullptr,
                                   nullptr,
                                   nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    return control;
}

HWND CreateEdit(DialogState& state, int id, int x, int y, int w, int h, DWORD extra_style = 0) {
    HWND control = CreateWindowExW(WS_EX_CLIENTEDGE,
                                   L"EDIT",
                                   L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra_style,
                                   Scale(state.window, x),
                                   Scale(state.window, y),
                                   Scale(state.window, w),
                                   Scale(state.window, h),
                                   state.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr,
                                   nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    return control;
}

HWND CreateButton(DialogState& state, int id, const wchar_t* text, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0,
                                   L"BUTTON",
                                   text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   Scale(state.window, x),
                                   Scale(state.window, y),
                                   Scale(state.window, w),
                                   Scale(state.window, h),
                                   state.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr,
                                   nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    return control;
}

HWND CreateCheckBox(DialogState& state, int id, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0,
                                   L"BUTTON",
                                   L"USD",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   Scale(state.window, x),
                                   Scale(state.window, y),
                                   Scale(state.window, w),
                                   Scale(state.window, h),
                                   state.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr,
                                   nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    return control;
}

HWND CreateCheckBox(DialogState& state,
                    int id,
                    const wchar_t* text,
                    int x,
                    int y,
                    int w,
                    int h) {
    HWND control = CreateWindowExW(0,
                                   L"BUTTON",
                                   text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   Scale(state.window, x),
                                   Scale(state.window, y),
                                   Scale(state.window, w),
                                   Scale(state.window, h),
                                   state.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr,
                                   nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    return control;
}

HWND CreateListBox(DialogState& state, int id, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(WS_EX_CLIENTEDGE,
                                   L"LISTBOX",
                                   L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                       LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                   Scale(state.window, x),
                                   Scale(state.window, y),
                                   Scale(state.window, w),
                                   Scale(state.window, h),
                                   state.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr,
                                   nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    return control;
}

HWND CreateMarketCombo(DialogState& state, int id, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0,
                                   L"COMBOBOX",
                                   L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                       WS_VSCROLL,
                                   Scale(state.window, x),
                                   Scale(state.window, y),
                                   Scale(state.window, w),
                                   Scale(state.window, h),
                                   state.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr,
                                   nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    ComboBox_AddString(control, L"hk");
    ComboBox_AddString(control, L"cn");
    ComboBox_AddString(control, L"us");
    ComboBox_SetCurSel(control, 0);
    return control;
}

HWND CreateComboBox(DialogState& state, int id, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0,
                                   L"COMBOBOX",
                                   L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                       WS_VSCROLL,
                                   Scale(state.window, x),
                                   Scale(state.window, y),
                                   Scale(state.window, w),
                                   Scale(state.window, h),
                                   state.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr,
                                   nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    return control;
}

std::wstring GetWindowString(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(std::max(length, 0) + 1), L'\0');
    if (length > 0) {
        GetWindowTextW(control, value.data(), length + 1);
    }
    value.resize(static_cast<size_t>(std::max(length, 0)));
    return value;
}

std::wstring TrimCopy(const std::wstring& value) {
    size_t first = 0;
    while (first < value.size() && std::iswspace(value[first])) {
        ++first;
    }
    size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1])) {
        --last;
    }
    return value.substr(first, last - first);
}

void SetWindowString(HWND control, const std::wstring& value) {
    SetWindowTextW(control, value.c_str());
}

std::wstring GetComboText(HWND combo) {
    const int index = ComboBox_GetCurSel(combo);
    if (index < 0) {
        return L"hk";
    }
    wchar_t buffer[16]{};
    ComboBox_GetLBText(combo, index, buffer);
    return buffer;
}

std::wstring GetComboText(HWND combo, const wchar_t* fallback) {
    const int index = ComboBox_GetCurSel(combo);
    if (index < 0) {
        return fallback;
    }
    wchar_t buffer[32]{};
    ComboBox_GetLBText(combo, index, buffer);
    return buffer;
}

std::wstring FormatOptionalDouble(std::optional<double> value) {
    if (!value) {
        return L"";
    }
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.4f", *value);
    std::wstring text = buffer;
    while (!text.empty() && text.back() == L'0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == L'.') {
        text.pop_back();
    }
    return text;
}

std::optional<double> ParseOptionalDouble(const std::wstring& text, bool* ok) {
    if (ok != nullptr) {
        *ok = true;
    }
    if (text.empty()) {
        return std::nullopt;
    }
    wchar_t* end = nullptr;
    const double value = wcstod(text.c_str(), &end);
    if (end == text.c_str() || (end != nullptr && *end != L'\0')) {
        if (ok != nullptr) {
            *ok = false;
        }
        return std::nullopt;
    }
    return value;
}

void SelectMarket(HWND combo, const std::wstring& market) {
    const int count = ComboBox_GetCount(combo);
    for (int i = 0; i < count; ++i) {
        wchar_t buffer[16]{};
        ComboBox_GetLBText(combo, i, buffer);
        if (_wcsicmp(buffer, market.c_str()) == 0) {
            ComboBox_SetCurSel(combo, i);
            return;
        }
    }
    ComboBox_SetCurSel(combo, 0);
}

void PopulateAlpacaFeedCombo(HWND combo) {
    for (const wchar_t* feed : kAlpacaFeeds) {
        ComboBox_AddString(combo, feed);
    }
    ComboBox_SetCurSel(combo, 1);
}

void SelectAlpacaFeed(HWND combo, const std::wstring& feed) {
    const int count = ComboBox_GetCount(combo);
    for (int i = 0; i < count; ++i) {
        wchar_t buffer[32]{};
        ComboBox_GetLBText(combo, i, buffer);
        if (_wcsicmp(buffer, feed.c_str()) == 0) {
            ComboBox_SetCurSel(combo, i);
            return;
        }
    }
    ComboBox_SetCurSel(combo, 1);
}

void PopulateSortModeCombo(HWND combo, bool chinese) {
    ComboBox_AddString(combo, Text(chinese, L"Config order", L"配置顺序"));
    ComboBox_AddString(combo, Text(chinese, L"Top gainers", L"涨幅优先"));
    ComboBox_AddString(combo, Text(chinese, L"Top losers", L"跌幅优先"));
    ComboBox_SetCurSel(combo, 0);
}

void SelectSortMode(HWND combo, StockSortMode sort_mode) {
    int index = 0;
    switch (sort_mode) {
    case StockSortMode::kTopGainers:
        index = 1;
        break;
    case StockSortMode::kTopLosers:
        index = 2;
        break;
    case StockSortMode::kConfigOrder:
    default:
        index = 0;
        break;
    }
    ComboBox_SetCurSel(combo, index);
}

StockSortMode GetSelectedSortMode(HWND combo) {
    switch (ComboBox_GetCurSel(combo)) {
    case 1:
        return StockSortMode::kTopGainers;
    case 2:
        return StockSortMode::kTopLosers;
    case 0:
    default:
        return StockSortMode::kConfigOrder;
    }
}

std::wstring StockListLabel(const StockTarget& stock, bool chinese) {
    if (TrimCopy(stock.symbol).empty() && TrimCopy(stock.code).empty()) {
        return Text(chinese, L"New stock", L"新股票");
    }
    std::wstring label = stock.symbol;
    label += L"    ";
    label += stock.code;
    label += L"    ";
    label += stock.market;
    return label;
}

int FindGroupIndexByName(const std::vector<StockGroup>& groups, const std::wstring& name) {
    for (size_t i = 0; i < groups.size(); ++i) {
        if (_wcsicmp(groups[i].name.c_str(), name.c_str()) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool IsBlankStock(const StockTarget& stock) {
    return TrimCopy(stock.symbol).empty() && TrimCopy(stock.code).empty();
}

void NormalizeStockFields(StockTarget& stock) {
    stock.symbol = TrimCopy(stock.symbol);
    stock.code = TrimCopy(stock.code);
    stock.market = TrimCopy(stock.market);
    stock.source = TrimCopy(stock.source);
    stock.alpaca_feed = TrimCopy(stock.alpaca_feed);
    if (stock.market.empty()) {
        stock.market = L"hk";
    }
}

void RemoveBlankStocks(std::vector<StockTarget>& stocks) {
    for (auto& stock : stocks) {
        NormalizeStockFields(stock);
    }
    std::erase_if(stocks, [](const StockTarget& stock) { return IsBlankStock(stock); });
}

std::wstring MakeUniqueGroupName(const std::vector<StockGroup>& groups, bool chinese) {
    const std::wstring prefix = Text(chinese, L"List ", L"列表 ");
    for (int index = 1; index < 1000; ++index) {
        const std::wstring name = prefix + std::to_wstring(index);
        if (FindGroupIndexByName(groups, name) < 0) {
            return name;
        }
    }
    return prefix + L"New";
}

void RefreshGroupCombo(DialogState& state) {
    state.syncing = true;
    ComboBox_ResetContent(state.group_combo);
    for (const auto& group : state.groups) {
        ComboBox_AddString(state.group_combo, group.name.c_str());
    }
    if (!state.groups.empty()) {
        state.selected_group =
            std::clamp(state.selected_group, 0, static_cast<int>(state.groups.size()) - 1);
        ComboBox_SetCurSel(state.group_combo, state.selected_group);
        SetWindowString(state.group_name, state.groups[static_cast<size_t>(state.selected_group)].name);
        EnableWindow(state.add_group, state.groups.size() < kMaxStockGroups ? TRUE : FALSE);
        EnableWindow(state.remove_group, state.groups.size() > 1 ? TRUE : FALSE);
    } else {
        state.selected_group = -1;
        SetWindowString(state.group_name, L"");
        EnableWindow(state.add_group, TRUE);
        EnableWindow(state.remove_group, FALSE);
    }
    state.syncing = false;
}

void CommitCurrentGroup(DialogState& state) {
    if (state.syncing || state.selected_group < 0 ||
        state.selected_group >= static_cast<int>(state.groups.size())) {
        return;
    }
    StockGroup& group = state.groups[static_cast<size_t>(state.selected_group)];
    std::wstring group_name = TrimCopy(GetWindowString(state.group_name));
    if (!group_name.empty()) {
        group.name = std::move(group_name);
    }
    RemoveBlankStocks(state.stocks);
    group.stocks = state.stocks;
}

void RefreshStockList(DialogState& state) {
    state.syncing = true;
    ListBox_ResetContent(state.stock_list);
    for (const auto& stock : state.stocks) {
        const std::wstring label = StockListLabel(stock, state.chinese);
        ListBox_AddString(state.stock_list, label.c_str());
    }
    if (!state.stocks.empty()) {
        state.selected_stock = std::clamp(state.selected_stock, 0, static_cast<int>(state.stocks.size()) - 1);
        ListBox_SetCurSel(state.stock_list, state.selected_stock);
    } else {
        state.selected_stock = -1;
    }
    const bool has_selection = state.selected_stock >= 0;
    EnableWindow(state.remove_stock, has_selection ? TRUE : FALSE);
    EnableWindow(state.move_stock_up, has_selection && state.selected_stock > 0 ? TRUE : FALSE);
    EnableWindow(state.move_stock_down,
                 has_selection && state.selected_stock + 1 < static_cast<int>(state.stocks.size())
                     ? TRUE
                     : FALSE);
    state.syncing = false;
}

void LoadSelectedStockToEditor(DialogState& state) {
    state.syncing = true;
    if (state.selected_stock < 0 ||
        state.selected_stock >= static_cast<int>(state.stocks.size())) {
        SetWindowString(state.symbol, L"");
        SetWindowString(state.code, L"");
        SelectMarket(state.market, L"hk");
        Button_SetCheck(state.show_usd, BST_UNCHECKED);
        Button_SetCheck(state.min_price_enabled, BST_UNCHECKED);
        SetWindowString(state.min_price, L"");
        EnableWindow(state.min_price, FALSE);
        Button_SetCheck(state.max_price_enabled, BST_UNCHECKED);
        SetWindowString(state.max_price, L"");
        EnableWindow(state.max_price, FALSE);
        EnableWindow(state.remove_stock, FALSE);
        EnableWindow(state.move_stock_up, FALSE);
        EnableWindow(state.move_stock_down, FALSE);
        state.syncing = false;
        return;
    }

    const StockTarget& stock = state.stocks[static_cast<size_t>(state.selected_stock)];
    SetWindowString(state.symbol, stock.symbol);
    SetWindowString(state.code, stock.code);
    SelectMarket(state.market, stock.market);
    Button_SetCheck(state.show_usd, stock.show_usd ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(state.min_price_enabled, stock.min_price ? BST_CHECKED : BST_UNCHECKED);
    SetWindowString(state.min_price, FormatOptionalDouble(stock.min_price));
    EnableWindow(state.min_price, stock.min_price ? TRUE : FALSE);
    Button_SetCheck(state.max_price_enabled, stock.max_price ? BST_CHECKED : BST_UNCHECKED);
    SetWindowString(state.max_price, FormatOptionalDouble(stock.max_price));
    EnableWindow(state.max_price, stock.max_price ? TRUE : FALSE);
    EnableWindow(state.remove_stock, TRUE);
    EnableWindow(state.move_stock_up, state.selected_stock > 0 ? TRUE : FALSE);
    EnableWindow(state.move_stock_down,
                 state.selected_stock + 1 < static_cast<int>(state.stocks.size()) ? TRUE : FALSE);
    state.syncing = false;
}

void CommitEditorToSelectedStock(DialogState& state) {
    if (state.syncing || state.selected_stock < 0 ||
        state.selected_stock >= static_cast<int>(state.stocks.size())) {
        return;
    }
    StockTarget& stock = state.stocks[static_cast<size_t>(state.selected_stock)];
    stock.symbol = TrimCopy(GetWindowString(state.symbol));
    stock.code = TrimCopy(GetWindowString(state.code));
    stock.market = TrimCopy(GetComboText(state.market));
    stock.show_usd = Button_GetCheck(state.show_usd) == BST_CHECKED;
    stock.source = _wcsicmp(stock.market.c_str(), L"us") == 0 ? L"alpaca" : L"";
    bool min_ok = true;
    bool max_ok = true;
    stock.min_price = Button_GetCheck(state.min_price_enabled) == BST_CHECKED
                          ? ParseOptionalDouble(TrimCopy(GetWindowString(state.min_price)), &min_ok)
                          : std::nullopt;
    stock.max_price = Button_GetCheck(state.max_price_enabled) == BST_CHECKED
                          ? ParseOptionalDouble(TrimCopy(GetWindowString(state.max_price)), &max_ok)
                          : std::nullopt;
    state.editor_has_invalid_number = !min_ok || !max_ok;
    const int keep_selection = state.selected_stock;
    RefreshStockList(state);
    state.selected_stock = keep_selection;
    ListBox_SetCurSel(state.stock_list, state.selected_stock);
}

void SelectStock(DialogState& state, int index) {
    CommitEditorToSelectedStock(state);
    state.selected_stock = index;
    ListBox_SetCurSel(state.stock_list, state.selected_stock);
    LoadSelectedStockToEditor(state);
}

void SelectGroup(DialogState& state, int index) {
    if (index < 0 || index >= static_cast<int>(state.groups.size())) {
        return;
    }
    CommitEditorToSelectedStock(state);
    CommitCurrentGroup(state);
    state.selected_group = index;
    state.stocks = state.groups[static_cast<size_t>(state.selected_group)].stocks;
    state.selected_stock = state.stocks.empty() ? -1 : 0;
    RefreshGroupCombo(state);
    RefreshStockList(state);
    LoadSelectedStockToEditor(state);
}

void AddGroup(DialogState& state) {
    if (state.groups.size() >= kMaxStockGroups) {
        MessageBoxW(state.window,
                    Text(state.chinese,
                         L"Up to 6 watchlists are supported.",
                         L"最多支持 6 个自选列表。"),
                    Text(state.chinese, L"Stock Config", L"股票配置"),
                    MB_OK | MB_ICONINFORMATION);
        RefreshGroupCombo(state);
        return;
    }

    CommitEditorToSelectedStock(state);
    CommitCurrentGroup(state);

    StockGroup group;
    group.name = MakeUniqueGroupName(state.groups, state.chinese);
    StockTarget stock;
    stock.market = L"hk";
    group.stocks.push_back(stock);
    state.groups.push_back(group);
    SelectGroup(state, static_cast<int>(state.groups.size()) - 1);
    SetFocus(state.group_name);
    SendMessageW(state.group_name, EM_SETSEL, 0, -1);
}

void RemoveSelectedGroup(DialogState& state) {
    if (state.groups.size() <= 1 || state.selected_group < 0 ||
        state.selected_group >= static_cast<int>(state.groups.size())) {
        return;
    }
    state.groups.erase(state.groups.begin() + state.selected_group);
    if (state.selected_group >= static_cast<int>(state.groups.size())) {
        state.selected_group = static_cast<int>(state.groups.size()) - 1;
    }
    state.stocks = state.groups[static_cast<size_t>(state.selected_group)].stocks;
    state.selected_stock = state.stocks.empty() ? -1 : 0;
    RefreshGroupCombo(state);
    RefreshStockList(state);
    LoadSelectedStockToEditor(state);
}

void AddStock(DialogState& state) {
    CommitEditorToSelectedStock(state);
    StockTarget stock;
    stock.market = L"hk";
    state.stocks.push_back(stock);
    state.selected_stock = static_cast<int>(state.stocks.size()) - 1;
    RefreshStockList(state);
    LoadSelectedStockToEditor(state);
    SetFocus(state.symbol);
    SendMessageW(state.symbol, EM_SETSEL, 0, -1);
}

void RemoveSelectedStock(DialogState& state) {
    if (state.selected_stock < 0 ||
        state.selected_stock >= static_cast<int>(state.stocks.size())) {
        return;
    }
    state.stocks.erase(state.stocks.begin() + state.selected_stock);
    if (state.selected_stock >= static_cast<int>(state.stocks.size())) {
        state.selected_stock = static_cast<int>(state.stocks.size()) - 1;
    }
    RefreshStockList(state);
    LoadSelectedStockToEditor(state);
}

void MoveSelectedStock(DialogState& state, int direction) {
    if (direction == 0 || state.selected_stock < 0 ||
        state.selected_stock >= static_cast<int>(state.stocks.size())) {
        return;
    }
    const int target_index = state.selected_stock + direction;
    if (target_index < 0 || target_index >= static_cast<int>(state.stocks.size())) {
        return;
    }

    CommitEditorToSelectedStock(state);
    std::swap(state.stocks[static_cast<size_t>(state.selected_stock)],
              state.stocks[static_cast<size_t>(target_index)]);
    state.selected_stock = target_index;
    RefreshStockList(state);
    LoadSelectedStockToEditor(state);
}

void PopulateControls(DialogState& state) {
    SetWindowString(state.endpoint, state.config->alpaca_endpoint);
    SetWindowString(state.key, state.config->alpaca_key_id);
    SetWindowString(state.secret, state.config->alpaca_secret_key);
    SelectAlpacaFeed(state.feed, state.config->alpaca_feed);
    SelectSortMode(state.sort_mode, state.config->sort_mode);

    state.groups = state.config->stock_groups;
    if (state.groups.size() > kMaxStockGroups) {
        state.groups.resize(kMaxStockGroups);
    }
    if (state.groups.empty()) {
        state.groups.push_back({state.config->active_group.empty() ? L"Default"
                                                                    : state.config->active_group,
                                state.config->stocks});
    }
    state.selected_group = FindGroupIndexByName(state.groups, state.config->active_group);
    if (state.selected_group < 0) {
        state.selected_group = 0;
    }
    state.stocks = state.groups[static_cast<size_t>(state.selected_group)].stocks;
    state.selected_stock = state.stocks.empty() ? -1 : 0;
    RefreshGroupCombo(state);
    RefreshStockList(state);
    LoadSelectedStockToEditor(state);
}

bool ValidateStocks(DialogState& state) {
    if (state.editor_has_invalid_number) {
        MessageBoxW(state.window,
                    Text(state.chinese,
                         L"Low Alert and High Alert must be numbers when enabled.",
                         L"启用后，低价提醒和高价提醒必须填写数字。"),
                    Text(state.chinese, L"Stock Config", L"股票配置"),
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    RemoveBlankStocks(state.stocks);
    RefreshStockList(state);
    LoadSelectedStockToEditor(state);
    for (size_t i = 0; i < state.stocks.size(); ++i) {
        StockTarget& stock = state.stocks[i];
        NormalizeStockFields(stock);
        if (stock.symbol.empty() || stock.code.empty()) {
            state.selected_stock = static_cast<int>(i);
            RefreshStockList(state);
            LoadSelectedStockToEditor(state);
            MessageBoxW(state.window,
                        Text(state.chinese,
                             L"Each stock needs both Name and Code.",
                             L"每个股票都需要同时填写名称和代码。"),
                        Text(state.chinese, L"Stock Config", L"股票配置"),
                        MB_OK | MB_ICONWARNING);
            return false;
        }
        for (size_t j = i + 1; j < state.stocks.size(); ++j) {
            StockTarget& other = state.stocks[j];
            NormalizeStockFields(other);
            if (_wcsicmp(stock.market.c_str(), other.market.c_str()) == 0 &&
                _wcsicmp(stock.code.c_str(), other.code.c_str()) == 0) {
                state.selected_stock = static_cast<int>(j);
                RefreshStockList(state);
                LoadSelectedStockToEditor(state);
                MessageBoxW(state.window,
                            Text(state.chinese,
                                 L"This watchlist already contains the same market and code.",
                                 L"当前自选列表里已经有相同市场和代码的股票。"),
                            Text(state.chinese, L"Stock Config", L"股票配置"),
                            MB_OK | MB_ICONWARNING);
                return false;
            }
        }
        if (stock.min_price && stock.max_price && *stock.min_price >= *stock.max_price) {
            state.selected_stock = static_cast<int>(i);
            RefreshStockList(state);
            LoadSelectedStockToEditor(state);
            MessageBoxW(state.window,
                        Text(state.chinese,
                             L"Min Price must be lower than Max Price.",
                             L"最低价必须小于最高价。"),
                        Text(state.chinese, L"Stock Config", L"股票配置"),
                        MB_OK | MB_ICONWARNING);
            return false;
        }
    }
    return true;
}

bool ValidateGroups(DialogState& state) {
    CommitEditorToSelectedStock(state);
    CommitCurrentGroup(state);

    if (state.groups.empty()) {
        MessageBoxW(state.window,
                    Text(state.chinese,
                         L"Keep at least one watchlist.",
                         L"至少保留一个自选列表。"),
                    Text(state.chinese, L"Stock Config", L"股票配置"),
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    if (state.groups.size() > kMaxStockGroups) {
        state.groups.resize(kMaxStockGroups);
        if (state.selected_group >= static_cast<int>(state.groups.size())) {
            state.selected_group = static_cast<int>(state.groups.size()) - 1;
        }
    }

    for (size_t i = 0; i < state.groups.size(); ++i) {
        state.groups[i].name = TrimCopy(state.groups[i].name);
        RemoveBlankStocks(state.groups[i].stocks);
        if (state.groups[i].name.empty()) {
            SelectGroup(state, static_cast<int>(i));
            MessageBoxW(state.window,
                        Text(state.chinese,
                             L"Each watchlist needs a name.",
                             L"每个自选列表都需要名称。"),
                        Text(state.chinese, L"Stock Config", L"股票配置"),
                        MB_OK | MB_ICONWARNING);
            return false;
        }
        for (size_t j = i + 1; j < state.groups.size(); ++j) {
            if (_wcsicmp(state.groups[i].name.c_str(), state.groups[j].name.c_str()) == 0) {
                SelectGroup(state, static_cast<int>(j));
                MessageBoxW(state.window,
                            Text(state.chinese,
                                 L"Watchlist names must be unique.",
                                 L"自选列表名称不能重复。"),
                            Text(state.chinese, L"Stock Config", L"股票配置"),
                            MB_OK | MB_ICONWARNING);
                return false;
            }
        }
        if (state.groups[i].stocks.empty()) {
            SelectGroup(state, static_cast<int>(i));
            MessageBoxW(state.window,
                        Text(state.chinese,
                             L"Each watchlist needs at least one stock.",
                             L"每个自选列表至少需要一只股票。"),
                        Text(state.chinese, L"Stock Config", L"股票配置"),
                        MB_OK | MB_ICONWARNING);
            return false;
        }
        for (size_t stock_index = 0; stock_index < state.groups[i].stocks.size(); ++stock_index) {
            StockTarget& stock = state.groups[i].stocks[stock_index];
            NormalizeStockFields(stock);
            if (stock.symbol.empty() || stock.code.empty()) {
                SelectGroup(state, static_cast<int>(i));
                state.selected_stock = static_cast<int>(stock_index);
                RefreshStockList(state);
                LoadSelectedStockToEditor(state);
                MessageBoxW(state.window,
                            Text(state.chinese,
                                 L"Each stock needs both Name and Code.",
                                 L"每个股票都需要同时填写名称和代码。"),
                            Text(state.chinese, L"Stock Config", L"股票配置"),
                            MB_OK | MB_ICONWARNING);
                return false;
            }
            for (size_t other_index = stock_index + 1;
                 other_index < state.groups[i].stocks.size();
                 ++other_index) {
                StockTarget& other = state.groups[i].stocks[other_index];
                NormalizeStockFields(other);
                if (_wcsicmp(stock.market.c_str(), other.market.c_str()) == 0 &&
                    _wcsicmp(stock.code.c_str(), other.code.c_str()) == 0) {
                    SelectGroup(state, static_cast<int>(i));
                    state.selected_stock = static_cast<int>(other_index);
                    RefreshStockList(state);
                    LoadSelectedStockToEditor(state);
                    MessageBoxW(state.window,
                                Text(state.chinese,
                                     L"This watchlist already contains the same market and code.",
                                     L"当前自选列表里已经有相同市场和代码的股票。"),
                                Text(state.chinese, L"Stock Config", L"股票配置"),
                                MB_OK | MB_ICONWARNING);
                    return false;
                }
            }
            if (stock.min_price && stock.max_price && *stock.min_price >= *stock.max_price) {
                SelectGroup(state, static_cast<int>(i));
                state.selected_stock = static_cast<int>(stock_index);
                RefreshStockList(state);
                LoadSelectedStockToEditor(state);
                MessageBoxW(state.window,
                            Text(state.chinese,
                                 L"Min Price must be lower than Max Price.",
                                 L"最低价必须小于最高价。"),
                            Text(state.chinese, L"Stock Config", L"股票配置"),
                            MB_OK | MB_ICONWARNING);
                return false;
            }
        }
    }

    return true;
}

bool SaveControls(DialogState& state) {
    AppConfig next = *state.config;
    next.alpaca_endpoint = TrimCopy(GetWindowString(state.endpoint));
    next.alpaca_key_id = TrimCopy(GetWindowString(state.key));
    next.alpaca_secret_key = TrimCopy(GetWindowString(state.secret));
    next.alpaca_feed = TrimCopy(GetComboText(state.feed, L"iex"));
    if (next.alpaca_endpoint.empty()) {
        next.alpaca_endpoint = L"https://data.alpaca.markets";
    }
    if (next.alpaca_feed.empty()) {
        next.alpaca_feed = L"iex";
    }
    next.sort_mode = GetSelectedSortMode(state.sort_mode);

    CommitEditorToSelectedStock(state);
    CommitCurrentGroup(state);
    if (!ValidateGroups(state)) {
        return false;
    }
    if (!ValidateStocks(state)) {
        return false;
    }

    next.stock_groups = state.groups;
    next.active_group = state.groups[static_cast<size_t>(state.selected_group)].name;
    next.stocks = state.stocks;
    if (!SaveConfig(next)) {
        MessageBoxW(state.window,
                    Text(state.chinese,
                         L"Unable to save stocks_config.json.",
                         L"无法保存 stocks_config.json。"),
                    Text(state.chinese, L"Stock Config", L"股票配置"),
                    MB_OK | MB_ICONERROR);
        return false;
    }

    *state.config = std::move(next);
    state.saved = true;
    DestroyWindow(state.window);
    return true;
}

void CreateDialogControls(DialogState& state) {
    const int font_height = -Scale(state.window, 14);
    state.font = CreateFontW(font_height,
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
                             CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS,
                             L"Segoe UI");

    CreateGroupBox(state, Text(state.chinese, L"Alpaca API", L"Alpaca API"), 14, 12, 640, 138);
    CreateLabel(state, L"Endpoint", 32, 42, 88, 22);
    state.endpoint = CreateEdit(state, kEndpointEditId, 124, 40, 500, 24);
    CreateLabel(state, L"Key ID", 32, 74, 88, 22);
    state.key = CreateEdit(state, kKeyEditId, 124, 72, 500, 24);
    CreateLabel(state, L"Secret Key", 32, 106, 88, 22);
    state.secret = CreateEdit(state, kSecretEditId, 124, 104, 328, 24, ES_PASSWORD);
    CreateLabel(state, L"Feed", 470, 106, 44, 22);
    state.feed = CreateComboBox(state, kFeedEditId, 514, 104, 110, 160);
    PopulateAlpacaFeedCombo(state.feed);

    CreateGroupBox(state, Text(state.chinese, L"Watchlists", L"自选列表"), 14, 160, 250, 158);
    CreateLabel(state, Text(state.chinese, L"Active", L"当前"), 32, 190, 70, 22);
    state.group_combo = CreateComboBox(state, kGroupComboId, 104, 188, 134, 160);
    CreateLabel(state, Text(state.chinese, L"Name", L"名称"), 32, 226, 70, 22);
    state.group_name = CreateEdit(state, kGroupNameEditId, 104, 224, 134, 24);
    state.add_group = CreateButton(state,
                                   kAddGroupButtonId,
                                   Text(state.chinese, L"Add", L"添加"),
                                   32,
                                   268,
                                   94,
                                   28);
    state.remove_group = CreateButton(state,
                                      kRemoveGroupButtonId,
                                      Text(state.chinese, L"Remove", L"删除"),
                                      144,
                                      268,
                                      94,
                                      28);

    CreateGroupBox(state, Text(state.chinese, L"List Options", L"列表设置"), 284, 160, 370, 158);
    CreateLabel(state, Text(state.chinese, L"Sort", L"排序"), 302, 190, 76, 22);
    state.sort_mode = CreateComboBox(state, kSortModeComboId, 384, 188, 160, 120);
    PopulateSortModeCombo(state.sort_mode, state.chinese);
    CreateLabel(state,
                Text(state.chinese,
                     L"Config order uses the stock order below; use Up/Down to change it.",
                     L"配置顺序会使用下方股票顺序；可用上移/下移调整。"),
                302,
                228,
                320,
                48);

    CreateGroupBox(state, Text(state.chinese, L"Stocks In Current Watchlist", L"当前列表股票"), 14, 330, 292, 328);
    state.stock_list = CreateListBox(state, kStockListId, 32, 362, 254, 214);
    state.add_stock = CreateButton(state,
                                   kAddStockButtonId,
                                   Text(state.chinese, L"Add", L"添加"),
                                   32,
                                   596,
                                   74,
                                   28);
    state.remove_stock = CreateButton(state,
                                      kRemoveStockButtonId,
                                      Text(state.chinese, L"Remove", L"删除"),
                                      118,
                                      596,
                                      74,
                                      28);
    state.move_stock_up = CreateButton(state,
                                      kMoveStockUpButtonId,
                                      Text(state.chinese, L"Up", L"上移"),
                                      32,
                                      628,
                                      74,
                                      28);
    state.move_stock_down = CreateButton(state,
                                        kMoveStockDownButtonId,
                                        Text(state.chinese, L"Down", L"下移"),
                                        118,
                                        628,
                                        74,
                                        28);

    CreateGroupBox(state, Text(state.chinese, L"Selected Stock", L"当前股票"), 326, 330, 328, 274);
    CreateLabel(state, Text(state.chinese, L"Name", L"名称"), 344, 362, 84, 20);
    state.symbol = CreateEdit(state, kSymbolEditId, 438, 360, 188, 24);
    CreateLabel(state, Text(state.chinese, L"Code", L"代码"), 344, 402, 84, 20);
    state.code = CreateEdit(state, kCodeEditId, 438, 400, 188, 24);
    CreateLabel(state, Text(state.chinese, L"Market", L"市场"), 344, 442, 84, 20);
    state.market = CreateMarketCombo(state, kMarketComboId, 438, 440, 132, 160);
    CreateLabel(state, Text(state.chinese, L"Display", L"显示"), 344, 482, 84, 20);
    state.show_usd = CreateCheckBox(state, kShowUsdCheckId, 438, 480, 90, 24);
    state.min_price_enabled = CreateCheckBox(state, kMinPriceEnableId, L"", 344, 522, 26, 24);
    CreateLabel(state, Text(state.chinese, L"Low Alert", L"低价提醒"), 374, 522, 92, 20);
    state.min_price = CreateEdit(state, kMinPriceEditId, 488, 520, 138, 24);
    state.max_price_enabled = CreateCheckBox(state, kMaxPriceEnableId, L"", 344, 562, 26, 24);
    CreateLabel(state, Text(state.chinese, L"High Alert", L"高价提醒"), 374, 562, 92, 20);
    state.max_price = CreateEdit(state, kMaxPriceEditId, 488, 560, 138, 24);

    state.save = CreateButton(state,
                              kSaveButtonId,
                              Text(state.chinese, L"Save", L"保存"),
                              450,
                              628,
                              90,
                              30);
    state.cancel = CreateButton(state,
                                kCancelButtonId,
                                Text(state.chinese, L"Cancel", L"取消"),
                                548,
                                628,
                                90,
                                30);
    PopulateControls(state);
}

LRESULT CALLBACK DialogProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    DialogState* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<DialogState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_CREATE:
        CreateDialogControls(*state);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w_param) == kGroupComboId && HIWORD(w_param) == CBN_SELCHANGE) {
            const int selected = ComboBox_GetCurSel(state->group_combo);
            SelectGroup(*state, selected);
            return 0;
        }
        if (LOWORD(w_param) == kGroupNameEditId && HIWORD(w_param) == EN_CHANGE) {
            if (!state->syncing && state->selected_group >= 0 &&
                state->selected_group < static_cast<int>(state->groups.size())) {
                state->groups[static_cast<size_t>(state->selected_group)].name =
                    GetWindowString(state->group_name);
            }
            return 0;
        }
        if (LOWORD(w_param) == kAddGroupButtonId) {
            AddGroup(*state);
            return 0;
        }
        if (LOWORD(w_param) == kRemoveGroupButtonId) {
            RemoveSelectedGroup(*state);
            return 0;
        }
        if (LOWORD(w_param) == kStockListId && HIWORD(w_param) == LBN_SELCHANGE) {
            const int selected = ListBox_GetCurSel(state->stock_list);
            SelectStock(*state, selected);
            return 0;
        }
        if (LOWORD(w_param) == kAddStockButtonId) {
            AddStock(*state);
            return 0;
        }
        if (LOWORD(w_param) == kRemoveStockButtonId) {
            RemoveSelectedStock(*state);
            return 0;
        }
        if (LOWORD(w_param) == kMoveStockUpButtonId) {
            MoveSelectedStock(*state, -1);
            return 0;
        }
        if (LOWORD(w_param) == kMoveStockDownButtonId) {
            MoveSelectedStock(*state, 1);
            return 0;
        }
        if ((LOWORD(w_param) == kSymbolEditId || LOWORD(w_param) == kCodeEditId ||
             LOWORD(w_param) == kMinPriceEditId || LOWORD(w_param) == kMaxPriceEditId) &&
            HIWORD(w_param) == EN_CHANGE) {
            CommitEditorToSelectedStock(*state);
            return 0;
        }
        if (LOWORD(w_param) == kMarketComboId && HIWORD(w_param) == CBN_SELCHANGE) {
            CommitEditorToSelectedStock(*state);
            return 0;
        }
        if (LOWORD(w_param) == kShowUsdCheckId) {
            CommitEditorToSelectedStock(*state);
            return 0;
        }
        if (LOWORD(w_param) == kMinPriceEnableId) {
            const bool enabled = Button_GetCheck(state->min_price_enabled) == BST_CHECKED;
            EnableWindow(state->min_price, enabled ? TRUE : FALSE);
            if (!enabled) {
                SetWindowString(state->min_price, L"");
            }
            CommitEditorToSelectedStock(*state);
            return 0;
        }
        if (LOWORD(w_param) == kMaxPriceEnableId) {
            const bool enabled = Button_GetCheck(state->max_price_enabled) == BST_CHECKED;
            EnableWindow(state->max_price, enabled ? TRUE : FALSE);
            if (!enabled) {
                SetWindowString(state->max_price, L"");
            }
            CommitEditorToSelectedStock(*state);
            return 0;
        }
        if (LOWORD(w_param) == kSaveButtonId) {
            SaveControls(*state);
            return 0;
        }
        if (LOWORD(w_param) == kCancelButtonId) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state != nullptr && state->font != nullptr) {
            DeleteObject(state->font);
            state->font = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

void CenterOverOwner(HWND window, HWND owner, int width, int height) {
    RECT owner_rect{};
    if (owner != nullptr && IsWindow(owner)) {
        GetWindowRect(owner, &owner_rect);
    } else {
        owner_rect = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
    const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
    SetWindowPos(window, nullptr, std::max(0, x), std::max(0, y), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

}  // namespace

bool ShowStockConfigDialog(HWND owner,
                           HINSTANCE instance_handle,
                           bool chinese,
                           AppConfig* config) {
    if (config == nullptr) {
        return false;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = DialogProc;
    window_class.hInstance = instance_handle;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kDialogClassName;
    RegisterClassExW(&window_class);

    DialogState state;
    state.chinese = chinese;
    state.config = config;

    const int width = Scale(owner, 690);
    const int height = Scale(owner, 720);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME,
                                  kDialogClassName,
                                  Text(chinese, L"Stock Config", L"股票配置"),
                                  WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  width,
                                  height,
                                  owner,
                                  nullptr,
                                  instance_handle,
                                  &state);
    if (window == nullptr) {
        return false;
    }

    CenterOverOwner(window, owner, width, height);
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return state.saved;
}

}  // namespace stock_taskbar_monitor
