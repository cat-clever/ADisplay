// Windows 兼容垫片的实现。
//
// 只编译进 Windows 构建 —— 别的平台这些接口由系统提供，这个文件是空的。
// 声明与理由见 compat/win32/AdisplayWin32Compat.h 与 compat/win32/pthread.h。

#if defined(_MSC_VER)

#include "AdisplayWin32Compat.h"

#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pthread.h"

// ---------------------------------------------------------------------------
// 时钟与睡眠
// ---------------------------------------------------------------------------

int adisplay_clock_gettime(int clock_id, struct timespec* out_time) {
    (void) clock_id;   // 只有实时钟一种，CLOCK_REALTIME 映射到 0
    if (out_time == NULL) {
        return -1;
    }
    // timespec_get 是 C11 标准函数，MSVC 2015 起有。
    return timespec_get(out_time, TIME_UTC) == TIME_UTC ? 0 : -1;
}

void adisplay_usleep(unsigned int microseconds) {
    // Sleep 的粒度是毫秒。向上取整而不是截断：UxPlay 的轮询循环用
    // sleepms(1) 这种小值，截断会变成 0 从而变成忙等，把 CPU 打满。
    if (microseconds == 0) {
        return;
    }
    Sleep((microseconds + 999u) / 1000u);
}

// ---------------------------------------------------------------------------
// 线程
// ---------------------------------------------------------------------------

// 线程入口需要两个参数，而 _beginthreadex 只接受一个 void*，
// 所以在堆上中转一次。由线程自己负责释放。
typedef struct {
    void* (*start)(void*);
    void* arg;
} AdisplayThreadStart;

static unsigned __stdcall adisplay_thread_entry(void* raw) {
    AdisplayThreadStart* start = (AdisplayThreadStart*) raw;
    void* (*fn)(void*) = start->start;
    void* arg = start->arg;
    free(start);
    fn(arg);
    return 0;
}

int pthread_create(pthread_t* thread, const void* attr, void* (*start)(void*), void* arg) {
    (void) attr;
    if (thread == NULL || start == NULL) {
        return 1;
    }

    AdisplayThreadStart* entry = (AdisplayThreadStart*) malloc(sizeof(*entry));
    if (entry == NULL) {
        return 1;
    }
    entry->start = start;
    entry->arg = arg;

    // 用 _beginthreadex 而不是 CreateThread：前者会初始化一份 CRT 状态，
    // 线程里调 malloc / fopen 之类才安全 —— 协议层确实会在工作线程里分配内存。
    const uintptr_t handle = _beginthreadex(NULL, 0, adisplay_thread_entry, entry, 0, NULL);
    if (handle == 0) {
        free(entry);
        return 1;
    }

    *thread = (pthread_t) handle;
    return 0;
}

int pthread_join(pthread_t thread, void** retval) {
    // 上游的线程函数返回值没人取（THREAD_JOIN 传的是 NULL）。
    (void) retval;
    if (thread == NULL) {
        return 0;
    }
    // 线程收到信号就结束，等它彻底退出再关句柄 —— 关早了句柄就无效了。
    WaitForSingleObject((HANDLE) thread, INFINITE);
    CloseHandle((HANDLE) thread);
    return 0;
}

// ---------------------------------------------------------------------------
// 互斥量
//
// 用 SRWLOCK。这里要的是「全零即有效」这个性质，理由见 compat/win32/pthread.h ——
// 上游有一处互斥量从未初始化就直接上锁，而零初始化的 SRWLOCK 恰好合法。
// ---------------------------------------------------------------------------

int pthread_mutex_init(pthread_mutex_t* mutex, const void* attr) {
    (void) attr;
    if (mutex == NULL) {
        return 1;
    }
    // SRWLOCK_INIT 就是全零，所以清零即为初始化。
    memset(mutex, 0, sizeof(*mutex));
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (mutex == NULL) {
        return 1;
    }
    AcquireSRWLockExclusive(mutex);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    if (mutex == NULL) {
        return 1;
    }
    ReleaseSRWLockExclusive(mutex);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex) {
    if (mutex == NULL) {
        return 1;
    }
    memset(mutex, 0, sizeof(*mutex));
    return 0;
}

// ---------------------------------------------------------------------------
// 条件变量
// ---------------------------------------------------------------------------

int pthread_cond_init(pthread_cond_t* cond, const void* attr) {
    (void) attr;
    if (cond == NULL) {
        return 1;
    }
    InitializeConditionVariable(&cond->cv);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* cond) {
    (void) cond;   // Win32 的条件变量不需要释放
    return 0;
}

int pthread_cond_signal(pthread_cond_t* cond) {
    if (cond == NULL) {
        return 1;
    }
    WakeConditionVariable(&cond->cv);
    return 0;
}

int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if (cond == NULL || mutex == NULL) {
        return 1;
    }
    // 这个调用会原子地释放锁并在被唤醒后重新获取 —— 与 pthread_cond_wait
    // 的约定一致，上游就是靠它把睡眠放在锁外的。
    SleepConditionVariableSRW(&cond->cv, mutex, INFINITE, 0);
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                           const struct timespec* abstime) {
    if (cond == NULL || mutex == NULL || abstime == NULL) {
        return 1;
    }

    // Win32 这里要的是「等多久」，而 POSIX 给的是「等到什么时候」，
    // 所以先取当前时刻换算。两边用的是同一个时基：adisplay_clock_gettime
    // 也是走 timespec_get(TIME_UTC)。
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        return 1;
    }

    const int64_t delta_ms =
        ((int64_t) abstime->tv_sec - (int64_t) now.tv_sec) * 1000 +
        ((int64_t) abstime->tv_nsec - (int64_t) now.tv_nsec) / 1000000;
    int64_t wait_ms = delta_ms;
    if (wait_ms < 0) {
        wait_ms = 0;
    }
    if (wait_ms > 0x7fffffffLL) {
        wait_ms = 0x7fffffffLL;
    }

    // 返回 0 表示被唤醒，非 0 表示超时 —— 与 POSIX 一致。
    // 上游不检查这个返回值（它只是拿它当一次定时睡眠用），但语义给对总是好的。
    if (!SleepConditionVariableSRW(&cond->cv, mutex, (DWORD) wait_ms, 0)) {
        return 1;
    }
    return 0;
}

#endif  // _MSC_VER
