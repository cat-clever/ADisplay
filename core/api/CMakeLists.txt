# C ABI 门面 —— 界面层唯一会链接的产物。
#
# 产物名在各平台不同：
#   Windows  castcore.dll + castcore.lib（导入库）
#   macOS    libcastcore.dylib
#   Linux    libcastcore.so
#   Android  libcastcore.so
add_library(castcore SHARED
    src/AdEngine.cpp
)

target_include_directories(castcore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        # configure_file 生成的 adisplay/version.h 落在这里。
        ${CMAKE_BINARY_DIR}/generated/include
)

target_link_libraries(castcore
    PRIVATE
        adisplay_common
        adisplay_discovery
        adisplay_dlna
)

# AD_API 在 Windows 上要区分「编译核心库」与「使用核心库」，
# 这个宏让 adisplay.h 走 dllexport 分支。
target_compile_definitions(castcore PRIVATE ADISPLAY_CORE_BUILD)

# 非 Windows 平台把符号默认藏起来，只有 AD_API 标注的才导出。
#
# 刻意不设 VERSION / SOVERSION：那会让 macOS 上的 dylib 变成
# libcastcore.0.dylib，可执行文件按这个名字去引用，而打包脚本拷进去的是
# 不带后缀的那个 —— 两边对不上，App 一启动就报
# "Library not loaded: @rpath/libcastcore.0.dylib" 然后崩掉。
#
# 版本化的 dylib 是给系统库做多版本 ABI 共存用的。我们是自用项目，
# 界面与核心永远一起发布，不需要这个，保持 libcastcore.dylib 最简单。
set_target_properties(castcore PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME castcore
)

if(WIN32)
    # Windows 上动态库默认不导出任何符号，靠 adisplay.h 里的 __declspec(dllexport)。
    set_target_properties(castcore PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
endif()
