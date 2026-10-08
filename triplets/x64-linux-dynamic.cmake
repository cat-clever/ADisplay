# 自定义 vcpkg 三元组：Linux x64，依赖用动态链接。
#
# 为什么不能用内置的 x64-linux（静态链接）：
#   FFmpeg 的部分 x86 汇编目标文件（libavcodec.a 里的 vc1dsp_mmx.o 等）不是
#   位置无关代码，链进 libcastcore.so 时报
#     relocation R_X86_64_PC32 against symbol `ff_pw_9' can not be used when
#     making a shared object; recompile with -fPIC
#   静态构建时 vcpkg 只把 -fPIC 加到了 C 编译上，没传到 NASM 那一步；而
#   --enable-shared 会让 FFmpeg 自己把 pic 打开，汇编也跟着变成 PIC。
#   Windows/macOS 那边 vcpkg 的 port 会带 --enable-pic，所以只有 Linux 炸。
#
# 动态链接在 Linux 桌面端本来就是常规做法（发行版都这么分发），代价是
# libcastcore.so 运行时要能找到这几个 .so —— 将来做 GTK 界面时要随包带上。
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
# 双保险：即使将来某些依赖仍走静态，也保证它们是 PIC。
set(VCPKG_POSITION_INDEPENDENT_CODE ON)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
