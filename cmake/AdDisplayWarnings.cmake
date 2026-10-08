# ADisplay —— 统一的编译告警级别
#
# 项目要在 MSVC / Clang / GCC 三套编译器上构建（文档 2.5 的多架构要求），
# 这里把各家的告警开关收在一处，避免每个 CMakeLists 重复。
#
# 注意：告警当作错误只在 CI 的 Debug 构建里开（ADISPLAY_WARNINGS_AS_ERRORS），
# 本地开发默认只警告不阻断，否则第三方头文件的告警会让人没法干活。

option(ADISPLAY_WARNINGS_AS_ERRORS "把编译告警当作错误" OFF)

# 建一个 INTERFACE 目标承载这些选项，各目标 link 它即可。
add_library(adisplay_warnings INTERFACE)

if(MSVC)
    target_compile_options(adisplay_warnings INTERFACE
        /W4          # 高于默认的 /W3
        /permissive- # 严格标准一致模式
        /utf-8       # 源码与执行字符集都用 UTF-8 —— 本项目的注释与日志都是中文
        /Zc:__cplusplus
        /Zc:preprocessor
        /MP          # 并行编译
    )
    if(ADISPLAY_WARNINGS_AS_ERRORS)
        target_compile_options(adisplay_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(adisplay_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
    )
    if(ADISPLAY_WARNINGS_AS_ERRORS)
        target_compile_options(adisplay_warnings INTERFACE -Werror)
    endif()
endif()

# 第三方头文件（spdlog / nlohmann / httplib）不参与告警，
# 由 CMake 的 SYSTEM 属性处理，见各 target 的 include 声明。
