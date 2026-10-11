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


// ---------------------------------------------------------------------------
// Android 上的 glob / globfree
//
// FFmpeg 的 libavformat 在编译期探测到 Bionic 的头文件里有 glob 声明，就把它
// 用进了自己的文件协议代码里。可 Bionic 的 glob / globfree 是 API 28 才提供的
// 符号，本项目按 android-24 构建，于是链接期直接缺符号：
//
//   ld.lld: error: undefined symbol: glob
//   ld.lld: error: undefined symbol: globfree
//
// macOS / Windows 的 libc 里都有这两个函数，所以只有 Android 炸。和上面的
// fwrite_unlocked 一样，重新编一份 FFmpeg 治标不治本（下次升级还得再来一次），
// 补符号更省事。
//
// 有一点必须先讲清楚：我们的核心路径是把内存里的字节喂给 avformat（远端分片
// 由我们自己的 HTTP 客户端取回），从不把本地路径通配交给它，所以这段代码在
// 正常调用路径上根本不会被执行。但正因为它属于「平时不走、一旦被走到就必须
// 是对的」的兜底实现，这里不能写成永远返回错误的空壳 —— 真被调到时要能给出
// 正确结果，否则就是把一个链接错误换成了一个静默的运行时错误。
//
// 覆盖的 flag：GLOB_NOSORT / GLOB_MARK / GLOB_NOCHECK / GLOB_ERR，以及
// GLOB_DOOFFS（配合 glob_t::gl_offs 的保留槽位）。其余 flag 一律忽略：
// GLOB_APPEND 的结果拼接、GLOB_NOESCAPE、GLOB_PERIOD、GLOB_BRACE 这些要么
// 本实现不支持、要么我们上面说的调用路径用不到，忽略它们不影响正确性。
// ---------------------------------------------------------------------------
#include <dirent.h>
#include <fnmatch.h>
#include <glob.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// 结果数组的构造。POSIX 的 glob_t 用一段连续内存表示结果：
//   [gl_offs 个保留槽位] [gl_pathc 个路径指针] [nullptr 结束标记]
// calloc 出来的内存天然就是这个布局（保留槽位与结束标记都是空指针），
// 只要把路径指针逐个填进去即可。
bool glob_build_result(const std::vector<std::string>& matches, int flags,
                       glob_t* pglob) {
    const std::size_t offs = ((flags & GLOB_DOOFFS) != 0) ? pglob->gl_offs : 0;
    char** table = static_cast<char**>(
        std::calloc(offs + matches.size() + 1, sizeof(char*)));
    if (table == nullptr) {
        return false;
    }
    pglob->gl_offs = offs;
    pglob->gl_pathv = table;
    pglob->gl_pathc = 0;
    for (std::size_t i = 0; i < matches.size(); ++i) {
        const std::string& match = matches[i];
        char* slot = static_cast<char*>(std::malloc(match.size() + 1));
        if (slot == nullptr) {
            // 只装了一半：gl_pathc 停在已填数量上，globfree 仍能按
            // gl_offs + gl_pathc 正确释放，不会漏也不会越界。
            return false;
        }
        std::memcpy(slot, match.c_str(), match.size() + 1);
        table[offs + i] = slot;
        pglob->gl_pathc = i + 1;
    }
    return true;
}

}  // namespace

// 签名与 Bionic 的 glob.h 保持一致，保证与 FFmpeg 那侧按同一个 glob_t 布局
// 读写（gl_pathc / gl_pathv / gl_offs 的偏移必须对得上）。
extern "C" int glob(const char* pattern, int flags,
                    int (*errfunc)(const char*, int), glob_t* pglob) {
    if (pglob == nullptr) {
        return GLOB_NOSPACE;
    }
    // 不支持 GLOB_APPEND 的结果拼接：进来先清空，免得把上一次的指针留在结构里
    // 变成悬空引用。POSIX 容许实现不支持 APPEND。
    pglob->gl_pathc = 0;
    pglob->gl_pathv = nullptr;

    std::string spec(pattern != nullptr ? pattern : "");
    const std::string original = spec;

    // 尾随 '/' 只表示「匹配到的必须是目录」，去掉它再做目录/文件名的拆分；
    // 根路径 "/" 本身要保留，否则会被拆成空字符串。
    bool require_dir = false;
    while (spec.size() > 1 && spec.back() == '/') {
        spec.pop_back();
        require_dir = true;
    }
    if (spec.empty()) {
        return GLOB_NOMATCH;  // 空模式不匹配任何东西
    }

    std::vector<std::string> matches;

    if (spec == "/") {
        // 根目录只有一个候选，就是它自己。
        matches.push_back("/");
    } else {
        const std::size_t slash = spec.find_last_of('/');
        const bool has_dir = (slash != std::string::npos);
        std::string dir = ".";
        std::string name_pattern = spec;
        if (has_dir) {
            dir = spec.substr(0, slash);
            if (dir.empty()) {
                dir = "/";
            }
            name_pattern = spec.substr(slash + 1);
        }

        DIR* dp = ::opendir(dir.c_str());
        if (dp == nullptr) {
            // 目录打不开按 GLOB_ABORTED 返回，而不是当成「没匹配上」——
            // 把「不存在」和「读不了」混为一谈，会让调用方做出错误判断。
            if (errfunc != nullptr) {
                errfunc(dir.c_str(), errno);
            }
            return GLOB_ABORTED;
        }

        // 模式以 '.' 开头时，前导点是普通字符（"." ".." 也因此可见）；否则
        // 交给 FNM_PERIOD：通配符不许吃开头的点，隐藏文件必须显式写出来。
        const int fn_flags =
            (!name_pattern.empty() && name_pattern[0] == '.') ? 0 : FNM_PERIOD;

        bool aborted = false;
        int read_error = 0;
        for (;;) {
            // readdir 只会在出错时置 errno，每次调用前清零才能判断循环是
            // 正常读完还是中途出错（循环体内的 stat 等也可能改 errno）。
            errno = 0;
            struct dirent* entry = ::readdir(dp);
            if (entry == nullptr) {
                read_error = errno;
                break;
            }

            const std::string name(entry->d_name);
            if (::fnmatch(name_pattern.c_str(), name.c_str(), fn_flags) != 0) {
                continue;
            }

            std::string full;
            if (!has_dir) {
                full = name;  // 模式里没有 '/'：结果就是文件名本身，不带 "./"
            } else if (dir == "/") {
                full = "/" + name;
            } else {
                full = dir + "/" + name;
            }

            bool is_dir = false;
            if ((flags & GLOB_MARK) != 0 || require_dir) {
                struct stat st {};
                if (::stat(full.c_str(), &st) == 0) {
                    is_dir = S_ISDIR(st.st_mode);
                } else if ((flags & GLOB_ERR) != 0) {
                    // 调用方要求「出错即停」：转告 errfunc 并放弃整次查询，
                    // 免得一份残缺的结果被当成完整的。
                    if (errfunc != nullptr) {
                        errfunc(full.c_str(), errno);
                    }
                    aborted = true;
                    break;
                }
            }
            if (require_dir && !is_dir) {
                continue;
            }
            if ((flags & GLOB_MARK) != 0 && is_dir && full.back() != '/') {
                full.push_back('/');
            }
            matches.push_back(full);
        }

        ::closedir(dp);

        if (aborted) {
            return GLOB_ABORTED;
        }
        if ((flags & GLOB_ERR) != 0 && read_error != 0) {
            if (errfunc != nullptr) {
                errfunc(dir.c_str(), read_error);
            }
            return GLOB_ABORTED;
        }
    }

    if (matches.empty()) {
        if ((flags & GLOB_NOCHECK) != 0) {
            // 一个都没匹配上时，按 POSIX 把模式本身当作唯一结果返回。
            // 这里原样返回，不做反斜杠反转义（本实现把反斜杠当转义符用，
            // 差别只在带转义的极端模式上）。
            matches.push_back(original);
        } else {
            return GLOB_NOMATCH;
        }
    }

    if ((flags & GLOB_NOSORT) == 0) {
        std::sort(matches.begin(), matches.end());
    }

    return glob_build_result(matches, flags, pglob) ? 0 : GLOB_NOSPACE;
}

// 释放 glob 自己分配的内存：每个非空的路径指针，以及承载它们的指针数组。
// 前 gl_offs 个槽位是保留位，恒为空指针，free(nullptr) 是安全的。
extern "C" void globfree(glob_t* pglob) {
    if (pglob == nullptr || pglob->gl_pathv == nullptr) {
        return;
    }
    const std::size_t total = pglob->gl_offs + pglob->gl_pathc;
    for (std::size_t i = 0; i < total; ++i) {
        std::free(pglob->gl_pathv[i]);
    }
    std::free(pglob->gl_pathv);
    pglob->gl_pathv = nullptr;
    pglob->gl_pathc = 0;
    pglob->gl_offs = 0;
}

#endif
