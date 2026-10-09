# AirPlay 接收端（文档 3.1，批次 4）
#
# 协议实现复用 UxPlay，只取它的 lib/（取舍理由见 cmake/UxPlayProtocol.cmake）。
# 这里负责三件事：
#   1. 把上游那些 C 源码编成一个静态库（adisplay_airplay_proto）
#   2. 接上接缝层（DnssdShim.cpp）—— 协议层读设备身份的接口，
#      同时是「mDNS 通告 == /info 应答」这条一致性的保证
#   3. 接上对外的 C++ 接口（AirplayReceiver.cpp）

# 顶层 project() 只启用了 CXX，而这里要编 C 源码。
enable_language(C)

# Android 暂不编协议层。
#
# 不是做不到，是不放在这一批做：APK 要同时操心体积（上游那些源文件会带上
# libplist 与 OpenSSL）和 Bionic 的缺口（本项目已经为它补过一次 libc 兼容层），
# 而这两件事在桌面两端验证通过之前都是盲改。等 Windows / macOS 的镜像跑通
# 再单独做一轮，风险集中在一处。
if(ANDROID)
    set(ADISPLAY_HAVE_AIRPLAY_RECEIVER 0)
else()
    set(ADISPLAY_HAVE_AIRPLAY_RECEIVER 1)
endif()

add_library(adisplay_airplay STATIC
    src/AirplayReceiver.cpp
    # 镜像帧的 NALU 规范化（参数集缓存与补齐）。纯逻辑，单独成文件便于单元测试，
    # 也刻意不依赖协议层 —— 测试不必把 UxPlay 拖进来。
    src/MirrorNalu.cpp
)

target_include_directories(adisplay_airplay
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    # MirrorNalu.h 住在 src 下：它是内部实现，不进公开 API，但要被单元测试
    # 直接包含（协议层的测试不能只靠 C ABI 那一层黑盒）。
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# 对外告诉调用方（castcore）这份构建到底有没有协议层。
# 界面层据此如实提示，而不是让用户对着「设备可见但连不上」自己猜。
if(ADISPLAY_HAVE_AIRPLAY_RECEIVER)
    target_compile_definitions(adisplay_airplay PUBLIC ADISPLAY_HAVE_AIRPLAY_RECEIVER=1)
else()
    target_compile_definitions(adisplay_airplay PUBLIC ADISPLAY_HAVE_AIRPLAY_RECEIVER=0)
endif()

target_link_libraries(adisplay_airplay
    PUBLIC
        adisplay_common
        # 广播仍是 DiscoveryService 的事，接收端只是需要它的地址规范化结果。
        adisplay_discovery
    PRIVATE
        adisplay_warnings
)

if(ADISPLAY_HAVE_AIRPLAY_RECEIVER)
    include(${CMAKE_SOURCE_DIR}/cmake/UxPlayProtocol.cmake)

    set(UXPLAY_LIB_DIR "${uxplay_protocol_SOURCE_DIR}/lib")
    if(NOT EXISTS "${UXPLAY_LIB_DIR}/raop.c")
        message(FATAL_ERROR
            "UxPlay 源码没取到预期内容（${UXPLAY_LIB_DIR}）。"
            "上游改了目录结构就得同步更新这里与 cmake/UxPlayProtocol.cmake。")
    endif()

    # 源文件用 GLOB 而不是逐个列。上游版本由标签固定，但增删文件是常事，
    # 一份手写清单会悄悄过期 —— 那种错误的表现是「升级后少了几个符号」。
    file(GLOB UXPLAY_PROTOCOL_SOURCES CONFIGURE_DEPENDS
        "${UXPLAY_LIB_DIR}/*.c"
        "${UXPLAY_LIB_DIR}/llhttp/*.c"
        "${UXPLAY_LIB_DIR}/playfair/*.c"
    )
    # dnssd.c 排除：它要么依赖 Apple Bonjour 要么依赖 Avahi，Windows 上两者
    # 都没有。协议层需要的那几个 dnssd_* 符号由 DnssdShim.cpp 提供。
    list(FILTER UXPLAY_PROTOCOL_SOURCES EXCLUDE REGEX "/dnssd\\.c$")

    add_library(adisplay_airplay_proto STATIC ${UXPLAY_PROTOCOL_SOURCES})

    target_include_directories(adisplay_airplay_proto
        PRIVATE
            "${UXPLAY_LIB_DIR}"
            "${UXPLAY_LIB_DIR}/llhttp"
            "${UXPLAY_LIB_DIR}/playfair"
    )

    target_compile_definitions(adisplay_airplay_proto
        PRIVATE
            # 上游默认行为：新设备接入时断开已有连接（见 lib/CMakeLists.txt）。
            NOHOLD
            # libplist 2.1 / 2.3 各改过一次 API。vcpkg 给的是 2.8，
            # 两个开关都要打开，否则会走到已废弃的兼容分支上。
            PLIST_210
            PLIST_230
            # OpenSSL 3 把 1.1.1 那一代的 API 标成了废弃，而这个宏让它按
            # 1.1.1 的可见性暴露出来 —— 上游对所有平台都设了它，
            # 漏掉的话协议层里用到的那几个接口会随 OpenSSL 版本时有时无。
            OPENSSL_API_COMPAT=0x10101000L
    )

    if(WIN32)
        # 上游官方只支持 MinGW/MSYS2，我们用 MSVC，缺的少数 POSIX 接口
        # 由这个垫片补上（清单与理由见 compat/win32/AdisplayWin32Compat.h）。
        target_include_directories(adisplay_airplay_proto
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/compat/win32")
        target_compile_options(adisplay_airplay_proto
            PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/compat/win32/AdisplayWin32Compat.h")
        # 上游的 compat.h / sockets.h 判的是 WIN32 而不是 _WIN32。CMake 给 MSVC
        # 默认加了 /DWIN32，但那是它的默认值而不是我们的声明 —— 依赖别人的默认
        # 值，会在某次工具链更新后变成一段只在 Windows 上走错分支的代码。
        target_compile_definitions(adisplay_airplay_proto PRIVATE WIN32)
        # Windows 上上游按 pthread 写，而 pthreads-win32 的 pthread_t 不是
        # 能赋 0 的类型（上游的 THREAD_CREATE 宏里有一句 handle = 0），
        # 在 MSVC 上编不过。协议层用到的接口只有十三个，我们自己在
        # compat/win32/pthread.h 里包一层 Win32 原语，顺带去掉一个依赖。
        target_link_libraries(adisplay_airplay_proto
            PUBLIC ws2_32 wsock32 iphlpapi)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(adisplay_airplay_proto PUBLIC Threads::Threads)
    endif()

    # 这份构建没开 TLS，协议本身也不需要 —— 播放列表走的是明文 HTTP。
    find_package(OpenSSL REQUIRED)
    find_package(unofficial-libplist CONFIG REQUIRED)

    target_link_libraries(adisplay_airplay_proto
        PRIVATE
            OpenSSL::Crypto
            unofficial::libplist::libplist
    )

    # 第三方源码不套我们的严格告警设置：-Werror 用在自己的代码上是纪律，
    # 用在别人的代码上只会让每一次上游升级都变成一次排错。
    set_target_properties(adisplay_airplay_proto PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )

    # 接缝：用我们自己的一份 dnssd_* 替代上游那份。
    target_sources(adisplay_airplay PRIVATE src/DnssdShim.cpp)

    target_include_directories(adisplay_airplay PRIVATE "${UXPLAY_LIB_DIR}")
    target_link_libraries(adisplay_airplay PRIVATE adisplay_airplay_proto)

    if(WIN32)
        # 平台垫片的实现（usleep / clock_gettime）。
        target_sources(adisplay_airplay PRIVATE src/AdisplayWin32Compat.c)
        target_include_directories(adisplay_airplay
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/compat/win32")
    endif()

    message(STATUS "ADisplay: AirPlay 协议层已启用（UxPlay ${uxplay_protocol_SOURCE_DIR}）")
else()
    message(STATUS "ADisplay: AirPlay 协议层未编入（Android 暂不支持）")
endif()
