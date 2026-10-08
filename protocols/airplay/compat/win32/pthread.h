// 只给 UxPlay 协议层使用的 pthread 替身。
//
// 为什么不用现成的 pthreads-win32（vcpkg 的 pthreads）：它的 pthread_t 不是
// 可以赋 0 的类型，而上游的 THREAD_CREATE 宏里恰好有一句 `handle = 0`，
// 在 MSVC 上直接编不过。协议层用到的 pthread 接口只有下面这十三个，全部是
// Win32 原语的一对一包装，自己写一层比迁就第三方的类型定义更省事，也顺带
// 去掉一个依赖。
//
// 这个头只在编译协议层时出现在 include 路径上（见 protocols/airplay/CMakeLists.txt），
// 不会影响项目里其他任何代码。
#ifndef ADISPLAY_UXPLAY_WIN32_PTHREAD_H
#define ADISPLAY_UXPLAY_WIN32_PTHREAD_H

#if defined(_MSC_VER)

#include <windows.h>
#include <time.h>

// HANDLE 是指针，所以上游那句 `handle = 0` 是合法的 —— 这正是不能用
// pthreads-win32 的原因。
typedef HANDLE pthread_t;

// 临界区与 POSIX 互斥量语义一致（同一线程可重入），够用了。
typedef CRITICAL_SECTION pthread_mutex_t;

// Win32 的条件变量没有独立的销毁动作，包一层只为对齐类型名。
typedef struct {
    CONDITION_VARIABLE cv;
} pthread_cond_t;

int pthread_create(pthread_t* thread, const void* attr, void* (*start)(void*), void* arg);
int pthread_join(pthread_t thread, void** retval);

int pthread_mutex_init(pthread_mutex_t* mutex, const void* attr);
int pthread_mutex_lock(pthread_mutex_t* mutex);
int pthread_mutex_unlock(pthread_mutex_t* mutex);
int pthread_mutex_destroy(pthread_mutex_t* mutex);

int pthread_cond_init(pthread_cond_t* cond, const void* attr);
int pthread_cond_destroy(pthread_cond_t* cond);
int pthread_cond_signal(pthread_cond_t* cond);
int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
// abstime 是绝对时间（相对实时钟），与 POSIX 一致 —— 上游就是这么用的：
// clock_gettime(CLOCK_REALTIME) 之后加上超时秒数。
int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                           const struct timespec* abstime);

#endif  // _MSC_VER

#endif  // ADISPLAY_UXPLAY_WIN32_PTHREAD_H
