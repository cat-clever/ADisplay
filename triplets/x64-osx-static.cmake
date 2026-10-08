# 自定义 vcpkg 三元组：macOS 单架构 + 静态链接。
#
# 为什么不能用内置的 x64-osx：
#   内置三元组用的是动态链接，FFmpeg 会产出一组 .dylib。而我们要 Universal
#   产物 —— 那就得把每一个 .dylib 都用 lipo 合并、再逐个改 install_name，
#   步骤多且容易漏。静态之后 FFmpeg 直接进 libcastcore.dylib，只需要 lipo
#   这一个文件。
#
# 为什么是单架构而不是像 arm64-osx-universal 那样一次编两个：
#   FFmpeg 的 configure 只认一个 --arch，一次 configure 出不了胖库。
#   所以改成分两次编（arm64 / x64），最后 lipo 合并 castcore。
#   Universal 的保证仍在 CI 里断言，只是从「一次编出来」变成「两次编完再合」。
#
# 用法：VCPKG_OVERLAY_TRIPLETS 指向 triplets/ 目录，
#      再设 VCPKG_TARGET_TRIPLET=x64-osx-static。

set(VCPKG_TARGET_ARCHITECTURE x64)

# 动态链接 CRT 与 C++ 运行库。
set(VCPKG_CRT_LINKAGE dynamic)

# 静态链接第三方库：自用项目，省得用户机器上还要装一堆依赖，
# 也让后面的 lipo 只需要处理一个文件。
set(VCPKG_LIBRARY_LINKAGE static)

# 与 CMakeLists / Info.plist 里的最低系统版本保持一致。
set(VCPKG_OSX_DEPLOYMENT_TARGET "12.0")

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
