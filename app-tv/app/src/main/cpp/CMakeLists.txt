# JNI 薄壳，只给电视端用。
#
# 刻意不进 Gradle 的 externalNativeBuild：那要求 Gradle 自己再搭一遍
# vcpkg + NDK 工具链，等于把已经验证过的一套东西复制成两份维护。
# 放进主构建之后，castcore 就是同一个构建里的目标，直接链接即可，
# 不用在 Gradle 和 CMake 之间来回传库路径。
cmake_minimum_required(VERSION 3.21)

project(adisplay_jni CXX)

add_library(adisplay_jni SHARED adisplay_jni.cpp)

target_link_libraries(adisplay_jni
    PRIVATE
        castcore
        # Android 的日志。
        log
)

set_target_properties(adisplay_jni PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    # JNI 那几个 Java_xxx 符号靠 JNIEXPORT 导出（默认可见），
    # 所以整体藏起来不影响它们被 JVM 找到。
    VISIBILITY_INLINES_HIDDEN ON
)
