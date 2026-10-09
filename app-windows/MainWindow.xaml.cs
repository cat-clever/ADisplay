// ADisplay —— Windows 主窗口
//
// 界面刻意做得很薄：所有状态都在 castcore 里，这里只负责显示和转发用户操作。
// 文档 v2 第 5.2 节的设计前提就是「界面层与核心之间只通过一组 C 接口通信」，
// 所以这个文件里不应该出现任何协议相关的逻辑。

using System;
using System.Collections.ObjectModel;
using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;
using Windows.Media.Core;
using Windows.Media.Playback;
using ADisplay.Windows.Interop;

namespace ADisplay.Windows;

public sealed partial class MainWindow : Window
{
    // 日志只留最近这些行。长时间挂着接收服务时，无限增长会拖慢界面。
    private const int MaxLogLines = 500;

    private readonly AdEngine _engine = new();
    private readonly DispatcherQueue _dispatcher;

    // 当前投屏会话。0 表示没有在投屏。
    private uint _castSessionId;
    // 每秒把播放器状态回报给核心。手机每隔一段时间会拉 GetPositionInfo，
    // 位置必须持续更新，否则手机上的进度条一直停在起点。
    private DispatcherTimer? _reportTimer;
    private readonly ObservableCollection<string> _logLines = new();

    // 程序化改动 ToggleSwitch.IsOn 时会再次触发 Toggled，
    // 用它避免在失败回滚时递归调用 Start/Stop。
    private bool _suppressToggle;

    public MainWindow()
    {
        StartupLog.Enter("MainWindow 构造函数");

        // XAML 解析失败是「进程活着却没有窗口」的头号嫌疑，这一对进入/离开
        // 正好把它圈出来：日志停在「进入」而没有「离开」，就是解析这一步炸了。
        StartupLog.Enter("MainWindow.InitializeComponent()");
        InitializeComponent();
        StartupLog.Leave("MainWindow.InitializeComponent()");

        _dispatcher = DispatcherQueue.GetForCurrentThread();
        LogList.ItemsSource = _logLines;
        // 投屏页的日志区跟设置页共用同一个集合，两边同步。
        CastingLogList.ItemsSource = _logLines;

        _engine.StateChanged += OnEngineStateChanged;
        _engine.LogEmitted += OnEngineLogEmitted;
        _engine.MediaUrlReceived += OnMediaUrlReceived;
        _engine.PlaybackCommandReceived += OnPlaybackCommandReceived;
        _engine.CastingEnded += OnCastingEnded;

        Closed += OnWindowClosed;

        // 创建核心引擎要加载 castcore.dll 及其原生依赖，是启动路上第二容易出事的地方。
        StartupLog.Enter("InitializeEngine");
        InitializeEngine();
        StartupLog.Leave("InitializeEngine");

        StartupLog.Leave("MainWindow 构造函数");
    }

    private void InitializeEngine()
    {
        try
        {
            // 核心自带一套日志（adisplay.log，默认落在 %APPDATA%\ADisplay），
            // 但它要等 Create 成功之后才开写。这里的启动日志比它早，负责的是
            // 「引擎还没起来、或压根起不来」的那一段，两者不重复也不共用文件。
            StartupLog.Enter("_engine.Create");
            _engine.Create(
                deviceName: string.Empty,   // 空表示由核心取主机名作为默认设备名
                logFilePath: string.Empty,  // 空表示用平台默认日志路径
                configFilePath: string.Empty);
            StartupLog.Leave("_engine.Create");

            DeviceNameBox.Text = _engine.DeviceName;
            DeviceIdRun.Text = _engine.DeviceId;
            AddressRun.Text = FormatAddresses(_engine.LocalAddresses);
            VersionRun.Text = ReadVersion();

            AppendLog(AdLogLevel.Info, "ADisplay 已就绪。打开开关即可开始接收投屏。");
        }
        catch (Exception ex)
        {
            StartupLog.Failed("_engine.Create", ex);
            StatusText.Text = ex.Message;
            AppendLog(AdLogLevel.Error, ex.Message);
            ServiceToggle.IsEnabled = false;
        }
    }

    private static string ReadVersion()
    {
        IntPtr text = AdNative.ad_version_string();
        if (text == IntPtr.Zero)
        {
            return "未知";
        }
        return Marshal.PtrToStringAnsi(text) ?? "未知";
    }

    // 核心返回的地址是换行分隔的，直接塞进一行会很难看。
    private static string FormatAddresses(string raw)
    {
        if (string.IsNullOrEmpty(raw))
        {
            return "（未检测到可用的局域网地址）";
        }
        string[] lines = raw.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        return lines.Length == 0 ? "（未检测到可用的局域网地址）" : string.Join("，", lines);
    }

    // ---------------------------------------------------------------------
    // 服务开关
    // ---------------------------------------------------------------------

    private void OnServiceToggled(object sender, RoutedEventArgs e)
    {
        if (_suppressToggle)
        {
            return;
        }

        if (ServiceToggle.IsOn)
        {
            // 成对记录进出。原生崩溃不会抛托管异常，try/catch 接不住 ——
            // 那种情况下日志会停在「进入」而没有「离开」，一眼就能看出
            // 是死在核心库里，而不是界面层或配置读写。
            StartupLog.Enter("_engine.Start");
            try
            {
                _engine.Start();
                StartupLog.Leave("_engine.Start");
            }
            catch (Exception ex)
            {
                StartupLog.Failed("_engine.Start", ex);
                // 启动失败（最常见的是端口被占）要立刻把开关拨回去，
                // 否则界面会显示成「已开启」但实际没在监听。
                AppendLog(AdLogLevel.Error, ex.Message);
                SetToggleSilently(false);
                StatusText.Text = ex.Message;
            }
        }
        else
        {
            _engine.Stop();
        }
    }

    private void SetToggleSilently(bool value)
    {
        _suppressToggle = true;
        ServiceToggle.IsOn = value;
        _suppressToggle = false;
    }

    // ---------------------------------------------------------------------
    // 设备名称
    // ---------------------------------------------------------------------

    private void OnDeviceNameTextChanged(object sender, TextChangedEventArgs e)
    {
        string? reason = AdEngine.ValidateDeviceName(DeviceNameBox.Text);

        if (reason == null)
        {
            NameHintText.Visibility = Visibility.Collapsed;
            ApplyNameButton.IsEnabled = true;
        }
        else
        {
            NameHintText.Text = reason;
            NameHintText.Visibility = Visibility.Visible;
            ApplyNameButton.IsEnabled = false;
        }
    }

    private void OnApplyNameClick(object sender, RoutedEventArgs e)
    {
        try
        {
            _engine.DeviceName = DeviceNameBox.Text;
            // 核心改完名称会重新注册 mDNS / SSDP，手机端列表几秒内跟着变。
            AppendLog(AdLogLevel.Info, $"设备名称已保存为「{DeviceNameBox.Text}」。手机端列表可能需要几秒刷新。");
        }
        catch (Exception ex)
        {
            AppendLog(AdLogLevel.Error, ex.Message);
        }
    }

    // ---------------------------------------------------------------------
    // 来自核心的事件
    //
    // 回调在核心的工作线程上触发，必须切回 UI 线程再碰控件。
    // ---------------------------------------------------------------------

    // ---------------------------------------------------------------------
    // 投屏播放（文档 3.2、4.1）
    //
    // 核心不播放：DLNA 给的是一条 URL，解码渲染交给系统播放器 ——
    // MediaPlayerElement 背后是 Media Foundation，硬解与音画同步都是现成的。
    // 这一层只做三件事：起播、执行手机发来的控制意图、把真实状态回报给核心。
    //
    // 第三条是必须的，不是可选优化：核心不碰播放器，手机的 GetTransportInfo /
    // GetPositionInfo / GetVolume 全靠这份回报作答。
    // ---------------------------------------------------------------------

    private void OnMediaUrlReceived(uint sessionId, string url)
    {
        // 回调在核心的工作线程上触发，控件只能在 UI 线程碰。
        _dispatcher.TryEnqueue(() => BeginCasting(sessionId, url));
    }

    private void OnPlaybackCommandReceived(int command, long value)
    {
        _dispatcher.TryEnqueue(() => ApplyPlaybackCommand(command, value));
    }

    private void OnCastingEnded()
    {
        _dispatcher.TryEnqueue(EndCasting);
    }

    private void BeginCasting(uint sessionId, string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out Uri? uri))
        {
            AppendLog(AdLogLevel.Error, $"投屏地址无法解析，播放器起不来：{url}");
            return;
        }

        _castSessionId = sessionId;
        AppendLog(AdLogLevel.Info, $"开始拉流：{url}");

        SettingsPanel.Visibility = Visibility.Collapsed;
        CastingPanel.Visibility = Visibility.Visible;
        CastingTitleText.Text = "正在接收投屏";

        PlayerElement.Source = MediaSource.CreateFromUri(uri);
        PlayerElement.MediaPlayer.Volume = 1.0;
        PlayerElement.MediaPlayer.Play();

        _reportTimer ??= new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _reportTimer.Tick -= OnReportTick;
        _reportTimer.Tick += OnReportTick;
        _reportTimer.Start();

        ReportPlayback();
    }

    private void EndCasting()
    {
        // 项目约定不用 ?. 空条件运算符，写成显式判断。
        if (_reportTimer != null)
        {
            _reportTimer.Stop();
        }
        _castSessionId = 0;

        PlayerElement.Source = null;
        CastingPanel.Visibility = Visibility.Collapsed;
        SettingsPanel.Visibility = Visibility.Visible;
    }

    private void OnStopCastingClick(object sender, RoutedEventArgs e)
    {
        // 只结束本地播放：核心那边下次收到推送会重新开会话。
        EndCasting();
    }

    private void OnReportTick(object? sender, object e)
    {
        ReportPlayback();
    }

    private void ApplyPlaybackCommand(int command, long value)
    {
        MediaPlayer? player = PlayerElement.MediaPlayer;
        if (player == null)
        {
            return;
        }

        switch ((AdPlaybackCommand)command)
        {
            case AdPlaybackCommand.Play:
                player.Play();
                break;
            case AdPlaybackCommand.Pause:
                player.Pause();
                break;
            case AdPlaybackCommand.Seek:
                player.PlaybackSession.Position = TimeSpan.FromMilliseconds(value);
                break;
            case AdPlaybackCommand.SetVolume:
                player.Volume = Math.Clamp(value, 0, 100) / 100.0;
                break;
            case AdPlaybackCommand.SetMute:
                player.IsMuted = value != 0;
                break;
            case AdPlaybackCommand.Stop:
                // DLNA 的 Stop 是「停止播放」而不是「结束投屏」：媒体还挂着，
                // 手机随后可以再 Play。所以回到起点并暂停，不销毁会话。
                player.Pause();
                player.PlaybackSession.Position = TimeSpan.Zero;
                break;
        }

        // 控制结果立刻回报，不等下一拍。
        ReportPlayback();
    }

    private void ReportPlayback()
    {
        MediaPlayer? player = PlayerElement.MediaPlayer;
        if (_castSessionId == 0 || player == null)
        {
            return;
        }

        MediaPlaybackSession session = player.PlaybackSession;

        int transport = session.PlaybackState switch
        {
            MediaPlaybackState.Playing => (int)AdTransportState.Playing,
            MediaPlaybackState.Paused => (int)AdTransportState.Paused,
            MediaPlaybackState.Opening => (int)AdTransportState.Transitioning,
            MediaPlaybackState.Buffering => (int)AdTransportState.Transitioning,
            _ => (int)AdTransportState.Stopped,
        };

        // 直播流的 NaturalDuration 是 0 或者无穷，这种按「未知」(-1) 上报，
        // 否则手机上会出现一条荒谬的进度条。
        long durationMs = -1;
        if (session.NaturalDuration > TimeSpan.Zero &&
            !double.IsInfinity(session.NaturalDuration.TotalMilliseconds))
        {
            durationMs = (long)session.NaturalDuration.TotalMilliseconds;
        }

        _engine.ReportPlayback(
            _castSessionId,
            transport,
            (long)session.Position.TotalMilliseconds,
            durationMs,
            (int)Math.Round(player.Volume * 100),
            player.IsMuted ? 1 : 0);
    }

    private void OnEngineStateChanged(AdServiceState state)
    {
        _dispatcher.TryEnqueue(() => ApplyState(state));
    }

    private void OnEngineLogEmitted(AdLogLevel level, string message)
    {
        _dispatcher.TryEnqueue(() => AppendLog(level, message));
    }

    private void ApplyState(AdServiceState state)
    {
        StatusText.Text = DescribeState(state);

        switch (state)
        {
            case AdServiceState.Stopped:
            case AdServiceState.Error:
                SetToggleSilently(false);
                break;
            case AdServiceState.Running:
            case AdServiceState.Streaming:
                SetToggleSilently(true);
                break;
            default:
                break;
        }
    }

    private static string DescribeState(AdServiceState state)
    {
        switch (state)
        {
            case AdServiceState.Stopped:
                return "未启动";
            case AdServiceState.Starting:
                return "正在启动…";
            case AdServiceState.Running:
                return "正在广播，等待手机连接";
            case AdServiceState.Streaming:
                return "正在投屏";
            case AdServiceState.Stopping:
                return "正在停止…";
            case AdServiceState.Error:
                return "启动失败";
            default:
                return "未知状态";
        }
    }

    // ---------------------------------------------------------------------
    // 日志
    // ---------------------------------------------------------------------

    private void AppendLog(AdLogLevel level, string message)
    {
        string stamp = DateTime.Now.ToString("HH:mm:ss");
        _logLines.Add($"[{stamp}] [{level.ToString().ToUpperInvariant()}] {message}");

        while (_logLines.Count > MaxLogLines)
        {
            _logLines.RemoveAt(0);
        }

        // 刚 Add 完时布局还没更新，此刻读 ScrollableHeight 拿到的是旧值，
        // 滚动会差一行。再排一次队，等这一帧构建完再滚。
        _dispatcher.TryEnqueue(() =>
        {
            LogScrollViewer.ChangeView(null, LogScrollViewer.ScrollableHeight, null);
            if (CastingLogPanel.Visibility == Visibility.Visible)
            {
                CastingLogScrollViewer.ChangeView(null, CastingLogScrollViewer.ScrollableHeight, null);
            }
        });
    }

    private void OnCastingLogToggled(object sender, RoutedEventArgs e)
    {
        CastingLogPanel.Visibility = CastingLogToggle.IsChecked == true
            ? Visibility.Visible
            : Visibility.Collapsed;
    }

    // 日志区每行是独立的 TextBlock，只能一行行选。这个按钮把全部日志
    // 一次性放进剪贴板，方便贴到别处排查。
    private void OnCopyLogClick(object sender, RoutedEventArgs e)
    {
        if (_logLines.Count == 0)
        {
            return;
        }

        DataPackage package = new DataPackage();
        package.SetText(string.Join(Environment.NewLine, _logLines));
        Clipboard.SetContent(package);
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        // 先摘事件再销毁引擎，避免销毁过程中的状态回调打到已经在拆的界面上。
        _engine.StateChanged -= OnEngineStateChanged;
        _engine.LogEmitted -= OnEngineLogEmitted;
        _engine.Dispose();
    }
}
