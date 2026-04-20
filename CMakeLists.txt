# CMake最低版本要求（适配C++20）
cmake_minimum_required(VERSION 3.20)

# ==================== VS工程美化配置 ====================
# 开启VS文件夹分组功能
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
# 【修正拼写】收纳CMake内置目标（ZERO_CHECK等）到 CMakeTargets 文件夹
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMakeTargets")

# ==================== 编译器标准配置 ====================
# 强制使用C++20标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==================== 项目定义 ====================
project(fusion_window)

# ==================== 源文件 ====================
set(SOURCES
    main.cpp
    ui/native_ui.hpp
)

# ==================== 生成Windows窗口程序（无控制台弹窗） ====================
add_executable(fusion_window WIN32 ${SOURCES})

# ==================== 你要求的VS专属配置 ====================
# 1. 开启UTF-8编码（解决中文乱码）
if(WIN32)
    ADD_DEFINITIONS(-DUNICODE)
    ADD_DEFINITIONS(-D_UNICODE)
    #[[
    if (MSVC)
    target_compile_options(${library} PUBLIC "/utf-8")
    endif()
    #]]
    cmake_host_system_information(RESULT CPU_NUMBER_OF_LOGICAL_CORES QUERY NUMBER_OF_LOGICAL_CORES)
    add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/MP${CPU_NUMBER_OF_LOGICAL_CORES}>)
endif()

target_compile_options(fusion_window PUBLIC "/utf-8")

# 2. Windows窗口程序链接配置（屏蔽控制台）
target_link_options(fusion_window PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT fusion_window)