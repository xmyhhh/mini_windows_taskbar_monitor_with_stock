cmake_minimum_required(VERSION 3.20)

project(stock_taskbar_monitor VERSION 0.8.0 LANGUAGES CXX)

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

include(CTest)
if(BUILD_TESTING)
    add_executable(stock_regression_tests
        tests/stock_regression_tests.cpp
        src/stock_fetcher.cpp
        src/stock_config.cpp
    )
    add_executable(taskbar_embedder_tests tests/taskbar_embedder_tests.cpp)
    add_executable(background_worker_tests tests/background_worker_tests.cpp)
    add_executable(ui_responsiveness_tests
        tests/ui_responsiveness_tests.cpp
        src/app_config.cpp
        src/stock_config.cpp
        src/stock_config_dialog.cpp
        src/stock_fetcher.cpp
        src/process_monitor.cpp
        src/system_metrics.cpp
        src/taskbar_embedder.cpp
    )
    foreach(test_target IN ITEMS stock_regression_tests taskbar_embedder_tests background_worker_tests ui_responsiveness_tests)
        target_compile_features(${test_target} PRIVATE cxx_std_20)
        target_compile_options(${test_target} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)
        target_compile_definitions(${test_target} PRIVATE
            UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
            WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000
        )
        set_target_properties(${test_target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests"
        )
    endforeach()
    target_link_libraries(stock_regression_tests PRIVATE winhttp)
    target_link_libraries(taskbar_embedder_tests PRIVATE shell32)
    target_link_libraries(ui_responsiveness_tests PRIVATE
        pdh iphlpapi psapi advapi32 shell32 winhttp gdiplus ole32)
    add_test(NAME stock_quotes_and_config COMMAND stock_regression_tests)
    add_test(NAME taskbar_display_recovery COMMAND taskbar_embedder_tests)
    add_test(NAME background_worker_responsiveness COMMAND background_worker_tests)
    add_test(NAME stock_ui_responsiveness COMMAND ui_responsiveness_tests)
endif()
