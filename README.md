# stock_taskbar_monitor V0.3

一个基于 `minimal_taskbar_monitor` 任务栏嵌入逻辑的 Windows 任务栏监视器。

默认启动是系统资源监视模式，外观和交互尽量保持参考项目风格。通过快捷键切换到股票模式后，任务栏显示简洁股价，悬浮窗显示股票详情、当日涨跌幅和市场信息。

项目地址：https://github.com/xmyhhh/mini_windows_taskbar_monitor_with_stock

## 功能

- Windows 10 / Windows 11 任务栏嵌入显示
- 状态模式：CPU、内存、网络、GPU、磁盘、进程悬浮窗
- 股票模式：港股、A 股、美股行情
- 港股 / A 股使用新浪行情接口
- 美股使用 Alpaca 行情接口，支持盘前 / 盘后可用快照
- 可配置模式切换快捷键
- 支持鼠标悬停弹窗和点击弹窗
- 股票可按配置顺序、当日涨幅、当日跌幅排序
- 支持多个股票分组 / 多个自选列表
- 股票模式下，任务栏控件支持滚轮快速切换自选列表，股票悬浮窗顶部支持临时 tab 切换
- 任务栏股票显示数量可选 2 / 4 / 6 / 8
- 默认英文界面，可在菜单切换中文
- 托盘图标、开机启动、图标嵌入

## 构建

依赖：

- Windows 10 或 Windows 11
- CMake 3.20+
- Visual Studio C++ 工具链
- PowerShell，用于从 `logo.png` 生成 `app_icon.ico`

Visual Studio 2022：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Visual Studio 2026：

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

如果使用 CLion / Ninja，只要 MSVC 环境配置正确也可以构建。

常见输出位置：

```text
build/Release/stock_taskbar_monitor.exe
```

或者 IDE 自己设置的构建目录。

## 使用

运行 `stock_taskbar_monitor.exe` 后，控件会嵌入任务栏。

默认模式是 `Status`，按 `Alt+Q` 在两个模式之间切换：

- `Status`：系统资源监视
- `Stock`：股票价格监视

右键任务栏控件或托盘图标可以打开菜单。

## 菜单说明

一级菜单：

- `Status`：系统资源监视设置
- `Stock`：股票监视设置
- `Hotkey`：选择模式切换快捷键
- `Language`：切换英文 / 中文
- `Launch at startup`：开机启动
- `Help`：内置帮助
- `Exit`：退出程序

`Status` 菜单：

- `Visible Metrics`：CPU、内存、上传、下载、GPU、磁盘读、磁盘写
- `Network Units`：网络单位，bits/sec 或 bytes/sec
- `Popup Mode`：悬停弹窗或点击弹窗
- `Refresh Interval`：刷新间隔，1 / 2 / 5 / 10 秒

`Stock` 菜单：

- `Show stock monitor`：切换到股票模式
- `Reload Stock Config`：重载 `stocks_config.json`
- `Taskbar Symbols`：任务栏显示股票数量，2 / 4 / 6 / 8
- `Sort`：按配置顺序、当日涨幅、当日跌幅排序

股票模式下，鼠标悬停在任务栏控件上时，滚轮可以在不同自选列表之间快速切换；股票悬浮窗顶部提供 tab 临时切换分组，不会改变任务栏当前列表。

## 配置文件

程序会在 exe 同目录生成配置文件：

```text
config.json
stocks_config.json
```

`config.json` 保存程序和状态模式设置：

- 显示哪些系统指标
- 网络单位
- 弹窗模式
- 快捷键
- 界面语言
- 状态模式刷新间隔

`stocks_config.json` 保存股票列表、Alpaca key 和股票模式设置。

## 股票配置示例

```json
{
  "_settings": {
    "sample_interval_seconds": 2,
    "usd_hkd_rate": 7.84,
    "alpaca_endpoint": "https://data.alpaca.markets",
    "alpaca_key": "",
    "alpaca_secret_key": "",
    "alpaca_feed": "iex",
    "popup_activation_mode": "hover",
    "taskbar_symbol_count": 4,
    "sort_mode": "config",
    "active_group": "HK"
  },
  "_groups": {
    "HK": {
      "BABA": {
        "code": "09988",
        "market": "hk",
        "adr_factor": 8,
        "show_usd": true,
        "min_price": 170,
        "max_price": 175
      }
    },
    "CN": {
      "PINGAN": {
        "code": "000001",
        "market": "cn"
      }
    },
    "US": {
      "NVDA": {
        "code": "NVDA",
        "market": "us",
        "source": "alpaca",
        "alpaca_feed": "iex"
      }
    }
  }
}
```

## `_settings` 字段

- `sample_interval_seconds`：股票刷新间隔，范围会限制在 1 到 60 秒
- `usd_hkd_rate`：港币到美元折算汇率，用于港股 ADR 风格美元价格显示
- `alpaca_endpoint`：Alpaca Market Data endpoint，默认 `https://data.alpaca.markets`
- `alpaca_key`：Alpaca API Key ID
- `alpaca_secret_key`：Alpaca Secret Key
- `alpaca_feed`：默认 Alpaca feed，免费账户通常用 `iex`
- `popup_activation_mode`：弹窗模式，`hover` 或 `click`
- `taskbar_symbol_count`：任务栏股票数量，只支持 `2`、`4`、`6`、`8`
- `sort_mode`：股票排序方式，`config`、`top_gainers`、`top_losers`
- `active_group`：当前启用的自选列表名称

## 多分组 / 自选列表

多个自选列表配置在 `_groups` 下：

```json
"_groups": {
  "HK": {
    "BABA": {
      "code": "09988",
      "market": "hk"
    }
  },
  "US": {
    "NVDA": {
      "code": "NVDA",
      "market": "us",
      "source": "alpaca"
    }
  }
}
```

规则：

- `_groups` 的每个 key 就是一个自选列表名称
- `_settings.active_group` 指定当前启用的列表
- 最多支持 6 个自选列表；如果配置超过 6 个，只读取前 6 个，后面的忽略
- 股票模式下，鼠标悬停在任务栏控件上时，滚轮会切换到前一个 / 后一个列表
- 股票悬浮窗顶部使用 tab 临时切换展示分组，不保存，也不改变任务栏当前列表
- 股票编辑窗口支持新增 / 删除分组，并编辑每个分组里的股票
- 旧格式仍兼容：如果没有 `_groups`，程序会把原来的顶层股票自动当作一个默认列表

## 单个股票字段

- `code`：行情代码
- `market`：市场，支持 `hk`、`cn`、`us`
- `source`：可选，写 `alpaca` 时强制走 Alpaca
- `alpaca_feed`：可选，单个股票覆盖默认 Alpaca feed
- `adr_factor`：ADR 折算倍数，默认 `1`
- `show_usd`：港股是否在股票悬浮窗里额外显示 ADR 风格美元价格
- `min_price`：可选，保留给价格阈值配置兼容
- `max_price`：可选，保留给价格阈值配置兼容

## 港股配置

示例：

```json
"BABA": {
  "code": "09988",
  "market": "hk"
}
```

规则：

- `market` 写 `hk`
- `code` 写 5 位港股代码
- 数据源是新浪港股行情
- 如果要显示 ADR 风格美元价格，可以设置 `show_usd: true`
- `adr_factor` 是 ADR 折算倍数，例如 BABA 港股和美股 ADR 常用 `8`

## A 股配置

示例：

```json
"PINGAN": {
  "code": "000001",
  "market": "cn"
}
```

规则：

- `market` 写 `cn`
- `code` 写 6 位 A 股代码
- `6` 开头自动走上交所 `sh`
- `0` / `3` 等常见前缀自动走深交所 `sz`
- 数据源是新浪 A 股行情

## 美股配置

示例：

```json
"NVDA": {
  "code": "NVDA",
  "market": "us",
  "source": "alpaca"
}
```

规则：

- `market` 写 `us`，或者 `source` 写 `alpaca`
- `code` 写美股 ticker
- 需要在 `_settings` 里配置 Alpaca key
- 免费 Alpaca 账户通常使用 `iex`
- 如果账号有 SIP 权限，可以把 `alpaca_feed` 改成 `sip`

## Alpaca 配置

在 `stocks_config.json` 的 `_settings` 中填写：

```json
"alpaca_endpoint": "https://data.alpaca.markets",
"alpaca_key": "你的 key id",
"alpaca_secret_key": "你的 secret key",
"alpaca_feed": "iex"
```

免费账户一般用：

```json
"alpaca_feed": "iex"
```

如果美股显示 `(null)`，优先检查：

- `alpaca_endpoint` 是否正确。行情接口默认用 `https://data.alpaca.markets`，不是 paper trading 的 `https://paper-api.alpaca.markets/v2`
- `alpaca_key` 是否正确
- `alpaca_secret_key` 是否正确
- `alpaca_feed` 是否有权限
- ticker 是否拼写正确
- 网络是否能访问 Alpaca

## 快捷键

默认快捷键：

```text
Alt+Q
```

菜单里可选：

- `Alt+Q`
- `Alt+S`
- `Alt+M`
- `Ctrl+Alt+Q`

选择后会保存到 `config.json`：

```json
"toggle_hotkey": "alt_q"
```

## 语言

默认语言是英文。

切换中文：

```text
右键菜单 -> Language -> 中文
```

保存到 `config.json`：

```json
"language": "zh_cn"
```

切回英文：

```json
"language": "en"
```

任务栏状态模式里的短标签会保持参考项目的紧凑风格，例如 `MEM`、`R`、`W` 不会翻译成中文。

## 注意事项

- 已存在的 `stocks_config.json` 不会因为代码里新增默认示例而自动覆盖。
- 修改 `stocks_config.json` 后，可以通过菜单 `Stock -> Reload Stock Config` 重载。
- 如果任务栏股票显示太宽，可以把 `taskbar_symbol_count` 调小。
- 股票模式任务栏只显示简洁价格，涨跌幅放在股票悬浮窗里。
- 股票涨跌颜色统一，不使用红绿区分。
