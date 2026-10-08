# DLNA 渲染器（DMR），文档 3.2。
add_library(adisplay_dlna STATIC
    # 设备描述与 SCPD 的 XML 生成。纯函数，单独成文件便于单元测试。
    src/DlnaDescription.cpp
    src/DlnaIcon.h
    # SOAP 请求解析与应答构建。同样是纯逻辑，必须能被测试覆盖 ——
    # 应答格式错一个元素名，手机端就表现为「点了没反应」。
    src/DlnaSoap.cpp
    # HTTP 服务端、SOAP 动作分发、GENA 事件订阅。
    src/DlnaRenderer.cpp
)

target_include_directories(adisplay_dlna
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(adisplay_dlna
    PUBLIC
        adisplay_common
    PRIVATE
        adisplay_warnings
        tinyxml2::tinyxml2
        httplib::httplib
)

set_target_properties(adisplay_dlna PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
