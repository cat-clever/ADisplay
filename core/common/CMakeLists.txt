# 基础设施层。做成静态库：它不导出符号，也不需要被界面层直接链接。
add_library(adisplay_common STATIC
    src/ByteOrder.cpp
    src/Config.cpp
    src/DeviceIdentity.cpp
    src/DeviceName.cpp
    src/Log.cpp
    src/NetUtil.cpp
    src/Random.cpp
    src/ThreadPool.cpp
)

target_include_directories(adisplay_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# 第三方依赖标成 SYSTEM：它们的头文件不该让本项目的 -Wconversion 之类的告警淹没。
target_link_libraries(adisplay_common
    PUBLIC
        spdlog::spdlog
        fmt::fmt
        nlohmann_json::nlohmann_json
)

# 平台库
if(WIN32)
    # BCryptGenRandom（随机数）、GetAdaptersAddresses（网卡枚举）
    target_link_libraries(adisplay_common PUBLIC bcrypt iphlpapi ws2_32)

    # MSVC 会把 getenv / strncpy 这类 C 函数标成「不安全」并刷一屏 C4996。
    # 这些用法在本项目里都是有界的（读环境变量、已知长度的拷贝），
    # 关掉这个告警，免得把真正的警告淹掉。
    target_compile_definitions(adisplay_common PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    # arc4random_buf 在 libSystem 里，无需额外链接
elseif(UNIX)
    # Linux 上 getrandom 由 glibc 提供
    find_package(Threads REQUIRED)
    target_link_libraries(adisplay_common PUBLIC Threads::Threads)
endif()

target_link_libraries(adisplay_common PRIVATE adisplay_warnings)

set_target_properties(adisplay_common PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
