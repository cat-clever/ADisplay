# 媒体管线（文档 4.1）。
#
# 批次 3 的定位刻意收窄：不做解码、不做渲染、不碰音画同步 —— DLNA 给的是一条
# URL，那些都交给各平台自带的播放器。这里只放「平台播放器吃不下某种封装」
# 时需要的处理：把 TS 封装的 HLS 边下边换成 fMP4 再喂给播放器。
#
# 依赖同样收窄：只用 libavformat / libavcodec / libavutil（解复用 + 换封装），
# 不要 avfilter / swscale / swresample，也不要任何编码器 —— 不做解码就不需要它们。
add_library(adisplay_pipeline STATIC
    # 媒体源形态判断。纯函数，单独成文件便于单元测试。
    src/MediaSource.cpp
    # 本地 HLS 播放列表的改写。同样是纯函数，必须能被单元测试直接覆盖。
    src/HlsPlaylist.cpp
    # 远端拉取（HTTP 客户端），播放列表与分片都要用它。
    src/HttpFetch.cpp
    # TS 分片 → fMP4 的换封装。只依赖 libavformat。
    src/Mp4Remuxer.cpp
    # 一次中转会话：分片的按需生产与缓存，以及那套并发模型。
    src/RelaySession.cpp
    # 本地 HTTP 中转服务与对外接口。
    src/MediaRelay.cpp
)

target_include_directories(adisplay_pipeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# ---------------------------------------------------------------------------
# FFmpeg
#
# 各平台装出来的 FFmpeg 提供的 CMake 包装并不统一，所以这里按「最具体到最兜底」
# 的顺序找一遍，谁先命中就用谁：
#   1. vcpkg 的 CONFIG 包（四个平台的 CI 都走这条），导入目标形如 FFMPEG::avformat
#   2. 名字小写的同类 CONFIG 包
#   3. pkg-config（本机装了 homebrew / apt 的 ffmpeg 时）
#   4. 手工 find_path + find_library（最后兜底）
# 少一个库都会在链接期炸掉，所以这里找不到就直接 FATAL_ERROR，不要留到运行期。
# ---------------------------------------------------------------------------
find_package(FFMPEG CONFIG QUIET)
if(NOT TARGET FFMPEG::avformat)
    find_package(ffmpeg CONFIG QUIET)
endif()

set(ADISPLAY_FFMPEG_LIBS "")

if(TARGET FFMPEG::avformat)
    foreach(component avformat avcodec avutil)
        if(TARGET FFMPEG::${component})
            list(APPEND ADISPLAY_FFMPEG_LIBS FFMPEG::${component})
        endif()
    endforeach()
elseif(TARGET ffmpeg::avformat)
    foreach(component avformat avcodec avutil)
        if(TARGET ffmpeg::${component})
            list(APPEND ADISPLAY_FFMPEG_LIBS ffmpeg::${component})
        endif()
    endforeach()
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ADISPLAY_FFMPEG QUIET IMPORTED_TARGET
                          libavformat libavcodec libavutil)
        if(TARGET PkgConfig::ADISPLAY_FFMPEG)
            list(APPEND ADISPLAY_FFMPEG_LIBS PkgConfig::ADISPLAY_FFMPEG)
        endif()
    endif()

    if(NOT ADISPLAY_FFMPEG_LIBS)
        # 兜底：自己找头文件与库。vcpkg 的工具链会把 CMAKE_PREFIX_PATH 指到
        # 已安装的依赖上，所以这几条在四个平台上都能命中（macOS 是通用构建，
        # 依赖按架构分别装，找法一样）。
        find_path(ADISPLAY_AVFORMAT_INCLUDE_DIR NAMES libavformat/avformat.h)
        find_library(ADISPLAY_AVFORMAT_LIBRARY NAMES avformat)
        find_library(ADISPLAY_AVCODEC_LIBRARY NAMES avcodec)
        find_library(ADISPLAY_AVUTIL_LIBRARY NAMES avutil)
        if(ADISPLAY_AVFORMAT_INCLUDE_DIR AND ADISPLAY_AVFORMAT_LIBRARY
           AND ADISPLAY_AVCODEC_LIBRARY AND ADISPLAY_AVUTIL_LIBRARY)
            add_library(adisplay_ffmpeg INTERFACE)
            target_include_directories(adisplay_ffmpeg INTERFACE
                "${ADISPLAY_AVFORMAT_INCLUDE_DIR}")
            target_link_libraries(adisplay_ffmpeg INTERFACE
                "${ADISPLAY_AVFORMAT_LIBRARY}"
                "${ADISPLAY_AVCODEC_LIBRARY}"
                "${ADISPLAY_AVUTIL_LIBRARY}")
            list(APPEND ADISPLAY_FFMPEG_LIBS adisplay_ffmpeg)
        endif()
    endif()
endif()

if(NOT ADISPLAY_FFMPEG_LIBS)
    message(FATAL_ERROR
        "找不到 FFmpeg 的 libavformat / libavcodec / libavutil。"
        "媒体管线的换封装需要它们（vcpkg.json 里声明了 ffmpeg 的 avcodec + avformat）。")
endif()

target_link_libraries(adisplay_pipeline
    PRIVATE
        adisplay_common
        adisplay_warnings
        httplib::httplib
        ${ADISPLAY_FFMPEG_LIBS}
)

set_target_properties(adisplay_pipeline PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
