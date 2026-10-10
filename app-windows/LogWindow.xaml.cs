// ADisplay —— 独立的日志窗口

using System;
using System.Collections.ObjectModel;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Windows.ApplicationModel.DataTransfer;

namespace ADisplay.Windows;

public sealed partial class LogWindow : Window
{
    // 与主窗口共用同一个集合实例，所以这边看到的日志和那边永远是同一份 ——
    // 不需要同步、也不会出现「两处日志不一样」这种最难查的情况。
    private readonly ObservableCollection<string> _lines;
    private readonly DispatcherQueue _dispatcher;

    /// <summary>合并刷新的计时器，见 OnLinesChanged。</summary>
    private DispatcherTimer? _refreshTimer;

    public LogWindow(ObservableCollection<string> lines)
    {
        InitializeComponent();

        _lines = lines;
        _dispatcher = DispatcherQueue.GetForCurrentThread();
        Title = "ADisplay 日志";
        RefreshText();

        // 打开时先滚到底 —— 点「查看日志」的人要看的是最后几行。
        _lines.CollectionChanged += OnLinesChanged;
        Closed += OnWindowClosed;
    }

    /// <summary>
    /// 日志来了。**合并成最多每 250 毫秒刷一次。**
    ///
    /// 每来一行就把 500 行整段重拼一遍、再重设一次 TextBlock 的文本，是 O(n) 的
    /// 界面线程开销 —— 而 AirPlay 那些 NTP/RTP 日志每秒来几十行。日志窗口开着
    /// 的时候，界面线程会被它吃掉相当一块，而镜像画面是同一根线程在画：
    /// 表现就是「投屏变得一卡一卡、画面明显滞后」。250 毫秒刷一次，眼睛看不出
    /// 区别，开销降两个数量级。
    /// </summary>
    private void OnLinesChanged(object? sender, System.Collections.Specialized.NotifyCollectionChangedEventArgs e)
    {
        _refreshTimer ??= CreateRefreshTimer();
        if (!_refreshTimer.IsEnabled)
        {
            _refreshTimer.Start();
        }
    }

    private DispatcherTimer CreateRefreshTimer()
    {
        DispatcherTimer timer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(250),
        };
        timer.Tick += (sender, e) =>
        {
            // 刷一次就停；后面还有日志会再把它启起来，于是自然形成「最多 4 Hz」。
            timer.Stop();
            RefreshText();

            // 刚换完文本时布局还没更新，此刻读 ScrollableHeight 拿到的是旧值，
            // 滚动会差一行。再排一次队，等这一帧构建完再滚。
            _dispatcher.TryEnqueue(() =>
            {
                LogScrollViewer.ChangeView(null, LogScrollViewer.ScrollableHeight, null);
            });
        };
        return timer;
    }

    /// 把整个集合拼成一份文本。每来一条日志重拼一次是 O(n)，但 n 上限是
    /// 500 行 —— 换成增量追加反而要自己维护「被裁掉的行怎么办」，不值得。
    /// 真正省下开销的是上面的合并刷新，不是这个函数本身。
    private void RefreshText()
    {
        LogText.Text = string.Join(Environment.NewLine, _lines);
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        // 窗口关了但集合还活着（主窗口仍在用），所以必须摘掉订阅，
        // 否则这里会一直被每一条新日志叫醒，而且窗口对象也回收不掉。
        _lines.CollectionChanged -= OnLinesChanged;
        if (_refreshTimer != null)
        {
            _refreshTimer.Stop();
            _refreshTimer = null;
        }
    }

    private void OnClearLogClick(object sender, RoutedEventArgs e)
    {
        // 清的是界面上这一份（主窗口与这里共用同一个集合），清空后
        // CollectionChanged 会把文本一起刷新掉。核心写的日志文件不动 ——
        // 那是给事后排查用的。
        _lines.Clear();
    }

    private void OnCopyLogClick(object sender, RoutedEventArgs e)
    {
        if (_lines.Count == 0)
        {
            return;
        }

        DataPackage package = new DataPackage();
        package.SetText(string.Join(Environment.NewLine, _lines));
        Clipboard.SetContent(package);
    }
}
