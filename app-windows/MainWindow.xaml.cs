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
using ADisplay.Windows.Interop;

namespace ADisplay.Windows;

public sealed partial class MainWindow : Window
{
    // 日志只留最近这些行。长时间挂着接收服务时，无限增长会拖慢界面。
    private const int MaxLogLines = 500;

    private readonly AdEngine _engine = new();
    private readonly DispatcherQueue _dispatcher;
    private readonly ObservableCollection<string> _logLines = new();

    // 程序化改动 ToggleSwitch.IsOn 时会再次触发 Toggled，
    // 用它避免在失败回滚时递归调用 Start/Stop。
    private bool _suppressToggle;

    public MainWindow()
    {
        InitializeComponent();

        _dispatcher = DispatcherQueue.GetForCurrentThread();
        LogList.ItemsSource = _logLines;

        _engine.StateChanged += OnEngineStateChanged;
        _engine.LogEmitted += OnEngineLogEmitted;

        Closed += OnWindowClosed;

        InitializeEngine();
    }

    private void InitializeEngine()
    {
        try
        {
            _engine.Create(
                deviceName: string.Empty,   // 空表示由核心取主机名作为默认设备名
                logFilePath: string.Empty,  // 空表示用平台默认日志路径
                configFilePath: string.Empty);

            DeviceNameBox.Text = _engine.DeviceName;
            DeviceIdRun.Text = _engine.DeviceId;
            AddressRun.Text = FormatAddresses(_engine.LocalAddresses);
            VersionRun.Text = ReadVersion();

            AppendLog(AdLogLevel.Info, "ADisplay 已就绪。打开开关即可开始接收投屏。");
        }
        catch (Exception ex)
        {
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
            try
            {
                _engine.Start();
            }
            catch (Exception ex)
            {
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
        });
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
