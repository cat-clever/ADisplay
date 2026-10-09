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

    private void OnLinesChanged(object? sender, System.Collections.Specialized.NotifyCollectionChangedEventArgs e)
    {
        RefreshText();

        // 刚换完文本时布局还没更新，此刻读 ScrollableHeight 拿到的是旧值，
        // 滚动会差一行。再排一次队，等这一帧构建完再滚。
        _dispatcher.TryEnqueue(() =>
        {
            LogScrollViewer.ChangeView(null, LogScrollViewer.ScrollableHeight, null);
        });
    }

    /// 把整个集合拼成一份文本。每来一条日志重拼一次是 O(n)，但 n 上限是
    /// 500 行 —— 换成增量追加反而要自己维护「被裁掉的行怎么办」，不值得。
    private void RefreshText()
    {
        LogText.Text = string.Join(Environment.NewLine, _lines);
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        // 窗口关了但集合还活着（主窗口仍在用），所以必须摘掉订阅，
        // 否则这里会一直被每一条新日志叫醒，而且窗口对象也回收不掉。
        _lines.CollectionChanged -= OnLinesChanged;
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
