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
    src/app.cpp
    src/tray_ui.cpp
    src/float_lists.cpp
    src/watcher.cpp
    src/util_path.cpp
    src/util_log.cpp
    src/desktop_scanner.cpp
    src/park_restore.cpp
    resources/app.rc)
target_include_directories(LiteDesk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources)
target_compile_definitions(LiteDesk PRIVATE UNICODE _UNICODE)
target_link_libraries(LiteDesk PRIVATE comctl32 shell32 ole32 uxtheme dwmapi)
