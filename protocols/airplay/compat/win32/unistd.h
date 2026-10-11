// UxPlay 的 threads.h 无条件 #include <unistd.h>，而 MSVC 没有这个头。
//
// 这个文件只在编译 UxPlay 协议层时出现在 include 路径上（见
// protocols/airplay/CMakeLists.txt），所以不会影响本项目的其他代码。
// 它本身只提供 threads.h 需要的 usleep，实现在 AdisplayWin32Compat.c 里。
#ifndef ADISPLAY_UXPLAY_WIN32_UNISTD_H
#define ADISPLAY_UXPLAY_WIN32_UNISTD_H

#include "AdisplayWin32Compat.h"

#endif
