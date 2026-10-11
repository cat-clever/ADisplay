# 自定义 vcpkg 三元组：生成 Universal（arm64 + x86_64）的依赖库。
#
# 为什么需要它：
#   文档 2.5 要求 macOS 端产出 Universal 通用二进制（同时覆盖 Apple Silicon 与 Intel）。
#   通用二进制要求可执行文件链接的每一个库都是通用的 —— 只要有一个依赖是单架构，
#   整个链接就会失败。而 vcpkg 内置的 arm64-osx / x64-osx 三元组各自只编一个架构。
#
#   所以这里自定义一个三元组，让每个依赖都用 -arch arm64 -arch x86_64 编译。
#   代价是依赖的编译时间大约翻倍，换来一个包通吃两种 Mac。
#
# 用法：把本目录通过 VCPKG_OVERLAY_TRIPLETS 传给 vcpkg，
#      再设 VCPKG_TARGET_TRIPLET=arm64-osx-universal。

set(VCPKG_TARGET_ARCHITECTURE arm64)

# 动态链接 CRT 与 C++ 运行库。
set(VCPKG_CRT_LINKAGE dynamic)

# 静态链接第三方库：自用项目，省得用户机器上还要装一堆依赖。
set(VCPKG_LIBRARY_LINKAGE static)

# 关键的一行：一次编译出两个架构。
set(VCPKG_OSX_ARCHITECTURES "arm64;x86_64")

# 与 CMakeLists / Info.plist 里的最低系统版本保持一致。
set(VCPKG_OSX_DEPLOYMENT_TARGET "12.0")

# 交给 vcpkg 自己去解析这些库的 CMake 配置。
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
