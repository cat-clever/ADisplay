cmake_minimum_required(VERSION 3.21)

# 版本号要跟着 git tag 走。发版时要同步的是这六处 —— 前五处是版本号本身，
# 后两处是从它算出来的构建号（主*10000 + 次*100 + 修订）：
#
#   CMakeLists.txt（这里）                界面显示的版本
#   vcpkg.json                            依赖清单里的项目版本
#   app-windows/ADisplay.Windows.csproj   Windows 程序集版本
#   app-macos/Info.plist                  CFBundleShortVersionString
#   app-tv/app/build.gradle.kts           versionName
#   app-macos/Info.plist                  CFBundleVersion       = 构建号
#   app-tv/app/build.gradle.kts           versionCode           = 构建号
#
# CI 里有一步会逐个核对（见 .github/workflows/ci.yml 的「版本号一致性」）。
# 后两处最容易漏：它们不是版本号，换个 5.7 出来却长得像另一个版本号，
# 逐文件搜旧版本号时不会被注意到 —— 这个清单就是为了不再靠记性。
project(ADisplay
    VERSION 0.5.8
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

# Windows 的 <windows.h> 默认定义 max / min 两个宏，会把标准库的
# std::max / std::min 展开成语法垃圾（error C2589: illegal token on right
# side of '::'）。NOMINMAX 关掉它们，WIN32_LEAN_AND_MEAN 顺带裁掉一批
# 用不到的旧头文件（winsock.h 尤其麻烦，它会和 winsock2.h 打架）。
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(AdDisplayOptions)
include(AdDisplayWarnings)

# ---------------------------------------------------------------------------
# 版本头
#
# 刻意不用 add_compile_definitions(ADISPLAY_VERSION="${PROJECT_VERSION}")：
# CMake 语言里的双引号是引用语法，不是值的一部分，那样传下去的是
# ADISPLAY_VERSION=0.1.0，宏在 C++ 里展开成 0.1.0 —— 非法记号，编译不过。
# 生成一个头文件没有这层歧义，也顺便让版本号能被界面层直接 include。
# ---------------------------------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/adisplay_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/adisplay/version.h"
    @ONLY
)

# ---------------------------------------------------------------------------
# 依赖
#
# 走 vcpkg manifest 模式（vcpkg.json）。若调用方没提供 vcpkg 工具链，
# 退回 find_package，方便在已装好依赖的机器上直接 cmake。
# ---------------------------------------------------------------------------
# 基础依赖（批次 0 起）
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# DLNA 模块的依赖（批次 2 起）：
#   tinyxml2     —— 生成设备描述 XML、解析手机发来的 SOAP 请求
#   cpp-httplib  —— 提供设备描述 XML 的 HTTP 服务，并接 SOAP 动作与 GENA 订阅
find_package(tinyxml2 CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)

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
add_subdirectory(protocols)

# 电视端的 JNI 薄壳。只在 Android 构建里出现 —— 桌面端三套界面各自
# 直接链接 castcore，不需要 JNI。
if(ANDROID)
    add_subdirectory(app-tv/app/src/main/cpp)
endif()

if(ADISPLAY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
