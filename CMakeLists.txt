cmake_minimum_required(VERSION 3.20)

project(stock_taskbar_monitor LANGUAGES CXX)

find_program(POWERSHELL_EXECUTABLE NAMES powershell pwsh REQUIRED)

set(APP_LOGO_PNG "${CMAKE_CURRENT_SOURCE_DIR}/logo.png")
set(APP_ICON_ICO "${CMAKE_CURRENT_BINARY_DIR}/app_icon.ico")
set(RESOURCE_HEADER_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/resource.h")

file(TO_CMAKE_PATH "${APP_LOGO_PNG}" APP_LOGO_PNG_PATH)
file(TO_CMAKE_PATH "${APP_ICON_ICO}" APP_ICON_ICO_PATH)
file(TO_CMAKE_PATH "${RESOURCE_HEADER_PATH}" RESOURCE_HEADER_PATH)

add_custom_command(
    OUTPUT "${APP_ICON_ICO}"
    COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_app_icon.ps1"
            -InputPng "${APP_LOGO_PNG}"
            -OutputIco "${APP_ICON_ICO}"
    DEPENDS
        "${APP_LOGO_PNG}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_app_icon.ps1"
    COMMENT "Generating application icon from logo.png"
    VERBATIM
)

add_custom_target(generate_app_icon DEPENDS "${APP_ICON_ICO}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/app_generated.rc"
    @ONLY
)

add_executable(stock_taskbar_monitor WIN32
    "${CMAKE_CURRENT_BINARY_DIR}/app_generated.rc"
    src/app_config.cpp
    src/main.cpp
    src/stock_config_dialog.cpp
    src/stock_fetcher.cpp
    src/stock_config.cpp
    src/process_monitor.cpp
    src/system_metrics.cpp
    src/taskbar_embedder.cpp
)

add_dependencies(stock_taskbar_monitor generate_app_icon)

target_compile_features(stock_taskbar_monitor PRIVATE cxx_std_20)

target_compile_definitions(stock_taskbar_monitor PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000000
)

target_link_libraries(stock_taskbar_monitor PRIVATE
    pdh
    iphlpapi
    psapi
    advapi32
    shell32
    winhttp
    gdiplus
    ole32
)

if(MSVC)
    target_compile_options(stock_taskbar_monitor PRIVATE
        $<$<CONFIG:Release>:/O2 /Oi /Ot /GF /Gy /Gw>
    )
    target_link_options(stock_taskbar_monitor PRIVATE
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>
    )
endif()
