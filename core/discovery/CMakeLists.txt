# 服务发现：mDNS 发布 + SSDP 应答（文档 4.2 的 DiscoveryService）。
#
# mDNS 走各平台的系统实现，源文件按平台挑选 —— 这是文档 4.5 的多平台适配
# 方案，协议细节交给系统服务，我们只维护一层薄薄的调用。

if(APPLE)
    set(ADISPLAY_MDNS_SOURCE
        "${CMAKE_SOURCE_DIR}/platform/macos/src/MdnsPublisherMac.cpp")
elseif(WIN32)
    set(ADISPLAY_MDNS_SOURCE
        "${CMAKE_SOURCE_DIR}/platform/windows/src/MdnsPublisherWindows.cpp")
else()
    set(ADISPLAY_MDNS_SOURCE
        "${CMAKE_SOURCE_DIR}/platform/linux/src/MdnsPublisherLinux.cpp")
endif()

add_library(adisplay_discovery STATIC
    # 统一管理 mDNS 与 SSDP，并处理网卡变化（文档 4.2）。
    src/DiscoveryService.cpp
    # SSDP 没有可用的系统服务，是自己实现的一份，一份代码跨平台。
    src/SsdpServer.cpp
    # mDNS 走各平台系统实现，源文件按平台挑。
    ${ADISPLAY_MDNS_SOURCE}
)

target_include_directories(adisplay_discovery
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(adisplay_discovery
    PUBLIC
        adisplay_common
    PRIVATE
        adisplay_warnings
)

# Windows 的 DNS-SD API 在 dnsapi.lib 里。
if(WIN32)
    target_link_libraries(adisplay_discovery PUBLIC dnsapi)
endif()

# macOS 的 DNSServiceRegister 系列通常在 libSystem 里就能解析到，
# 但显式找一下更稳 —— 找不到也不报错，交给系统默认解析。
if(APPLE)
    find_library(DNSSD_LIBRARY dns_sd)
    if(DNSSD_LIBRARY)
        target_link_libraries(adisplay_discovery PUBLIC "${DNSSD_LIBRARY}")
    endif()
endif()

set_target_properties(adisplay_discovery PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
