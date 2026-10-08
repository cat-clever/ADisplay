# 媒体管线（文档 4.1）。
#
# 批次 3 的定位刻意收窄：不做解码、不做渲染、不碰音画同步 —— DLNA 给的是一条
# URL，那些都交给各平台自带的播放器。这里只放「平台播放器吃不下某种封装」
# 时需要的处理：把 TS 封装的 HLS 边下边换成 fMP4 再喂给播放器。
#
# 依赖同样收窄：后续接 FFmpeg 时只开 libavformat（解复用 + 换封装），
# 不要 avcodec / swscale —— 不做解码就不需要它们。
add_library(adisplay_pipeline STATIC
    # 媒体源形态判断。纯函数，单独成文件便于单元测试。
    src/MediaSource.cpp
)

target_include_directories(adisplay_pipeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(adisplay_pipeline
    PRIVATE
        adisplay_warnings
)

set_target_properties(adisplay_pipeline PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
