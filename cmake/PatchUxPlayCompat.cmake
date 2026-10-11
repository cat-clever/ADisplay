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
#       -DRAOP_FILE=<要修补的 raop.c 路径>（第二处补丁，见本文件末尾）

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

# ===========================================================================
# 第二处：手机的自查询不该拆掉正在进行的镜像会话
#
# 要修的是 raop.c 里这一段（BLE 信标探测那条路）：
#
#     if (cseq || ble) {
#         if (httpd_count_connection_type(raop->httpd, CONNECTION_TYPE_RAOP)) {
#             if (httpd_nohold(raop->httpd)) {
#                 httpd_remove_known_connections(raop->httpd);   ← 把正在镜像的连接删掉
#
# 上面那个 ble 是怎么来的：/info?txtAirPlay&txtRAOP 这种没有 CSeq 的请求被标成
# 「蓝牙信标探测」。iOS 会隔一阵就用它重新发现一次设备 —— 那是**发现**，不是
# 「我要接管这条连接」。但我们开着 nohold（要它，否则上一台设备的残留连接会让
# 新设备直接吃 409），于是每次自查询都把正在投屏的会话拆掉。
#
# 真机上就是这个表现：投一会儿自己断开，日志里紧跟着一句
#   *****"nohold" feature: switch to new connection request from <同一个 IP>
#
# 改法是给这个「接管」判断加上 cseq：只有真正带 CSeq 的 RAOP 请求才算新客户端
# 要接管，BLE 信标探测照旧被识别成 RAOP 并正常回应 /info，只是不再拆任何东西。
#
# 入参：-DRAOP_FILE=<要修补的 raop.c 路径>

if(NOT DEFINED RAOP_FILE)
    message(FATAL_ERROR "PatchUxPlayCompat：缺少 RAOP_FILE 参数")
endif()

if(NOT EXISTS "${RAOP_FILE}")
    message(FATAL_ERROR "PatchUxPlayCompat：找不到 ${RAOP_FILE}")
endif()

file(READ "${RAOP_FILE}" raop_content)
string(REPLACE "\r\n" "\n" raop_content "${raop_content}")

# 用 string(REPLACE) 而不是正则：这行里有括号和 ->，正则要一路转义，容易写错。
# 原文的缩进是 12 个空格（见上游 raop.c 的 conn->connection_type 分支）。
string(REPLACE
    "            if (httpd_count_connection_type(raop->httpd, CONNECTION_TYPE_RAOP)) {"
    "            /* ADisplay 已改：BLE 信标探测（/info?txtAirPlay，无 CSeq）只是设备发现\n               在问一句，不算「新连接要接管」—— 否则手机会隔一阵把自己的镜像会话\n               拆掉。理由见 cmake/PatchUxPlayCompat.cmake。 */\n            if (cseq && httpd_count_connection_type(raop->httpd, CONNECTION_TYPE_RAOP)) {"
    raop_patched "${raop_content}")

if(raop_patched STREQUAL raop_content)
    # 没改动。要么已经修过（正常，重跑时会走到这里），要么上游改了写法 ——
    # 后者必须让人看见，否则就是「补丁悄悄失效、镜像又变成投一会儿就断」。
    string(FIND "${raop_content}" "cseq && httpd_count_connection_type" already)
    if(already EQUAL -1)
        message(FATAL_ERROR
            "PatchUxPlayCompat：raop.c 里没找到预期的那行写法（既没打过补丁、也不是\n"
            "原来的样子）。上游改了 raop.c，请更新本脚本（文件：${RAOP_FILE}）")
    endif()
    message(STATUS "PatchUxPlayCompat：raop.c 无需修改（已修补过）")
else()
    file(WRITE "${RAOP_FILE}" "${raop_patched}")
    message(STATUS "PatchUxPlayCompat：已让 BLE 信标探测不再拆掉镜像会话")
endif()
