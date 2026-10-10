# -*- coding: utf-8 -*-
import io, sys

def patch(path, pairs):
    with io.open(path, encoding='utf-8') as f:
        text = f.read()
    for old, new in pairs:
        n = text.count(old)
        if n != 1:
            sys.stderr.write('%s: anchor count=%d:\n----\n%s\n' % (path, n, old))
            sys.exit(1)
        text = text.replace(old, new)
    io.open(path, 'w', encoding='utf-8').write(text)
    print('ok ' + path)

# ---------------------------------------------------------------- 日志窗限流
patch('app-windows/LogWindow.xaml.cs', [
(
"""    private void OnLinesChanged(object? sender, System.Collections.Specialized.NotifyCollectionChangedEventArgs e)
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
    }""",
"""    /// <summary>
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
    }"""),
(
"""    private readonly ObservableCollection<string> _lines;
    private readonly DispatcherQueue _dispatcher;""",
"""    private readonly ObservableCollection<string> _lines;
    private readonly DispatcherQueue _dispatcher;

    /// <summary>合并刷新的计时器，见 OnLinesChanged。</summary>
    private DispatcherTimer? _refreshTimer;"""),
(
"""        _lines.CollectionChanged -= OnLinesChanged;""",
"""        _lines.CollectionChanged -= OnLinesChanged;
        if (_refreshTimer != null)
        {
            _refreshTimer.Stop();
            _refreshTimer = null;
        }"""),
])

# ---------------------------------------------------------------- 诊断行与画面交接
patch('app-windows/MainWindow.xaml.cs', [
(
"""using System.Runtime.InteropServices;
using System.Threading;""",
"""using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;"""),
(
"""    private WriteableBitmap? _mirrorBitmap;
    private bool _mirrorRenderLogged;""",
"""    private WriteableBitmap? _mirrorBitmap;
    private bool _mirrorRenderLogged;

    // 自解码这条路的两个诊断数：画了多少帧、最近一帧「从核心交下来到画上屏」
    // 花了多久。后者是判断「慢在哪一段」的关键 —— 它小就说明瓶颈在我们上游。
    private long _renderedFrames;
    private double _lastRenderDelayMs;
    private long _frameArrivedTicks;"""),
(
"""            Marshal.Copy(data, _frameBuffer, 0, bytes);
            _frameWidth = width;
            _frameHeight = height;
            _framePending = true;""",
"""            Marshal.Copy(data, _frameBuffer, 0, bytes);
            _frameWidth = width;
            _frameHeight = height;
            _framePending = true;
            _frameArrivedTicks = Stopwatch.GetTimestamp();"""),
(
"""            pixels = _frameBuffer;
            width = _frameWidth;
            height = _frameHeight;
        }""",
"""            pixels = _frameBuffer;
            width = _frameWidth;
            height = _frameHeight;
            arrivedTicks = _frameArrivedTicks;
        }
        _lastRenderDelayMs = (Stopwatch.GetTimestamp() - arrivedTicks) * 1000.0
            / Stopwatch.Frequency;"""),
(
"""        byte[] pixels;
        int width;
        int height;
        lock (_frameLock)
        {
            if (!_framePending)
            {
                return;
            }
            _framePending = false;""",
"""        byte[] pixels;
        int width;
        int height;
        long arrivedTicks;
        lock (_frameLock)
        {
            if (!_framePending)
            {
                return;
            }
            _framePending = false;"""),
(
"""            _mirrorBitmap.Invalidate();

            if (!_mirrorRenderLogged)
            {
                _mirrorRenderLogged = true;
                // 第一帧画出来了才让播放器那条路让开 —— 万一这边有问题，
                // 那条还能兜着。
                MirrorImage.Visibility = Visibility.Visible;
                PlayerElement.Visibility = Visibility.Collapsed;
                StopBufferingWatch();
                AppendLog(AdLogLevel.Info, "镜像：自解码渲染已开始（" + width + "×" + height
                    + "），播放器那条路已让开。");
            }""",
"""            _mirrorBitmap.Invalidate();
            _renderedFrames++;

            if (!_mirrorRenderLogged)
            {
                _mirrorRenderLogged = true;
                // 第一帧画出来了才让播放器那条路让开 —— 万一这边有问题，
                // 那条还能兜着。
                MirrorImage.Visibility = Visibility.Visible;
                PlayerElement.Visibility = Visibility.Collapsed;
                StopBufferingWatch();

                // 播放器与本地流服务一并收掉：留着只是空转（它拉的那条流已经没有
                // 数据了），而且它的状态混进诊断行里会让人误判成「落后 0.5 秒」。
                // 自解码这条路一旦画上第一帧就不会再退回播放器那条 —— 解码器起不
                // 起得来在第一帧就定了，那时还没走到这里。
                _player.Pause();
                if (_liveTs != null)
                {
                    _liveTs.Notice -= OnMirrorNotice;
                    _liveTs.Dispose();
                    _liveTs = null;
                }
                AppendLog(AdLogLevel.Info, "镜像：自解码渲染已开始（" + width + "×" + height
                    + "），播放器与本地流已收掉。");
            }"""),
(
"""        MediaPlaybackSession session = _player.PlaybackSession;
        // 「我们最新送出去的那一帧在流的第几秒」减「播放器正放到第几秒」，
        // 就是画面落后直播多远。
        double lag = server.ElapsedSeconds - session.Position.TotalSeconds;
        int elapsed = (int)(DateTime.Now - _mirrorStartedAt).TotalSeconds;

        AppendLog(AdLogLevel.Info, "镜像诊断（第 " + elapsed + " 秒）：播放器 "
            + session.PlaybackState + "，缓冲 "
            + (session.BufferingProgress * 100).ToString("F0") + "%，位置 "
            + session.Position.TotalSeconds.ToString("F1") + " 秒，已送出到 "
            + server.ElapsedSeconds.ToString("F1") + " 秒（落后 "
            + lag.ToString("F1") + " 秒）；已封 " + server.FramesQueued + " 帧 / "
            + (server.BytesSent / 1024) + " KB，客户端 " + server.ClientCount
            + " 个；伴音 " + _mirrorAudio.State + "，收 "
            + _mirrorAudio.EnqueuedSamples + " 个样本，送出 "
            + _mirrorAudio.DeliveredFrames + " 帧，丢 "
            + _mirrorAudio.DroppedSamples + " 个，峰值 \"""",
"""        int elapsed = (int)(DateTime.Now - _mirrorStartedAt).TotalSeconds;

        // 两条路各报各自的数。以前这里固定报播放器的位置与「已送出到第几秒」，
        // 那是给中转流量用的 —— 自解码这条路根本不用它，于是会报出「落后 0.5 秒」
        // 这种没有意义的数（中转早就收掉了）。哪条在跑就报哪条。
        string video;
        if (_renderedFrames > 0)
        {
            video = "自解码 已画 " + _renderedFrames + " 帧（最近一帧从收到到上屏 "
                + _lastRenderDelayMs.ToString("F1") + " 毫秒）";
        }
        else
        {
            MediaPlaybackSession session = _player.PlaybackSession;
            video = "播放器 " + session.PlaybackState + "，位置 "
                + session.Position.TotalSeconds.ToString("F1") + " 秒";
        }

        AppendLog(AdLogLevel.Info, "镜像诊断（第 " + elapsed + " 秒）：" + video
            + "；伴音 " + _mirrorAudio.State + "，收 "
            + _mirrorAudio.EnqueuedSamples + " 个样本，送出 "
            + _mirrorAudio.DeliveredFrames + " 帧，丢 "
            + _mirrorAudio.DroppedSamples + " 个，峰值 \""""),
(
"""        MirrorImage.Visibility = Visibility.Collapsed;
        PlayerElement.Visibility = Visibility.Visible;
        _mirrorRenderLogged = false;""",
"""        MirrorImage.Visibility = Visibility.Collapsed;
        PlayerElement.Visibility = Visibility.Visible;
        _mirrorRenderLogged = false;
        _renderedFrames = 0;
        _lastRenderDelayMs = 0;"""),
])
