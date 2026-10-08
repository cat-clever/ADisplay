# UxPlay 协议层的引入（文档 3.1.3）
#
# 文档明确写着：「以上字段细节（包头布局、密钥派生方式等）请以开源实现源码为准
# 逐项对照验证，不要仅凭本文档实现。」AirPlay 的配对、FairPlay 握手、镜像流
# 加密这些细节逆向成分很重 —— 照着文档臆写只会得到一堆看起来合理、
# 实际连不上的代码。所以协议层复用 UxPlay。
#
# 只取它的 lib/，不要 renderers/：
#   * renderers/ 依赖 GStreamer（Windows 上要拖进一整套运行时），
#     和「绿色版、双击即用」的定位冲突。音视频渲染由各平台自带播放器负责。
#   * lib/ 本身一处都不引用 renderers（已逐文件确认），它通过 raop_callbacks_t
#     把解密后的音视频帧交出来 —— 正好是我们要的边界。
#   * 它的 dnssd.c 不要：那份实现要么依赖 Apple Bonjour，要么依赖 Avahi，
#     Windows 上两者都没有。广播本来就是我们自己的 DiscoveryService 在做，
#     协议层只需要读设备身份，那由 DnssdShim.cpp 提供。
#
# 许可证：UxPlay 是 GPLv3，本项目是 AGPL-3.0，同族兼容。文档 3.1.3 也说明了
# 「仅自用且不分发时可以直接复用其代码（GPL 的开源义务主要在分发时产生）」。

include(FetchContent)

FetchContent_Declare(uxplay_protocol
    GIT_REPOSITORY https://github.com/FDH2/UxPlay.git
    # 固定标签，不用分支：UxPlay 在持续跟进 iOS 版本，跟着分支走等于
    # 让上游的改动直接决定我们的构建结果。
    GIT_TAG        v1.73.7
    GIT_SHALLOW    TRUE
    # 故意指向一个不存在的子目录。
    #
    # UxPlay 是个应用程序工程：它的顶层 CMakeLists 会去找 GStreamer、X11、dbus，
    # 还会生成 uxplay 可执行文件 —— 这些我们一样都不要。FetchContent 在
    # SOURCE_SUBDIR 不存在时只下载、不 add_subdirectory，于是我们拿到源码，
    # 用自己的规则编译（见 protocols/airplay/CMakeLists.txt）。
    SOURCE_SUBDIR  adisplay-does-not-use-uxplay-root-project
    # 上游的 lib/compat.h 会给 snprintf 定义一个 _snprintf 别名，而现代 UCRT 的
    # <stdio.h> 只要发现 snprintf 是宏就 #error —— 没有别的办法绕开，
    # 只能把那段去掉。理由与幂等性说明见 cmake/PatchUxPlayCompat.cmake。
    PATCH_COMMAND  ${CMAKE_COMMAND}
                   -DCOMPAT_FILE=<SOURCE_DIR>/lib/compat.h
                   -P "${CMAKE_CURRENT_LIST_DIR}/PatchUxPlayCompat.cmake"
)

FetchContent_MakeAvailable(uxplay_protocol)
