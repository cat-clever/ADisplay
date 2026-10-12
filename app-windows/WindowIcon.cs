// ADisplay —— 窗口图标
//
// exe 自身的图标由 csproj 的 <ApplicationIcon> 嵌进去了（资源管理器里看到的就是它），
// 但 WinUI 的窗口**不会**自动去用它：不调用 SetIcon，标题栏、任务栏、Alt-Tab 里就是
// 系统默认的那个空白图标。主窗与日志窗都要各设一次。
//
// 三条细节都是踩出来的：
//   * 路径要基于 AppContext.BaseDirectory —— unpackaged 应用的当前工作目录不一定
//     是 exe 所在目录（从别的地方双击、或从资源管理器以管理员身份启动就会变）。
//   * SetIcon 需要 Windows App SDK 1.2 以上（本项目是 1.7）。
//   * **不能抛**：它是在窗口构造函数里调的，一个图标设不上不该让用户看到「程序打不开」——
//     那比没有图标难查得多。

using System;
using System.IO;
using Microsoft.UI.Xaml;

namespace ADisplay.Windows;

internal static class WindowIcon
{
    internal static void Apply(Window window)
    {
        try
        {
            string path = Path.Combine(AppContext.BaseDirectory, "adisplay.ico");
            if (!File.Exists(path))
            {
                StartupLog.Mark("窗口图标：包内没有 adisplay.ico，保持系统默认");
                return;
            }
            window.AppWindow.SetIcon(path);
        }
        catch (Exception error)
        {
            StartupLog.Mark("窗口图标设置失败（不影响使用）：" + error.Message);
        }
    }
}
