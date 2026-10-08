// ADisplay —— Android 的 libc 兼容补齐
//
// spdlog 的 os-inl.h 在 Linux 系平台上直接调 fwrite_unlocked（glibc 提供的
// 非加锁版本）。Android 的 Bionic 没有这个函数，于是交叉编译在最后一步炸掉：
//
//   ld.lld: error: undefined symbol: fwrite_unlocked
//   >>> referenced by os-inl.h:562
//       spdlog.cpp.o:(spdlog::details::os::fwrite_bytes)
//
// vcpkg 交叉编译出的 libspdlog.a 是按它自己的编译期判断编的，我们这边改不了。
// 与其去 patch 一个第三方依赖（下次升级还要重来），不如按平台惯例把这个符号
// 补上 —— fwrite_unlocked 语义上就是「不加锁的 fwrite」，而 spdlog 只在自身
// 的文件 sink 里用它，本就是单线程写入路径，转给 fwrite 是等价的。
//
// 刻意编进 castcore 而不是某个静态库：静态库的成员只有在被引用时才会被
// 拉进链接，而引用它的恰恰是排在链接行后面的 libspdlog.a，顺序上不保证
// 能被取到。放在共享库自己的源文件里就没有这个问题。
#if defined(__ANDROID__)

#include <cstddef>
#include <cstdio>

extern "C" std::size_t fwrite_unlocked(const void* ptr, std::size_t size,
                                       std::size_t count, std::FILE* stream) {
    return std::fwrite(ptr, size, count, stream);
}

#endif
