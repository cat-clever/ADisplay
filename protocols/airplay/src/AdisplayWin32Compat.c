// Windows 兼容垫片的实现（声明与理由见 compat/win32/AdisplayWin32Compat.h）。
//
// 只编译进 Windows 构建 —— 在别的平台上这个文件是空的，
// 因为那些平台不需要补任何东西。

#if defined(_MSC_VER)

#include "AdisplayWin32Compat.h"

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
    const unsigned int milliseconds = (microseconds + 999u) / 1000u;
    Sleep(milliseconds);
}

#endif  // _MSC_VER
