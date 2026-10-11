#include <adisplay/common/Random.h>

#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#  include <bcrypt.h>
#elif defined(__APPLE__)
#  include <stdlib.h>   // arc4random_buf
#elif defined(__ANDROID__)
#  include <errno.h>
#  include <fcntl.h>
#  include <unistd.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <sys/random.h>
#  include <unistd.h>
#endif

namespace adisplay::common {
namespace {

#if !defined(_WIN32) && !defined(__APPLE__)
// Linux 上 getrandom 不可用时的兜底：读 /dev/urandom。
bool read_dev_urandom(uint8_t* out, std::size_t count) {
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    std::size_t done = 0;
    while (done < count) {
        const ssize_t got = ::read(fd, out + done, count - done);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) {
                continue;
            }
            ::close(fd);
            return false;
        }
        done += static_cast<std::size_t>(got);
    }
    ::close(fd);
    return true;
}
#endif

}  // namespace

bool random_bytes(uint8_t* out, std::size_t count) {
    if (out == nullptr || count == 0) {
        return false;
    }

#if defined(_WIN32)
    const NTSTATUS status = ::BCryptGenRandom(
        nullptr, out, static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {   // 0 == STATUS_SUCCESS
        std::memset(out, 0, count);
        return false;
    }
    return true;

#elif defined(__APPLE__)
    ::arc4random_buf(out, count);
    return true;

#elif defined(__ANDROID__)
    // Android 直接读 /dev/urandom。
    //
    // 试过两条更短的路都不通：getrandom() 要 API 28 才声明出来（我们在
    // android-24 上构建，是编译期找不到符号，不是运行时 ENOSYS）；而
    // arc4random_buf 虽然在 Bionic 里有，libc++ 的 <cstdlib> 不会把它带进
    // 全局命名空间，同样编不过。
    //
    // 既然如此就用它下面的实现 —— read_dev_urandom 本来就摆在文件里，
    // 是给「内核太老没有 getrandom」准备的兜底，这里正好复用，不必为
    // 一个函数再去和头文件较劲。
    //
    // 另一条路是把 minSdk 抬到 28 换回 getrandom，但电视盒子里 Android 8
    // 及以下的还不少，为这个放弃那批设备不划算。
    if (!read_dev_urandom(out, count)) {
        std::memset(out, 0, count);
        return false;
    }
    return true;

#else
    std::size_t done = 0;
    while (done < count) {
        const ssize_t got = ::getrandom(out + done, count - done, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ENOSYS) {
                // 内核太老，没有 getrandom 系统调用。
                break;
            }
            std::memset(out, 0, count);
            return false;
        }
        done += static_cast<std::size_t>(got);
    }
    if (done < count) {
        if (!read_dev_urandom(out + done, count - done)) {
            std::memset(out, 0, count);
            return false;
        }
    }
    return true;
#endif
}

uint32_t random_below(uint32_t bound) {
    if (bound <= 1u) {
        return 0u;
    }

    // 拒绝采样：丢弃落在 [limit, 2^32) 的取值，保证各结果等概率。
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % bound) - 1u;

    uint32_t value = 0;
    for (int attempt = 0; attempt < 128; ++attempt) {
        uint8_t raw[4] = {0};
        if (!random_bytes(raw, sizeof(raw))) {
            return 0u;
        }
        value = (static_cast<uint32_t>(raw[0]) << 24) |
                (static_cast<uint32_t>(raw[1]) << 16) |
                (static_cast<uint32_t>(raw[2]) << 8) |
                 static_cast<uint32_t>(raw[3]);
        if (value <= limit) {
            return value % bound;
        }
    }
    return 0u;
}

}  // namespace adisplay::common
