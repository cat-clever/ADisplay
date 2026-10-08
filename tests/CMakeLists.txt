# 单元测试。文档第 10 节要求核心解析逻辑有单元测试与模糊测试 ——
# 这里覆盖批次 0 的基础设施，协议解析的测试随各批次补上。
#
# 测试链接 adisplay_common（静态库）而不是 castcore（共享库），
# 这样能直接测内部实现，不受 C ABI 限制。

set(ADISPLAY_TEST_SOURCES
    test_byte_order.cpp
    test_device_name.cpp
    test_device_identity.cpp
    test_ssdp_message.cpp
    test_dlna_description.cpp
    test_dlna_soap.cpp
    test_dlna_http.cpp
    test_dlna_media_url.cpp
    test_pipeline_media_source.cpp
    test_pipeline_hls_playlist.cpp
    test_pipeline_media_relay.cpp
)

foreach(test_source IN LISTS ADISPLAY_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)

    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE adisplay_common)

    # SSDP 报文的测试要直接触达 core/discovery/src 下的内部头（SsdpMessage.h）——
    # 那些纯函数刻意没有放进公开 API，但正是最该被测试覆盖的部分。
    if(test_name STREQUAL "test_ssdp_message")
        target_link_libraries(${test_name} PRIVATE adisplay_discovery)
        target_include_directories(${test_name} PRIVATE
            "${CMAKE_SOURCE_DIR}/core/discovery/src")
    endif()

    # DLNA 的测试同样要触达 protocols/dlna/src 下的内部头。
    if(test_name MATCHES "^test_dlna_")
        target_link_libraries(${test_name} PRIVATE adisplay_dlna)
        target_include_directories(${test_name} PRIVATE
            "${CMAKE_SOURCE_DIR}/protocols/dlna/src")
    endif()

    # HTTP 端点那条集成测试要自己起客户端发 SUBSCRIBE / UNSUBSCRIBE
    # （httplib 的客户端写请求行时不校验方法名），所以得单独链接 httplib —— 
    # adisplay_dlna 是 PRIVATE 链接它，头文件传不出来。
    if(test_name STREQUAL "test_dlna_http")
        target_link_libraries(${test_name} PRIVATE httplib::httplib)
    endif()

    # 媒体管线的用例直接链静态库。播放列表那几条只走纯逻辑，中转服务那条会
    # 真的起一次本地服务、真的发请求（换封装需要真实 TS 素材，那条路径不在
    # 单测里走）。
    if(test_name MATCHES "^test_pipeline_")
        target_link_libraries(${test_name} PRIVATE adisplay_pipeline)
    endif()

    # 中转服务的用例要自己起客户端发请求，得单独链 httplib ——
    # adisplay_pipeline 是 PRIVATE 链它的，头文件传不出来。
    if(test_name STREQUAL "test_pipeline_media_relay")
        target_link_libraries(${test_name} PRIVATE httplib::httplib)
    endif()
    target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${test_name} PRIVATE adisplay_warnings)

    # 中文测试名与中文断言信息要求源码按 UTF-8 解释。
    if(MSVC)
        target_compile_options(${test_name} PRIVATE /utf-8)
    endif()

    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

message(STATUS "  单元测试        : ${ADISPLAY_TEST_SOURCES}")
