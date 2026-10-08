// 只给 UxPlay 协议层使用的 Windows 兼容垫片。
//
// UxPlay 官方的 Windows 支持是「MinGW-64 + MSYS2」，而我们整套是 MSVC ——
// 两者的差别只落在少数几个 POSIX 接口上（逐文件清点过，就是下面这些），
// 所以不值得为它换编译器，补几个替身即可。
//
// 这个头通过 /FI 强制包含进协议层的每一个源文件，所以不依赖各文件自己的
// include 顺序；也正因为它会出现在每个编译单元里，内容必须是无副作用的声明。
//
// 它只挂在 adisplay_airplay_proto 这个目标上（PRIVATE include），
// 不会漏给项目里其他任何代码。
#ifndef ADISPLAY_UXPLAY_WIN32_COMPAT_H
#define ADISPLAY_UXPLAY_WIN32_COMPAT_H

#if defined(_MSC_VER)

// 先引 winsock2.h 再引 windows.h，顺序反了会拉进 winsock1 并报重定义。
// 强制包含的头在所有 #include 之前生效，正好适合担这个顺序责任。
#  include <winsock2.h>
#  include <windows.h>
#  include <BaseTsd.h>
#  include <time.h>

// UxPlay 用 ssize_t 接 recv 系列的返回值。MSVC 的对应类型叫 SSIZE_T。
#  ifndef _ADISPLAY_SSIZE_T_DEFINED
#    define _ADISPLAY_SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#  endif

// MSVC 把 strdup 归在「POSIX 已废弃」名下，改叫 _strdup。
#  ifndef strdup
#    define strdup _strdup
#  endif

// 时钟。
//
// UxPlay 在定时同步里用 clock_gettime(CLOCK_REALTIME, ...)，MSVC 没有这个函数。
// 这里映射到 C11 的 timespec_get —— 两者语义一致（取实时钟），
// 而 tv_sec / tv_nsec 的布局也相同，所以调用点不需要改。
//
// 用一个自己的名字而不是直接定义 clock_gettime 符号：万一某个 Windows SDK
// 版本将来提供了它，这里不会和系统符号撞车。
struct timespec;
int adisplay_clock_gettime(int clock_id, struct timespec* out_time);
#  ifndef CLOCK_REALTIME
typedef int adisplay_clockid_t;
#    define CLOCK_REALTIME 0
#  endif
#  ifndef clock_gettime
#    define clock_gettime adisplay_clock_gettime
#  endif

// 微秒级睡眠。
//
// 同样换成自己的名字：pthreads4w 自带一份 unistd.h，某些版本里就有 usleep，
// 直接定义同名符号可能在链接期撞车。
void adisplay_usleep(unsigned int microseconds);
#  ifndef usleep
#    define usleep adisplay_usleep
#  endif

#endif  // _MSC_VER

#endif  // ADISPLAY_UXPLAY_WIN32_COMPAT_H
