// ADisplay —— 界面侧启动日志
//
// 这份日志要回答的问题很具体：用户双击图标之后「什么都没发生」——
// 没有窗口、没有日志、没有任何提示。核心库（castcore）自己的日志要等
// ad_engine_create 成功之后才开始写，进程要是死在那之前，现场一点痕迹都不留，
// 所以需要一套不依赖核心、尽可能靠前的记录。
//
// 位置：优先 exe 同目录（绿色包解压出来日志就躺在 exe 边上，用户不用找），
// 该目录不可写时退到 %LOCALAPPDATA%\ADisplay\startup.log。
//
// 与核心日志的关系：核心的日志路径由 Config::default_log_path() 决定，
// 默认是 %APPDATA%\ADisplay\adisplay.log（跟 config.json 同目录），
// 由 ad_engine_create 的 log_file_path 参数传进去 —— 那是核心自己的东西。
// 这里是界面侧独立的一套，两边除了「都是诊断用的日志」之外没有交集，
// 不共用文件、也不会互相覆盖，改动一边不影响另一边。

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace ADisplay.Windows;

internal static class StartupLog
{
    // exe 同目录下的文件名。取一个不容易和别的文件混起来的名字，
    // 用户翻遍一堆 DLL 时也能一眼认出。
    private const string LogFileName = "ADisplay-startup.log";

    // 不用 Encoding.UTF8：它带 BOM，新建文件时会写出三个字节的不可见前缀，
    // 非记事本的编辑器打开会看到乱码。
    private static readonly UTF8Encoding Utf8NoBom = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);

    // 写入口可能来自任意线程 —— AppDomain / TaskScheduler 的异常回调不保证在
    // UI 线程上。加锁避免两条追加交叉成半行。
    private static readonly object Gate = new object();

    private static string? _logPath;
    private static bool _initialized;
    private static bool _initializing;

    // ---------------------------------------------------------------------
    // 入口
    // ---------------------------------------------------------------------

    // 模块初始化器会在本模块的任何代码之前运行：早于 XAML 编译器生成的 Main、
    // 早于 App 的构造函数，是托管侧能拿到的最早位置。再早就得往 C++ 侧加 native
    // 钩子，而 .NET 运行时本身加载失败时任何托管代码都救不了，所以到此为止。
    [ModuleInitializer]
    internal static void Init()
    {
        EnsureInitialized();
    }

    // 初始化整段裹在 try 里：诊断工具自己把进程弄崩，比没有这个工具更糟 ——
    // 用户看到的还是「打不开」，只是换了个原因。
    //
    // 之所以做成「谁先来谁触发」而不是只有模块初始化器一条路：unpackaged 形态下
    // Windows App SDK 的引导代码由它自己生成的模块初始化器负责，和我们的这个
    // 谁先执行不由这里决定。万一它排在前面又失败，我们这个就轮不到执行 ——
    // 那样 App 构造函数里的第一条 Mark 还能把日志拉起来。
    private static void EnsureInitialized()
    {
        if (_initialized || _initializing)
        {
            return;
        }

        _initializing = true;
        try
        {
            _logPath = ResolveLogPath();

            Mark("========== ADisplay 启动 ==========");
            Mark($"启动时刻：{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}");
            Mark($"日志文件：{_logPath}");
            DumpEnvironment();
            HookExceptions();

            // 只有整段都走完才算数；中途失败时留待下次调用重试，
            // 否则一次偶发失败（文件被占用之类）会让这次启动彻底没有日志。
            _initialized = true;
        }
        catch
        {
            _logPath = null;
        }
        finally
        {
            _initializing = false;
        }
    }

    /// <summary>记一行。任何时候都可以调，永不抛异常。</summary>
    internal static void Mark(string message)
    {
        EnsureInitialized();

        string? path = _logPath;
        if (path == null)
        {
            return;
        }

        try
        {
            // 每次都「打开—写—关闭」，没有缓冲区：进程随时可能被结束，
            // 留在缓冲区里的日志等于没写。也是因此不自己持有 StreamWriter ——
            // 那种写法要么忘了 flush，要么得处理跨线程和进程退出时的收尾。
            string line =
                $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} [线程 {Environment.CurrentManagedThreadId}] {message}{Environment.NewLine}";

            lock (Gate)
            {
                File.AppendAllText(path, line, Utf8NoBom);
            }
        }
        catch
        {
            // 磁盘满、文件被占用、权限中途被改……都不该影响主流程。
        }
    }

    // 进入 / 离开成对记录。日志的最后一行如果是「进入」而找不到对应的「离开」，
    // 卡住的位置就确定了：死在这一步里面，不用猜。
    internal static void Enter(string stage)
    {
        Mark($"→ 进入：{stage}");
    }

    internal static void Leave(string stage)
    {
        Mark($"← 离开：{stage}");
    }

    /// <summary>记录一个本该被自己兜住的步骤失败（例如创建核心引擎）。</summary>
    internal static void Failed(string stage, Exception ex)
    {
        Mark($"× 失败：{stage}");
        MarkException("捕获", ex);
    }

    // ---------------------------------------------------------------------
    // 日志文件位置
    // ---------------------------------------------------------------------

    private static string? ResolveLogPath()
    {
        foreach (string candidate in CandidatePaths())
        {
            if (TryPrepareFile(candidate))
            {
                return candidate;
            }
        }

        // 两条路都不通就静默放弃：写不了日志不是致命错误，不该让程序多一个死法。
        return null;
    }

    private static IEnumerable<string> CandidatePaths()
    {
        // 首选 exe 同目录：便携包解压出来日志就在 exe 边上，出了问题用户直接就能找到。
        string baseDirectory = AppContext.BaseDirectory;
        if (!string.IsNullOrEmpty(baseDirectory))
        {
            yield return Path.Combine(baseDirectory, LogFileName);
        }

        // 退路：装进 Program Files 这类普通用户没有写权限的目录时，上面那条会失败。
        string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (!string.IsNullOrEmpty(localAppData))
        {
            yield return Path.Combine(localAppData, "ADisplay", "startup.log");
        }
    }

    private static bool TryPrepareFile(string path)
    {
        try
        {
            string? directory = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(directory) && !Directory.Exists(directory))
            {
                Directory.CreateDirectory(directory);
            }

            // 空追加一次：既确认目录真的可写（只读目录下 CreateDirectory 不报错，
            // 真正写入才报），也顺手把文件建出来，用户第一次启动就能看到它。
            File.AppendAllText(path, string.Empty, Utf8NoBom);
            return true;
        }
        catch
        {
            return false;
        }
    }

    // ---------------------------------------------------------------------
    // 运行环境
    //
    // 一次写清。这些是远程排查时最想知道、又最难从用户嘴里问出来的东西：
    // 位数对不对、运行时是不是自包含那一份、该在的 DLL 在不在。
    // ---------------------------------------------------------------------

    private static void DumpEnvironment()
    {
        string baseDirectory = AppContext.BaseDirectory;

        Mark("---- 运行环境 ----");
        Mark($"运行时：{RuntimeInformation.FrameworkDescription}");
        Mark($"CLR 版本：{Environment.Version}");
        Mark($"操作系统：{Environment.OSVersion}");
        Mark($"OS 架构：{RuntimeInformation.OSArchitecture}，进程架构：{RuntimeInformation.ProcessArchitecture}");
        Mark($"64 位 OS：{Environment.Is64BitOperatingSystem}，64 位进程：{Environment.Is64BitProcess}");
        Mark($"基目录：{baseDirectory}");
        Mark($"进程路径：{Environment.ProcessPath}");

        // 这两个是 unpackaged + 自包含形态下最容易缺的东西：castcore.dll 是核心
        // 库本体，Microsoft.WindowsAppRuntime.dll 没了连 WinUI 都起不来。
        // 在不在，直接决定该往哪个方向查。
        Mark($"同目录 castcore.dll：{DescribeFile(baseDirectory, "castcore.dll")}");
        Mark($"同目录 Microsoft.WindowsAppRuntime.dll：{DescribeFile(baseDirectory, "Microsoft.WindowsAppRuntime.dll")}");
    }

    private static string DescribeFile(string directory, string fileName)
    {
        try
        {
            string fullPath = Path.Combine(directory, fileName);
            return File.Exists(fullPath) ? $"存在（{fullPath}）" : $"缺失（{fullPath}）";
        }
        catch
        {
            return "无法判断";
        }
    }

    // ---------------------------------------------------------------------
    // 未处理异常
    // ---------------------------------------------------------------------

    private static void HookExceptions()
    {
        // .NET 侧最后一道：能走到这里说明异常已经没人接了，进程通常马上就要退。
        AppDomain.CurrentDomain.UnhandledException += OnAppDomainUnhandledException;

        // 被 GC 判为「无人观察」的 Task 异常。默认不会让进程退出，但常常是
        // 某个后台步骤在悄悄失败的唯一线索，值得留一行。
        TaskScheduler.UnobservedTaskException += OnUnobservedTaskException;

        // 原生崩溃的钩子。装不上不是致命错误 —— 上面两条托管钩子还在。
        try
        {
            _nativeFilter = OnNativeUnhandledException;
            SetUnhandledExceptionFilter(_nativeFilter);
        }
        catch
        {
            _nativeFilter = null;
        }
    }

    // ---------------------------------------------------------------------
    // 原生异常
    //
    // 上面那些钩子只接托管异常。核心是 C++（castcore.dll），播放又经过
    // Media Foundation —— 访问违例、栈溢出这类崩溃根本不会变成 .NET 异常，
    // 进程直接被系统结束，日志里一行都不留。用户报「Windows 崩了、没有日志」，
    // 缺的就是这一段。
    //
    // SetUnhandledExceptionFilter 的过滤器在系统终止进程之前运行：在这里
    // 记下异常码、出错地址和它落在哪个模块（castcore.dll / mfplat.dll /
    // combase.dll …），再顺手写一个 minidump，事后就能接着查。
    // ---------------------------------------------------------------------

    [DllImport("kernel32.dll")]
    private static extern IntPtr SetUnhandledExceptionFilter(UnhandledExceptionFilter filter);

    private delegate int UnhandledExceptionFilter(IntPtr exceptionPointers);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool GetModuleHandleEx(uint flags, IntPtr moduleName, out IntPtr module);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetModuleFileName(IntPtr module, StringBuilder filename, uint size);

    [DllImport("dbghelp.dll", SetLastError = true)]
    private static extern bool MiniDumpWriteDump(
        IntPtr process, uint processId, IntPtr file, uint dumpType,
        IntPtr exceptionParam, IntPtr userStreamParam, IntPtr callbackParam);

    private const uint GetModuleHandleExFlagFromAddress = 0x00000004;
    private const uint GetModuleHandleExFlagUnchangedRefCount = 0x00000002;
    private const uint MiniDumpNormal = 0x00000000;

    // 必须一直拿着这个委托的引用：只把函数指针交给系统的话，托管侧下一次 GC
    // 就可能把它回收，崩溃时跳到一片已经释放的内存上 —— 那会比原来的崩溃更难查。
    private static UnhandledExceptionFilter? _nativeFilter;

    private static int OnNativeUnhandledException(IntPtr exceptionPointers)
    {
        try
        {
            uint code = 0;
            IntPtr address = IntPtr.Zero;

            if (exceptionPointers != IntPtr.Zero)
            {
                // EXCEPTION_POINTERS { ExceptionRecord; ContextRecord; }
                // EXCEPTION_RECORD 里 ExceptionCode 在偏移 0、ExceptionAddress
                // 在偏移 16（x86 与 x64 都是这个布局）。
                IntPtr record = Marshal.ReadIntPtr(exceptionPointers, 0);
                if (record != IntPtr.Zero)
                {
                    code = unchecked((uint)Marshal.ReadInt32(record, 0));
                    address = Marshal.ReadIntPtr(record, 16);
                }
            }

            Mark($"!! 原生异常：代码 0x{code:X8}，地址 0x{address.ToInt64():X}");
            Mark($"!! 出错位置：{DescribeAddress(address)}");

            WriteCrashDump(exceptionPointers);
        }
        catch
        {
            // 过滤器自己出错也必须往下走，不能挡住系统收尸。
        }

        // EXCEPTION_CONTINUE_SEARCH：证据留下，进程照原样终止（WER 也会跑）。
        return 0;
    }

    private static string DescribeAddress(IntPtr address)
    {
        if (address == IntPtr.Zero)
        {
            return "未知（地址为空）";
        }

        try
        {
            IntPtr module;
            if (!GetModuleHandleEx(
                    GetModuleHandleExFlagFromAddress | GetModuleHandleExFlagUnchangedRefCount,
                    address, out module) || module == IntPtr.Zero)
            {
                return "不在任何已加载模块内";
            }

            StringBuilder buffer = new StringBuilder(1024);
            uint length = GetModuleFileName(module, buffer, (uint)buffer.Capacity);
            string name = length > 0 ? buffer.ToString() : "未知模块";
            long offset = address.ToInt64() - module.ToInt64();
            return $"{name}+0x{offset:X}";
        }
        catch
        {
            return "无法解析";
        }
    }

    private static void WriteCrashDump(IntPtr exceptionPointers)
    {
        string? directory = _logPath != null ? Path.GetDirectoryName(_logPath) : null;
        if (string.IsNullOrEmpty(directory))
        {
            return;
        }

        // 固定文件名：崩溃可能连着发生，带时间戳的话转储会越攒越多，
        // 用户也说不清该发哪一份。覆盖上一份、永远只留最新的这次。
        string path = Path.Combine(directory, "ADisplay-crash.dmp");

        try
        {
            using FileStream stream = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None);
            bool ok = MiniDumpWriteDump(
                System.Diagnostics.Process.GetCurrentProcess().Handle,
                (uint)Environment.ProcessId,
                stream.SafeFileHandle.DangerousGetHandle(),
                MiniDumpNormal,
                exceptionPointers,
                IntPtr.Zero,
                IntPtr.Zero);

            Mark(ok
                ? $"崩溃转储已写入：{path}"
                : $"崩溃转储写入失败（错误码 {Marshal.GetLastWin32Error()}）：{path}");
        }
        catch (Exception ex)
        {
            Mark($"崩溃转储写入异常：{ex.GetType().Name}：{ex.Message}");
        }
    }

    /// <summary>
    /// 挂 WinUI 的未处理异常。这个事件长在 Application 实例上，
    /// 必须等实例存在才挂得上，所以由 App 构造函数调用，放不进模块初始化器。
    /// </summary>
    internal static void HookXamlExceptions(Microsoft.UI.Xaml.Application application)
    {
        application.UnhandledException += OnXamlUnhandledException;
    }

    private static void OnAppDomainUnhandledException(object sender, System.UnhandledExceptionEventArgs e)
    {
        Mark($"!! AppDomain 未处理异常，进程即将退出：{e.IsTerminating}");

        if (e.ExceptionObject is Exception exception)
        {
            MarkException("AppDomain", exception);
        }
        else
        {
            Mark($"AppDomain 异常对象不是 Exception：{e.ExceptionObject}");
        }
    }

    private static void OnUnobservedTaskException(object? sender, UnobservedTaskExceptionEventArgs e)
    {
        Mark("!! 未被观察的 Task 异常");
        MarkException("TaskScheduler", e.Exception);
    }

    private static void OnXamlUnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
    {
        // XAML 解析失败、控件模板出错都从这里出来，是「进程活着但没有窗口」的头号来源。
        Mark("!! XAML 未处理异常");
        MarkException("XAML", e.Exception);

        // 故意不设 e.Handled：维持「异常即终止」的默认行为。把异常吞掉只会让
        // 界面停在半初始化状态继续跑，症状更难解释；诊断要的是真相，不是遮掩。
    }

    private static void MarkException(string source, Exception? ex)
    {
        if (ex == null)
        {
            Mark($"{source}：异常对象为空");
            return;
        }

        Mark($"{source} 异常类型：{ex.GetType().FullName}");
        Mark($"{source} 异常消息：{ex.Message}");
        // ToString() 带堆栈，是远程定位的关键；原样写入，不做换行压缩。
        Mark($"{source} 异常详情：{ex}");

        // 加载 DLL 失败时真正的线索往往在内层：外层只有一句
        // "Unable to load DLL 'castcore'" ，底下才写着是哪个依赖没找到。
        Exception? inner = ex.InnerException;
        int depth = 1;
        while (inner != null && depth <= 8)
        {
            Mark($"{source} 内层异常 #{depth}：{inner.GetType().FullName}：{inner.Message}");
            inner = inner.InnerException;
            depth++;
        }
    }
}
