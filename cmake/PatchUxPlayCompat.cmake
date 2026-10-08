# 由 FetchContent 的 PATCH_COMMAND 调用，见 cmake/UxPlayProtocol.cmake。
#
# 要修的是 UxPlay 的 lib/compat.h 里这一段：
#
#     #ifndef snprintf
#     #define snprintf _snprintf
#     #endif
#
# 那是老 MSVC 还没有符合 C99 的 snprintf 的年代留下的。现代 UCRT 自带标准
# snprintf，而且它的 <stdio.h> 里有一条硬性检查：只要 snprintf 是宏就直接
# #error（"Macro definition of snprintf conflicts with Standard Library
# function declaration"）。整条链路上没有任何办法绕开 —— 不定义它，compat.h
# 会定义；定义了，stdio.h 又报错。所以只能把这三行去掉。
#
# 为什么不用 sed：Windows 的 runner 上不一定有。用 CMake 自己的脚本，
# 三个平台行为一致，而且天然幂等 —— 这个修补步骤在构建目录已存在时会重跑。
#
# 入参：-DCOMPAT_FILE=<要修补的 compat.h 路径>

if(NOT DEFINED COMPAT_FILE)
    message(FATAL_ERROR "PatchUxPlayCompat：缺少 COMPAT_FILE 参数")
endif()

if(NOT EXISTS "${COMPAT_FILE}")
    message(FATAL_ERROR "PatchUxPlayCompat：找不到 ${COMPAT_FILE}")
endif()

file(READ "${COMPAT_FILE}" content)

# 先统一换行。git 在 Windows 上可能按 autocrlf 检出成 CRLF，正则里逐个处理
# 两种换行会让表达式难读也难维护；这个文件的行尾用什么不影响编译。
string(REPLACE "\r\n" "\n" content "${content}")

string(REGEX REPLACE
    "#ifndef[ \t\n]+snprintf[ \t\n]+#[ \t]*define[ \t]+snprintf[ \t]+_snprintf[ \t\n]+#endif"
    "/* ADisplay 已移除：现代 UCRT 自带符合 C99 的 snprintf，见 cmake/PatchUxPlayCompat.cmake */"
    patched "${content}")

if(patched STREQUAL content)
    # 没改动。两种情况：已经修过（正常，重跑时会走到这里），
    # 或者上游改了写法而我们的正则不再匹配 —— 后者必须让人看见，
    # 否则就是「补丁悄悄失效、Windows 构建莫名其妙又坏了」。
    string(FIND "${content}" "_snprintf" leftover)
    if(NOT leftover EQUAL -1)
        message(FATAL_ERROR
            "PatchUxPlayCompat：compat.h 里仍有 _snprintf 但它已不是预期的那段写法。"
            "上游改了这个文件，请更新本脚本的正则（文件：${COMPAT_FILE}）")
    endif()
    message(STATUS "PatchUxPlayCompat：无需修改（已修补过）")
else()
    file(WRITE "${COMPAT_FILE}" "${patched}")
    message(STATUS "PatchUxPlayCompat：已移除 compat.h 里的 snprintf 别名")
endif()
