cmake_minimum_required(VERSION 3.21)

project(ADisplay
    VERSION 0.1.0
    DESCRIPTION "跨平台投屏接收端：AirPlay / DLNA 接收，支持 Windows、macOS、Android TV"
    HOMEPAGE_URL "https://github.com/cat-clever/ADisplay"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局语言设置
#
# 文档 2.5：「代码层面不做架构假设」。这里关掉编译器扩展，保证
# 在 MSVC / Clang / GCC 上用同一套严格语义。
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 静态库要被链进共享库（Windows 的 castcore.dll / macOS 的 dylib），必须开 PIC。
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 单配置生成器下默认走 Release。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# 归档与版本信息
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(AdDisplayOptions)
include(AdDisplayWarnings)

# 版本号注入到核心库，由 ad_version_string() 返回。
add_compile_definitions(ADISPLAY_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# 依赖
#
# 走 vcpkg manifest 模式（vcpkg.json）。若调用方没提供 vcpkg 工具链，
# 退回 find_package，方便在已装好依赖的机器上直接 cmake。
# ---------------------------------------------------------------------------
# 批次 0 只需要这三项。XML 解析（tinyxml2）与 HTTP 服务（cpp-httplib）
# 是 DLNA 模块的依赖，等批次 2 真正用到时再加 —— 提前引入只会多几个
# 配置阶段的硬失败点，不划算。
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

message(STATUS "")
message(STATUS "ADisplay ${PROJECT_VERSION} 配置摘要")
message(STATUS "  构建类型      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ 标准      : ${CMAKE_CXX_STANDARD}")
message(STATUS "  系统 / 架构   : ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE)
    message(STATUS "  目标架构      : ${CMAKE_OSX_ARCHITECTURES}")
endif()
message(STATUS "  构建核心库    : 是")
message(STATUS "  构建单元测试  : ${ADISPLAY_BUILD_TESTS}")
message(STATUS "")

# ---------------------------------------------------------------------------
# 子目录
# ---------------------------------------------------------------------------
add_subdirectory(core)

if(ADISPLAY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
