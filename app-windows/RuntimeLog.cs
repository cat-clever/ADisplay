// ADisplay —— 界面日志落盘
//
// 日志窗口里那份日志只活在内存里（ObservableCollection）。进程一崩，界面没了，
// 日志也跟着没了 —— 用户能说的只剩「崩了」两个字。核心自己有一份日志文件
// （%APPDATA%\ADisplay\adisplay.log），但那只收核心打的行；界面侧的事件
// （开始拉流、媒体管线失败、镜像启动失败、播放器状态）全都不在里面，而崩溃
// 恰恰最可能发生在这些路径上。
//
// 所以这里把界面日志原样再写一份到磁盘：
//   * 位置与 StartupLog 一致 —— 优先 exe 同目录（绿色包解压出来就在 exe 边上），
//     不可写时退到 %LOCALAPPDATA%\ADisplay\runtime.log；
//   * 持有 StreamWriter 并 AutoFlush：每行都立刻交给操作系统，进程被直接结束
//     也不会丢已经写过的行。比每行「打开—写—关闭」省得多，而 AirPlay 那类
//     日志一秒能来几十行；
//   * 单个文件超过 4MB 就转成 .1，只留一份备份，避免长期挂着把磁盘写满。

using System;
using System.IO;
using System.Text;

namespace ADisplay.Windows;

internal static class RuntimeLog
{
    private const string FileName = "ADisplay-runtime.log";
    private const long MaxBytes = 4L * 1024 * 1024;

    // 不用 Encoding.UTF8：它带 BOM，日志文件开头会多出三个不可见字节。
    private static readonly UTF8Encoding Utf8NoBom = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);

    // 写入口可能来自任意线程（核心的日志回调、界面错误、以后的后台步骤），
    // 加锁避免两条追加交叉成半行。
    private static readonly object Gate = new object();

    private static StreamWriter? _writer;
    private static string? _path;

    /// <summary>记一行。任何时候都可以调，永不抛异常。</summary>
    internal static void Write(string level, string message)
    {
        lock (Gate)
        {
            try
            {
                StreamWriter? writer = EnsureWriter();
                if (writer == null)
                {
                    return;
                }

                writer.WriteLine($"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} [{level}] {message}");
            }
            catch (Exception)
            {
                // 写日志失败不能反过来把程序弄崩。关掉当前 writer，
                // 下一次调用会重新尝试（文件可能只是临时被占用）。
                CloseWriter();
            }
        }
    }

    /// <summary>「清除日志」用：把这份文件也清掉，否则用户以为清干净了。</summary>
    internal static void Clear()
    {
        lock (Gate)
        {
            // 先记下路径再关 writer —— CloseWriter 会把 _path 清空。
            string? path = _path;
            CloseWriter();

            if (path == null)
            {
                return;
            }

            TryDelete(path);
            TryDelete(path + ".1");
        }
    }

    private static StreamWriter? EnsureWriter()
    {
        if (_writer != null)
        {
            return _writer;
        }

        foreach (string candidate in CandidatePaths())
        {
            try
            {
                string? directory = Path.GetDirectoryName(candidate);
                if (!string.IsNullOrEmpty(directory) && !Directory.Exists(directory))
                {
                    Directory.CreateDirectory(directory);
                }

                RotateIfNeeded(candidate);

                // FileShare.ReadWrite：用户正用记事本看着日志时也能继续写。
                FileStream stream = new FileStream(candidate, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
                _path = candidate;
                _writer = new StreamWriter(stream, Utf8NoBom) { AutoFlush = true };

                // 每次打开都留一条分隔线：中途写失败重开、或者用户连着跑了几次，
                // 一眼就能看出从哪一行开始是这一次的。版本号也顺手记上 ——
                // 「是不是旧版本」是远程排查时第一个要排除的变量。
                _writer.WriteLine(
                    $"===== {DateTime.Now:yyyy-MM-dd HH:mm:ss} ADisplay {typeof(RuntimeLog).Assembly.GetName().Version} 界面日志：{candidate} =====");

                return _writer;
            }
            catch (Exception)
            {
                // 这个位置写不了（只读目录、权限不够……），换下一个候选。
            }
        }

        return null;
    }

    private static System.Collections.Generic.IEnumerable<string> CandidatePaths()
    {
        // 首选 exe 同目录：便携包解压出来日志就在 exe 边上，出了问题用户直接就能找到。
        string baseDirectory = AppContext.BaseDirectory;
        if (!string.IsNullOrEmpty(baseDirectory))
        {
            yield return Path.Combine(baseDirectory, FileName);
        }

        // 退路：装进 Program Files 这类普通用户没有写权限的目录时，上面那条会失败。
        string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (!string.IsNullOrEmpty(localAppData))
        {
            yield return Path.Combine(localAppData, "ADisplay", "runtime.log");
        }
    }

    private static void RotateIfNeeded(string path)
    {
        try
        {
            FileInfo info = new FileInfo(path);
            if (!info.Exists || info.Length < MaxBytes)
            {
                return;
            }

            // File.Delete 对不存在的文件不抛异常。
            File.Delete(path + ".1");
            File.Move(path, path + ".1");
        }
        catch (Exception)
        {
            // 转不动就接着往原文件写 —— 丢日志比文件超限更糟。
        }
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch (Exception)
        {
            // 删不掉就算了，下一次写入会接着追加。
        }
    }

    private static void CloseWriter()
    {
        // 项目约定不用 ?. 空条件运算符，写成显式判断。
        StreamWriter? writer = _writer;
        if (writer != null)
        {
            try
            {
                writer.Dispose();
            }
            catch (Exception)
            {
                // 关不掉也无所谓，进程退出时操作系统会收尾。
            }
        }

        _writer = null;
        _path = null;
    }
}
