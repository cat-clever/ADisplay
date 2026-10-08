# 自定义 vcpkg 三元组：Linux x64 + 静态链接 + 位置无关代码（PIC）。
#
# 为什么不能用内置的 x64-linux：
#   它确实也是静态链接，但没有打开 PIC。静态库里的目标文件一旦带上绝对地址
#   重定位，就没法再链进共享库 —— 链接器会直接拒绝：
#
#     /usr/bin/ld: final link failed: bad value
#
#   而本项目恰恰要把这些静态库链进 libcastcore.so。这个问题之前一直没浮出来，
#   是因为其它依赖碰巧都是 PIC（spdlog / fmt / nlohmann-json / tinyxml2 /
#   cpp-httplib 要么自己开了 PIC，要么根本没产出这类重定位）。FFmpeg 是第一个
#   把这层前提打破的依赖，内置三元组的缺陷才在它身上暴露。
#
# 打开 VCPKG_POSITION_INDEPENDENT_CODE 之后，vcpkg 会给这些库的编译补上 -fPIC，
# 目标文件即可被链进共享库。
#
# 用法：VCPKG_OVERLAY_TRIPLETS 指向 triplets/ 目录，
#      再设 VCPKG_TARGET_TRIPLET=x64-linux-pic。

set(VCPKG_TARGET_ARCHITECTURE x64)

set(VCPKG_CRT_LINKAGE dynamic)

# 静态链接第三方库：自用项目，省得用户机器上还要装一堆依赖。
set(VCPKG_LIBRARY_LINKAGE static)

# 关键的一行：让静态库带着 PIC 编出来，否则链进 libcastcore.so 时报 bad value。
set(VCPKG_POSITION_INDEPENDENT_CODE ON)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)
