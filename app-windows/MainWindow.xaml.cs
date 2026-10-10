// ADisplay —— Windows 主窗口
//
// 界面刻意做得很薄：所有状态都在 castcore 里，这里只负责显示和转发用户操作。
// 文档 v2 第 5.2 节的设计前提就是「界面层与核心之间只通过一组 C 接口通信」，
// 所以这个文件里不应该出现任何协议相关的逻辑。

using System;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Net.Http;
using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Controls;
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
    // 镜像会话的帧源。null 表示当前不是镜像会话。
    private MirrorStreamSource? _mirrorSource;
    // 镜像伴音的播放端。核心里已经解成 PCM，这里只负责送进系统音频。
    private readonly MirrorAudioPlayer _mirrorAudio = new();
    // 独立的日志窗口。null 表示当前没开着。
    private LogWindow? _logWindow;
    private readonly ObservableCollection<string> _logLines = new();

    // 程序化改动 ToggleSwitch.IsOn 时会再次触发 Toggled，
    // 用它避免在失败回滚时递归调用 Start/Stop。
    private bool _suppressToggle;
    // 同理：程序化设置画质下拉框的选中项也会触发 SelectionChanged。
    private bool _suppressQuality;

    public MainWindow()
    {
        StartupLog.Enter("MainWindow 构造函数");

        // XAML 解析失败是「进程活着却没有窗口」的头号嫌疑，这一对进入/离开
        // 正好把它圈出来：日志停在「进入」而没有「离开」，就是解析这一步炸了。
        StartupLog.Enter("MainWindow.InitializeComponent()");
        InitializeComponent();
        StartupLog.Leave("MainWindow.InitializeComponent()");

        _dispatcher = DispatcherQueue.GetForCurrentThread();

        // 指针在投屏画面上一动，就把悬浮控件叫出来。
        //
        // 用 AddHandler(..., handledEventsToo: true) 而不是在 XAML 上挂
        // PointerMoved：MediaPlayerElement 自己会处理指针事件（它要靠悬停显示
        // 传输控件），XAML 上挂的处理器收不到已经被标记成 handled 的事件。
        CastingPanel.AddHandler(UIElement.PointerMovedEvent,
                                new PointerEventHandler(OnCastingPointerMoved),
                                true);
        // 日志集合由独立的日志窗口显示（见 LogWindow.xaml）。这里只持有它 ——
        // 谁显示、显示在哪，都是那个窗口自己的事。

        _engine.StateChanged += OnEngineStateChanged;
        _engine.LogEmitted += OnEngineLogEmitted;
        _engine.MediaUrlReceived += OnMediaUrlReceived;
        _engine.PlaybackCommandReceived += OnPlaybackCommandReceived;
        _engine.CastingEnded += OnCastingEnded;
        _engine.MirrorStarted += OnMirrorStarted;
        _engine.MirrorFrameReceived += OnMirrorFrameReceived;
        _engine.AudioFrameReceived += OnAudioFrameReceived;
        _mirrorAudio.Notice += OnMirrorNotice;

        // 播放器的打开 / 失败都要进日志（见 OnMediaFailed 的说明）。
        if (PlayerElement.MediaPlayer != null)
        {
            PlayerElement.MediaPlayer.MediaOpened += OnMediaOpened;
            PlayerElement.MediaPlayer.MediaFailed += OnMediaFailed;
        }

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
            // 档位的下拉项顺序与 AdQualityPreset 的取值一一对应（0/1/2）。
            _suppressQuality = true;
            QualityBox.SelectedIndex = ReadQualityIndex();
            _suppressQuality = false;
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
    // 画质档位（文档 2.3）
    // ---------------------------------------------------------------------

    /// <summary>读回已保存的档位。读不到就用「均衡」。</summary>
    private int ReadQualityIndex()
    {
        try
        {
            int preset = _engine.QualityPreset;
            if (preset >= 0 && preset <= 2)
            {
                return preset;
            }
        }
        catch (Exception ex)
        {
            AppendLog(AdLogLevel.Error, ex.Message);
        }
        return (int)AdQualityPreset.Balanced;
    }

    private void OnQualityChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressQuality || QualityBox.SelectedIndex < 0)
        {
            return;
        }
        try
        {
            _engine.SetQualityPreset((AdQualityPreset)QualityBox.SelectedIndex);
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

    // ---------------------------------------------------------------------
    // AirPlay 屏幕镜像
    //
    // 与 DLNA 那条路的区别只在「画面从哪来」：那边是一条 URL 交给播放器引擎，
    // 这边是持续的压缩帧。解码与渲染同样不归我们管 —— 帧喂进 MediaStreamSource，
    // 背后是 Media Foundation。
    // ---------------------------------------------------------------------

    private void OnMirrorStarted()
    {
        _dispatcher.TryEnqueue(() =>
        {
            if (_mirrorSource != null)
            {
                return;
            }
            // 会话号与 DLNA 共用一套编号，镜像这边只用来回报播放状态。
            _castSessionId = 0;

            _mirrorSource = new MirrorStreamSource();
            // 编码参数到齐之后才知道怎么解，那时才设播放源。
            _mirrorSource.Ready += OnMirrorReady;
            // 镜像渲染这条路上的失败都是静默的，表现统一是「界面正常、没有画面」。
            // 它自己说不出来的话，就只能靠猜。
            _mirrorSource.Notice += OnMirrorNotice;

            SettingsPanel.Visibility = Visibility.Collapsed;
            CastingPanel.Visibility = Visibility.Visible;
            CastingTitleText.Text = "正在镜像屏幕";
            // 刚进投屏先把控件亮出来一次：让用户知道现在什么状态、退路在哪，
            // 之后它自己会收起。
            ShowCastingControls();
            // 镜像这条没有 URL（帧是核心直接交下来的，不是渐进式下载），
            // 所以取不到文件大小，界面上只显示「正在缓冲」。
            StartBufferingWatch(null);
            AppendLog(AdLogLevel.Info, "iPhone 开始屏幕镜像");
        });
    }

    /// <summary>
    /// 媒体管线打开成功 / 失败都要说话。
    ///
    /// 镜像这条路上的失败是静默的：播放器不出画面，界面上就是一个转圈，
    /// 而「格式被拒」「系统没有 HEVC 解码器」「数据没喂进去」三种原因看起来
    /// 一模一样。MediaFailed 会带上具体错误码，是唯一能一句话分清它们的信号。
    /// </summary>
    private void OnMediaOpened(MediaPlayer sender, object args)
    {
        AppendLog(AdLogLevel.Info, "媒体管线已就绪，开始接收画面。");
    }

    private void OnMediaFailed(MediaPlayer sender, MediaPlayerFailedEventArgs args)
    {
        string detail = args.ErrorMessage;
        if (detail == null || detail.Length == 0)
        {
            detail = args.Error.ToString();
        }
        // 项目约定不用 ?. 空条件运算符，写成显式判断。
        string extended = "无扩展错误码";
        if (args.ExtendedErrorCode != null)
        {
            extended = "0x" + args.ExtendedErrorCode.HResult.ToString("X8");
        }
        AppendLog(AdLogLevel.Error,
            "媒体管线失败：" + detail + "（" + args.Error + "，" + extended + "）。"
            + "若是解码器缺失，装一下系统的「HEVC 视频扩展」即可。");
    }

    private void OnMirrorReady(MediaStreamSource source)
    {
        _dispatcher.TryEnqueue(() =>
        {
            PlayerElement.Source = MediaSource.CreateFromMediaStreamSource(source);
            PlayerElement.MediaPlayer.Volume = 1.0;
            PlayerElement.MediaPlayer.Play();
        });
    }

    private void OnAudioFrameReceived(float[] samples, int sampleRate, int channels)
    {
        // 不切回 UI 线程：伴音每秒约 92 帧，每帧跳一次会把主线程压满、声音断续。
        // 播放端只入队，音频图按自己的节奏来取。
        _mirrorAudio.Enqueue(samples, sampleRate, channels);
    }

    private void OnMirrorNotice(string message)
    {
        // 回调在核心的工作线程上触发，日志集合只能在 UI 线程碰。
        _dispatcher.TryEnqueue(() => AppendLog(AdLogLevel.Info, message));
    }

    private void OnMirrorFrameReceived(byte[] data, bool isH265, uint width, uint height, long ptsUs)
    {
        // 不切回 UI 线程：MediaStreamSource 是拉取式的，这里只需要把字节塞进队列。
        MirrorStreamSource? source = _mirrorSource;
        if (source != null)
        {
            source.Push(data, isH265, width, height, ptsUs);
        }
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
        ShowCastingControls();

        PlayerElement.Source = MediaSource.CreateFromUri(uri);
        PlayerElement.MediaPlayer.Volume = 1.0;
        PlayerElement.MediaPlayer.Play();

        _reportTimer ??= new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _reportTimer.Tick -= OnReportTick;
        _reportTimer.Tick += OnReportTick;
        _reportTimer.Start();

        StartBufferingWatch(url);

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
        // 伴音也停掉：会话结束了不该继续出声。
        _mirrorAudio.Stop();
        // 镜像的帧源要显式收掉：它挂着一次可能还没答复的拉取请求，
        // 放着不管会让管线一直等下去。
        if (_mirrorSource != null)
        {
            _mirrorSource.Ready -= OnMirrorReady;
            _mirrorSource.Notice -= OnMirrorNotice;
            _mirrorSource.Dispose();
            _mirrorSource = null;
        }
        ExitFullScreenOnCastingEnd();
        HideCastingControls();
        StopBufferingWatch();
        CastingPanel.Visibility = Visibility.Collapsed;
        SettingsPanel.Visibility = Visibility.Visible;
    }

    private void OnStopCastingClick(object sender, RoutedEventArgs e)
    {
        // 只结束本地播放、回设置页，**不停接收服务** —— 服务一停广播就撤了，
        // 手机那边立刻找不到这台机器。核心那边下次收到推送会重新开会话。
        EndCasting();
    }

    // ---------------------------------------------------------------------
    // 全屏
    //
    // 走 AppWindow 而不是 MediaPlayerElement.IsFullWindow：后者只让画面填满
    // 窗口，标题栏与边框还在原处，投屏时用户要的是整块屏幕。
    // ---------------------------------------------------------------------

    private void OnToggleFullScreenClick(object sender, RoutedEventArgs e)
    {
        SetFullScreen(!IsFullScreen());
    }

    private void OnEscapeInvoked(KeyboardAccelerator sender,
                                 KeyboardAcceleratorInvokedEventArgs args)
    {
        // 只在全屏时响应。窗口模式下按 Esc 什么也不做 —— 否则用户想关掉
        // 某个东西时会莫名进入全屏。
        if (IsFullScreen())
        {
            SetFullScreen(false);
            args.Handled = true;
        }
    }

    private bool IsFullScreen()
    {
        return AppWindow.Presenter.Kind == AppWindowPresenterKind.FullScreen;
    }

    private void SetFullScreen(bool fullScreen)
    {
        AppWindow.SetPresenter(fullScreen ? AppWindowPresenterKind.FullScreen
                                          : AppWindowPresenterKind.Default);
        FullScreenButton.Content = fullScreen ? "退出全屏" : "全屏";

        // 进出全屏时把控件亮出来一次，让用户看到按钮文字已经跟着变了；
        // 之后照旧自动收起。控制条不再单独隐藏 —— 它已经是悬浮的。
        ShowCastingControls();
    }

    private void ExitFullScreenOnCastingEnd()
    {
        // 投屏结束时人可能正停在全屏画面里，而那时投屏面板会被整个收起来 ——
        // 不收全屏的话，用户面对的就是一块没有内容的黑屏，还找不到退出按钮。
        if (IsFullScreen())
        {
            SetFullScreen(false);
        }
    }

    // ---------------------------------------------------------------------
    // 缓冲提示
    //
    // 大码率的片子（4K、B 站的高清源）起播前要拉一大段，那段时间画面是黑的 ——
    // 用户看到的就是「投屏没反应」。缓冲期间把速度摆出来，一眼能看出它在动、
    // 动得多快。
    // ---------------------------------------------------------------------

    private DispatcherTimer? _bufferingTimer;

    /// <summary>上一次 tick 时的下载进度（0..1）。相邻两次之差 × 文件大小 = 速度。</summary>
    private double _lastDownloadProgress;

    /// <summary>媒体文件的总字节数。取不到就退化成只显示「正在缓冲」。</summary>
    private long? _mediaSizeBytes;

    private void StartBufferingWatch(string? mediaUrl)
    {
        _lastDownloadProgress = 0;
        _mediaSizeBytes = null;

        if (mediaUrl != null && mediaUrl.Length > 0)
        {
            // 想要「多少 MB/s」就得知道总大小 —— MediaPlayerElement 只给 0..1 的
            // 进度，不给字节数，所以自己发一个 HEAD 问一下。问不到也不要紧。
            FetchMediaSize(mediaUrl);
        }

        _bufferingTimer ??= new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _bufferingTimer.Tick -= OnBufferingTick;
        _bufferingTimer.Tick += OnBufferingTick;
        // 先停再起：连投两段视频时不会留下上一条的计时。
        _bufferingTimer.Stop();
        _bufferingTimer.Start();
    }

    private void StopBufferingWatch()
    {
        if (_bufferingTimer != null)
        {
            _bufferingTimer.Stop();
        }
        BufferingBadge.Visibility = Visibility.Collapsed;
    }

    private async void FetchMediaSize(string url)
    {
        try
        {
            using HttpClient client = new HttpClient();
            using HttpRequestMessage request = new HttpRequestMessage(HttpMethod.Head, url);
            using HttpResponseMessage response = await client.SendAsync(request);
            _mediaSizeBytes = response.Content.Headers.ContentLength;
        }
        catch (Exception)
        {
            // 有些 CDN 不认 HEAD，或者要 Referer。取不到就退回只显示「正在缓冲」。
            _mediaSizeBytes = null;
        }
    }

    private void OnBufferingTick(object? sender, object e)
    {
        MediaPlayer? player = PlayerElement.MediaPlayer;
        if (player == null)
        {
            BufferingBadge.Visibility = Visibility.Collapsed;
            return;
        }

        MediaPlaybackSession session = player.PlaybackSession;
        // Opening 也算：那是「连上了但还没开始供数据」，在用户眼里同样是黑屏。
        bool buffering = session.PlaybackState == MediaPlaybackState.Buffering
                         || session.PlaybackState == MediaPlaybackState.Opening;

        // 一秒内的进度增量 × 总大小 = 字节/秒。
        double progress = session.DownloadProgress;
        double bytesPerSecond = 0;
        if (_mediaSizeBytes.HasValue && progress > _lastDownloadProgress)
        {
            bytesPerSecond = (progress - _lastDownloadProgress) * _mediaSizeBytes.Value;
        }
        _lastDownloadProgress = progress;

        if (buffering)
        {
            BufferingText.Text = bytesPerSecond > 0
                ? $"正在缓冲　{FormatSpeed(bytesPerSecond)}"
                : "正在缓冲…";
        }
        BufferingBadge.Visibility = buffering ? Visibility.Visible : Visibility.Collapsed;
    }

    /// <summary>
    /// 把字节/秒写成「1.2 MB/s」这种。
    /// 用固定区域格式：中文区域下小数点同样是点，但万一落到用逗号做小数点的
    /// 区域，同一行里「1,2 MB/s」会被读成一千二百。
    /// </summary>
    private static string FormatSpeed(double bytesPerSecond)
    {
        if (bytesPerSecond >= 1024 * 1024)
        {
            return string.Format(CultureInfo.InvariantCulture, "{0:F1} MB/s", bytesPerSecond / 1024 / 1024);
        }
        if (bytesPerSecond >= 1024)
        {
            return string.Format(CultureInfo.InvariantCulture, "{0:F0} KB/s", bytesPerSecond / 1024);
        }
        return string.Format(CultureInfo.InvariantCulture, "{0:F0} B/s", bytesPerSecond);
    }

    // ---------------------------------------------------------------------
    // 悬浮控件
    //
    // 默认收起，鼠标一动才显示，几秒后自己收起来 —— 投屏时用户看的就是画面，
    // 常驻一条控件既挡画面又白占高度。收起不是「消失」：鼠标一动就回来。
    // ---------------------------------------------------------------------

    /// <summary>悬浮控件出现后停留多久自动收起。</summary>
    private static readonly TimeSpan ControlsTimeout = TimeSpan.FromSeconds(4);

    private DispatcherTimer? _controlsTimer;

    /// <summary>唤出悬浮控件，并重新计时。</summary>
    private void ShowCastingControls()
    {
        if (CastingPanel.Visibility != Visibility.Visible)
        {
            return;
        }

        CastingOverlay.Visibility = Visibility.Visible;

        _controlsTimer ??= new DispatcherTimer { Interval = ControlsTimeout };
        _controlsTimer.Tick -= OnControlsTimerTick;
        _controlsTimer.Tick += OnControlsTimerTick;
        // 先停再起：控件正在显示时鼠标又动了一下，倒计时要重新计，
        // 而不是接着原来的走 —— 用户正在看控件，不该正好在这一刻收走。
        _controlsTimer.Stop();
        _controlsTimer.Start();
    }

    private void HideCastingControls()
    {
        if (_controlsTimer != null)
        {
            _controlsTimer.Stop();
        }
        CastingOverlay.Visibility = Visibility.Collapsed;
    }

    private void OnControlsTimerTick(object? sender, object e)
    {
        HideCastingControls();
    }

    private void OnCastingPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        ShowCastingControls();
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

        // 日志窗口自己盯着集合变化并滚动（见 LogWindow），
        // 这里不需要知道有没有窗口开着、开在哪一页。
    }

    // ---------------------------------------------------------------------
    // 日志窗口
    // ---------------------------------------------------------------------

    private void OnShowLogClick(object sender, RoutedEventArgs e)
    {
        if (_logWindow != null)
        {
            // WinUI 的窗口关了就是销毁，不能重新 Activate；所以只有还活着时
            // 才把它提到前面。
            _logWindow.Activate();
            return;
        }

        _logWindow = new LogWindow(_logLines);
        // 引用要在关闭时清掉：不清的话下次点「查看日志」会去 Activate 一个
        // 已经销毁的窗口，什么也不会发生。
        _logWindow.Closed += OnLogWindowClosed;
        _logWindow.Activate();
    }

    private void OnLogWindowClosed(object sender, WindowEventArgs args)
    {
        if (_logWindow != null)
        {
            _logWindow.Closed -= OnLogWindowClosed;
            _logWindow = null;
        }
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        // 先摘事件再销毁引擎，避免销毁过程中的状态回调打到已经在拆的界面上。
        _engine.StateChanged -= OnEngineStateChanged;
        _engine.LogEmitted -= OnEngineLogEmitted;
        _engine.MirrorStarted -= OnMirrorStarted;
        _engine.MirrorFrameReceived -= OnMirrorFrameReceived;
        _engine.Dispose();
    }
}
