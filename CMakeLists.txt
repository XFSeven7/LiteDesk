cmake_minimum_required(VERSION 3.20)
project(LiteDesk LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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

# 主程序（GUI 子系统；当前 main 为烟测占位）
add_executable(LiteDesk WIN32
    src/main.cpp
    src/float_lists.cpp
    src/util_path.cpp
    src/util_log.cpp
    src/desktop_scanner.cpp
    src/park_restore.cpp)
target_compile_definitions(LiteDesk PRIVATE UNICODE _UNICODE)
target_link_libraries(LiteDesk PRIVATE comctl32 shell32 ole32)
