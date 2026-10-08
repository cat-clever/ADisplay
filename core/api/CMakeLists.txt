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
)

target_link_libraries(castcore
    PRIVATE
        adisplay_common
)

# AD_API 在 Windows 上要区分「编译核心库」与「使用核心库」，
# 这个宏让 adisplay.h 走 dllexport 分支。
target_compile_definitions(castcore PRIVATE ADISPLAY_CORE_BUILD)

# 非 Windows 平台把符号默认藏起来，只有 AD_API 标注的才导出。
set_target_properties(castcore PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # 界面层拿到的名字不该带版本后缀，方便打包脚本直接拷贝。
    OUTPUT_NAME castcore
)

if(WIN32)
    # Windows 上动态库默认不导出任何符号，靠 adisplay.h 里的 __declspec(dllexport)。
    set_target_properties(castcore PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
endif()
