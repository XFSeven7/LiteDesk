cmake_minimum_required(VERSION 3.20)
project(LiteDesk LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 单配置生成器（如 Ninja）未指定时默认 Release；VS 多配置请用 --config Release
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# 源文件为无 BOM 的 UTF-8，MSVC 默认按本地代码页解析会让中文字面量乱码
if(MSVC)
    add_compile_options(/utf-8)
endif()
add_executable(LiteDeskTests
    tests/test_main.cpp
    src/window_snap.cpp
    src/util_path.cpp
    src/util_log.cpp
    src/desktop_scanner.cpp
    src/park_restore.cpp)
# 控制台测试子系统
set_target_properties(LiteDeskTests PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_definitions(LiteDeskTests PRIVATE UNICODE _UNICODE)
target_link_libraries(LiteDeskTests PRIVATE shell32 ole32)

# 主程序（GUI 子系统；托盘 + App 编排）
enable_language(RC)
add_executable(LiteDesk WIN32
    src/main.cpp
    src/hidden_restart.cpp
    src/app.cpp
    src/window_snap.cpp

    src/tray_ui.cpp
    src/float_lists.cpp src/file_drag.cpp
    src/watcher.cpp
    src/util_path.cpp
    src/util_log.cpp
    src/desktop_scanner.cpp
    src/park_restore.cpp
    resources/app.rc)
target_include_directories(LiteDesk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources)
target_compile_definitions(LiteDesk PRIVATE UNICODE _UNICODE)
target_link_libraries(LiteDesk PRIVATE comctl32 shell32 ole32 uxtheme)
# 空闲不加载：comctl32/uxtheme 延迟到首次创建悬浮列表时再映射
if(MSVC)
    target_link_libraries(LiteDesk PRIVATE delayimp)
    target_link_options(LiteDesk PRIVATE "/DELAYLOAD:comctl32.dll" "/DELAYLOAD:uxtheme.dll")
endif()

# 隔离合成桌面，禁止注册系统菜单或改变真实桌面图标。
add_executable(LiteDeskSessionTests tests/test_app_session.cpp src/app.cpp src/window_snap.cpp src/float_lists.cpp src/file_drag.cpp
    src/tray_ui.cpp src/watcher.cpp src/util_path.cpp src/util_log.cpp src/park_restore.cpp src/desktop_scanner.cpp)
target_include_directories(LiteDeskSessionTests PRIVATE resources)
target_compile_definitions(LiteDeskSessionTests PRIVATE UNICODE _UNICODE LITEDESK_TESTING)
target_link_libraries(LiteDeskSessionTests PRIVATE comctl32 shell32 ole32 uxtheme delayimp)
if(MSVC)
    target_link_options(LiteDeskSessionTests PRIVATE /DELAYLOAD:comctl32.dll /DELAYLOAD:uxtheme.dll)
endif()

add_executable(LiteDeskManifestTests tests/test_manifest_concurrency.cpp src/util_path.cpp src/util_log.cpp
    src/park_restore.cpp src/desktop_scanner.cpp)
target_compile_definitions(LiteDeskManifestTests PRIVATE UNICODE _UNICODE)
target_link_libraries(LiteDeskManifestTests PRIVATE shell32 ole32)

# 按需构建的安全内存测量程序，使用与集成测试相同的合成桌面。
add_executable(LiteDeskMemoryProbe EXCLUDE_FROM_ALL tests/memory_probe.cpp src/app.cpp src/window_snap.cpp src/float_lists.cpp src/file_drag.cpp
    src/tray_ui.cpp src/watcher.cpp src/util_path.cpp src/util_log.cpp src/park_restore.cpp src/desktop_scanner.cpp)
target_include_directories(LiteDeskMemoryProbe PRIVATE src resources)
target_compile_definitions(LiteDeskMemoryProbe PRIVATE UNICODE _UNICODE LITEDESK_TESTING)
target_link_libraries(LiteDeskMemoryProbe PRIVATE comctl32 shell32 ole32 uxtheme psapi delayimp)
if(MSVC)
    target_link_options(LiteDeskMemoryProbe PRIVATE /DELAYLOAD:comctl32.dll /DELAYLOAD:uxtheme.dll)
endif()

add_executable(LiteDeskCommandTests tests/test_on_demand.cpp src/app.cpp src/window_snap.cpp src/float_lists.cpp src/file_drag.cpp
    src/tray_ui.cpp src/watcher.cpp src/util_path.cpp src/util_log.cpp src/park_restore.cpp src/desktop_scanner.cpp)
target_include_directories(LiteDeskCommandTests PRIVATE resources)
target_compile_definitions(LiteDeskCommandTests PRIVATE UNICODE _UNICODE LITEDESK_TESTING)
target_link_libraries(LiteDeskCommandTests PRIVATE comctl32 shell32 ole32 uxtheme delayimp)
if(MSVC)
    target_link_options(LiteDeskCommandTests PRIVATE /DELAYLOAD:comctl32.dll /DELAYLOAD:uxtheme.dll)
endif()

add_executable(LiteDeskResourceTests tests/test_ui_resources.cpp src/float_lists.cpp src/file_drag.cpp src/util_path.cpp
    src/util_log.cpp src/park_restore.cpp src/desktop_scanner.cpp)
target_compile_definitions(LiteDeskResourceTests PRIVATE UNICODE _UNICODE LITEDESK_TESTING)
target_link_libraries(LiteDeskResourceTests PRIVATE comctl32 shell32 ole32 uxtheme delayimp)
if(MSVC)
    target_link_options(LiteDeskResourceTests PRIVATE /DELAYLOAD:comctl32.dll /DELAYLOAD:uxtheme.dll)
endif()

# 独立 GUI 测试程序使用真实入口，允许验证隐藏后的实例交接。
add_executable(LiteDeskTestApp WIN32 src/main.cpp src/hidden_restart.cpp src/app.cpp src/window_snap.cpp src/float_lists.cpp src/file_drag.cpp src/tray_ui.cpp
    src/watcher.cpp src/util_path.cpp src/util_log.cpp src/park_restore.cpp src/desktop_scanner.cpp)
target_include_directories(LiteDeskTestApp PRIVATE resources)
target_compile_definitions(LiteDeskTestApp PRIVATE UNICODE _UNICODE LITEDESK_TESTING)
target_link_libraries(LiteDeskTestApp PRIVATE comctl32 shell32 ole32 uxtheme delayimp)
if(MSVC)
    target_link_options(LiteDeskTestApp PRIVATE /DELAYLOAD:comctl32.dll /DELAYLOAD:uxtheme.dll)
endif()
add_dependencies(LiteDeskCommandTests LiteDeskTestApp)
target_link_libraries(LiteDeskCommandTests PRIVATE psapi)

add_executable(LiteDeskDragTests tests/test_file_drag.cpp src/app.cpp src/window_snap.cpp src/float_lists.cpp src/file_drag.cpp src/tray_ui.cpp
    src/watcher.cpp src/util_path.cpp src/util_log.cpp src/park_restore.cpp src/desktop_scanner.cpp)
target_include_directories(LiteDeskDragTests PRIVATE resources)
target_compile_definitions(LiteDeskDragTests PRIVATE UNICODE _UNICODE LITEDESK_TESTING)
target_link_libraries(LiteDeskDragTests PRIVATE comctl32 shell32 ole32 uxtheme)
