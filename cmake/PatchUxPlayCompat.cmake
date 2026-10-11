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

# ===========================================================================
# 第三处：丢几个包不该让声音停将近三秒
#
# 要修的是 raop_buffer.c 的出队逻辑。上游的写法是：队首那个包还没到时，
# **只要缓冲里还有空位就返回空、继续等重发**（它自己的注释：
# "Return nothing and hope resend gets on time"）。于是丢几个包就要一直等到缓冲被填满
# 才肯往下走 —— 256 个包 × 10.9 毫秒（AAC-ELD 每包 480 样本 / 44100 Hz）≈ **2.8 秒**。
# 实测的静音时长正是 2.8–4.7 秒（2026-10-11 用户日志里六次：3111 / 2879 / 3506 /
# 4734 / 3180 / 765 毫秒），而那一刻接收端整条伴音管道只启动过一次 —— 不是管道被拆，
# 是**帧卡在一个填不上的洞上**。
#
# 做法：加一个短超时。一个包等不到就往前跳，把那一小段丢掉 —— 丢一小段远好过静几秒。
#
# 2026-10-11 第二轮（v1.0.8 之后）：超时是加上了，静音却**一点没短**（实测 2996 /
# 3204 / 3182 / 3112 / 3458 / 3621 毫秒）。算一下才明白第一版错在哪：计时在**每跳过一个
# 洞之后都清零**，于是「连续丢 N 个包」要等 N × 超时 —— 7～9 个包正好又是三秒，和没打
# 补丁的 256 / 91.9 ≈ 2.78 秒几乎一样，白改。现在改成**一整串洞只在串首计一次时**。
#
# 同一份日志还给出两条事实，都写在这里免得下次又猜：
#   * 那些静音的同一段时间里，**画面那条流一直没停**（视频计数每 5 秒稳定 +150 帧，
#     = 30 fps 满速），所以不是「发送端停了」；停的只是伴音这一条。
#   * 每个洞都等满了整段时间才跳 —— 说明我们发出去的重发请求**始终没有兑现**，
#     等待纯属白等。所以这里再加一行日志把「跳了几个包、等了多久」记下来，
#     这是唯一能分辨「重发没来」和「洞太大」的证据。
#
# 入参：-DBUFFER_FILE=<要修补的 raop_buffer.c 路径>
#
# 这里用 string(REPLACE)（**字面**替换）而不是正则：要匹配的块里有括号、星号、花括号，
# 正则写起来又长又容易错。锚点对不上时下面的校验会 FATAL_ERROR，不会静默失效。
# ===========================================================================

if(NOT DEFINED BUFFER_FILE)
    message(FATAL_ERROR "PatchUxPlayCompat：缺少 BUFFER_FILE 参数")
endif()

if(NOT EXISTS "${BUFFER_FILE}")
    message(FATAL_ERROR "PatchUxPlayCompat：找不到 ${BUFFER_FILE}")
endif()

file(READ "${BUFFER_FILE}" buffer_content)
string(REPLACE "\r\n" "\n" buffer_content "${buffer_content}")

# 这个补丁是**插入**而不是替换，锚点在打完之后依然存在 —— 直接重跑会再插一遍。
# 所以先看标记：已经有了就整段跳过（本脚本在构建目录已存在时会重跑）。
string(FIND "${buffer_content}" "ADISPLAY_HOLE_TIMEOUT_MS" buffer_already)
if(NOT buffer_already EQUAL -1)
    message(STATUS "PatchUxPlayCompat：raop_buffer.c 已修补过，跳过")
else()

# ---- (1) clock_gettime 要用 <time.h> ----
string(REPLACE
    "#include <stdint.h>"
    "#include <stdint.h>\n/* ADisplay 补丁：下面的洞超时要 clock_gettime */\n#include <time.h>"
    buffer_content "${buffer_content}")

# ---- (2) 超时常量与取时间的辅助函数（放在结构体定义之前，保证使用点可见）----
string(REPLACE
    "struct raop_buffer_s {"
    "/* ADisplay 补丁：一串缺失的包最多等多久（毫秒）。见 raop_buffer_dequeue。\n   依据：重发请求就在同一条线程上、出队循环之后立刻发出，局域网上一个来回是几毫秒\n   到几十毫秒 —— 200 毫秒已经是十倍以上的余量。等更久不会让包回来（实测每次都等到\n   超时），只会把那一段静音拖长。 */\n#define ADISPLAY_HOLE_TIMEOUT_MS 200\n\nstatic int64_t adisplay_now_ms(void) {\n    struct timespec ts;\n    clock_gettime(CLOCK_REALTIME, &ts);\n    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;\n}\n\nstruct raop_buffer_s {"
    buffer_content "${buffer_content}")

# ---- (3) 结构体里记「这个洞是什么时候发现的」----
string(REPLACE
    "    raop_buffer_entry_t entries[RAOP_BUFFER_LENGTH];\n};"
    "    raop_buffer_entry_t entries[RAOP_BUFFER_LENGTH];\n\n    /* ADisplay 补丁：当前这串洞是什么时候发现的（毫秒）。0 表示没有洞。\n       calloc 出来就是 0，不需要另外初始化。\n       注意它表示的是「**这一串**洞的起点」：跳过一个洞之后不清零，\n       否则连续丢 N 个包就要等 N 倍超时（第一版就是这么错的）。 */\n    int64_t adisplay_hole_since_ms;\n\n    /* ADisplay 补丁：这串洞里已经跳掉了几个包（与上面的计时同生共死）。 */\n    unsigned short adisplay_hole_skip;\n};"
    buffer_content "${buffer_content}")

# ---- (4) 出队：等重发，但一整串洞只等一小会儿 ----
#
# 另外：缓冲被取空时也把计时清零（见下面的 (4b)）—— 空缓冲意味着没有洞了，
# 留着计时会让下一次交付打出一条「等了很久」的假日志。

string(REPLACE
    "    } else if (!entry->filled) {\n        /* Check how much we have space left in the buffer */\n        if (entry_count < RAOP_BUFFER_LENGTH) {\n            /* Return nothing and hope resend gets on time */\n            return NULL;\n        }\n        /* Risk of buffer overrun, return empty buffer */\n    }"
    "    } else if (!entry->filled) {\n        /* ADisplay 补丁：等重发，但一整串洞只等 ADISPLAY_HOLE_TIMEOUT_MS 这么久。\n           计时只在**串首**开始，跳过一个洞之后不清零 —— 否则连续丢 N 个包就是\n           N 倍超时，和上游那套「等到缓冲被填满（约 2.8 秒）」一样长（第一版实测\n           3 秒，就是这么来的）。 */\n        int64_t now_ms = adisplay_now_ms();\n        if (raop_buffer->adisplay_hole_since_ms == 0) {\n            raop_buffer->adisplay_hole_since_ms = now_ms;\n            raop_buffer->adisplay_hole_skip = 0;\n        }\n        /* Check how much we have space left in the buffer */\n        if (entry_count < RAOP_BUFFER_LENGTH &&\n            now_ms - raop_buffer->adisplay_hole_since_ms < ADISPLAY_HOLE_TIMEOUT_MS) {\n            /* Return nothing and hope resend gets on time（只等上面那点时间）*/\n            return NULL;\n        }\n        /* Risk of buffer overrun，或者等超时了：丢掉这一段往前跳 */\n        raop_buffer->adisplay_hole_skip++;\n    }"
    buffer_content "${buffer_content}")

# ---- (4b) 缓冲空了 = 没有洞了：计时与跳数清零 ----
#
# 不清的话，下一次交付会打出一条「等了很久」的假日志（等待时间从上一串洞算起）。
string(REPLACE
    "    /* Cannot dequeue from empty buffer */\n    if (raop_buffer->is_empty || entry_count <= 0) {\n        return NULL;\n    }"
    "    /* ADisplay 补丁：缓冲空了就是没有洞了，计时与跳数清零。 */\n    if (raop_buffer->is_empty || entry_count <= 0) {\n        raop_buffer->adisplay_hole_since_ms = 0;\n        raop_buffer->adisplay_hole_skip = 0;\n        return NULL;\n    }"
    buffer_content "${buffer_content}")

# ---- (5) 取到真包 = 这串洞走完了：打一行日志，再清计时 ----
#
# 只能挂在这里，不能挂在「往前走了」那一步：跳过洞时也是往前走，而那时候其实
# 还在同一串洞里。取到 `entry->filled` 为真的包，才说明这串洞真的结束了。
string(REPLACE
    "    entry->filled = 0;\n\n    /* Return entry payload buffer */"
    "    entry->filled = 0;\n\n    /* ADisplay 补丁：这串洞走完了（到这里一定是取到了真包）。\n       跳掉几个、在洞上等了多久 —— 只打这一行，不在每次跳过时打。 */\n    if (raop_buffer->adisplay_hole_since_ms != 0) {\n        logger_log(raop_buffer->logger, LOGGER_INFO,\n                   \"ADisplay 伴音：跳过 %u 个缺失的包（在洞上等了 %d 毫秒）\",\n                   (unsigned) raop_buffer->adisplay_hole_skip,\n                   (int) (adisplay_now_ms() - raop_buffer->adisplay_hole_since_ms));\n        raop_buffer->adisplay_hole_since_ms = 0;\n        raop_buffer->adisplay_hole_skip = 0;\n    }\n\n    /* Return entry payload buffer */"
    buffer_content "${buffer_content}")

# ---- (6) 整块 flush 时也清零 ----
string(REPLACE
    "        raop_buffer->first_seqnum = next_seq;"
    "        /* ADisplay 补丁：整块丢掉了，洞的计时与跳数一并清零。 */\n        raop_buffer->adisplay_hole_since_ms = 0;\n        raop_buffer->adisplay_hole_skip = 0;\n        raop_buffer->first_seqnum = next_seq;"
    buffer_content "${buffer_content}")

# ---- 校验：标记在，且上游那句原注释不在了 ----
string(FIND "${buffer_content}" "ADISPLAY_HOLE_TIMEOUT_MS" buffer_applied)
string(FIND "${buffer_content}" "hope resend gets on time */" buffer_leftover)
if(buffer_applied EQUAL -1 OR NOT buffer_leftover EQUAL -1)
    message(FATAL_ERROR
        "PatchUxPlayCompat：raop_buffer.c 的「洞超时」补丁没打上 —— "
        "上游改了这个文件的写法，请更新本脚本的锚点（文件：${BUFFER_FILE}）")
endif()

file(WRITE "${BUFFER_FILE}" "${buffer_content}")
message(STATUS "PatchUxPlayCompat：已给 raop_buffer.c 加上缺失包的超时")

endif()
