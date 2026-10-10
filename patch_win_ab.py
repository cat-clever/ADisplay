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

# ---------------------------------------------------------------- 伴音：挂起语义
patch('app-windows/Interop/MirrorAudioPlayer.cs', [
(
"""    private bool _startFailed;
    private bool _started;""",
"""    private bool _startFailed;
    private bool _started;

    /// <summary>
    /// 挂起：停止播放，并且**不再因为收到新帧而自动重启**。
    ///
    /// 用户点「结束投屏」之后手机还在推流，帧会一直进来。原来的写法里 Stop() 只是
    /// 把音频图拆掉，下一帧的 Enqueue 见 _started == false 就重建一张新图开播 ——
    /// 表现就是「画面关了，声音还在」。这个标志就是那道门禁。
    /// </summary>
    private bool _suspended;"""),
(
"""        bool needStart = false;
        lock (_lock)
        {
            if (_startFailed)
            {
                return;
            }""",
"""        bool needStart = false;
        lock (_lock)
        {
            // 挂起期间连帧都不收：既省掉解码，也彻底堵死「自动复活」这条路。
            if (_suspended || _startFailed)
            {
                return;
            }"""),
(
"""            _pending.Clear();
            _started = false;
            _channels = 0;
            _enqueuedSamples = 0;
            _consumedSamples = 0;
            _droppedSamples = 0;
            _peakRecent = 0;
            _deliveredFrames = 0;
            _deliverFailedLogged = false;""",
"""            _pending.Clear();
            _started = false;
            _channels = 0;
            _enqueuedSamples = 0;
            _consumedSamples = 0;
            _droppedSamples = 0;
            _peakRecent = 0;
            _deliveredFrames = 0;
            _deliverFailedLogged = false;
            // Stop 的语义是「停止且不许自己重启」，要重新出声必须显式 Resume。
            _suspended = true;"""),
(
"""    /// <summary>需要记进日志窗口的一句话。由界面层接上。</summary>
    public event Action<string>? Notice;""",
"""    /// <summary>
    /// 解除挂起：下一次收到帧时重新把音频图建起来。用于「继续观看」。
    /// </summary>
    public void Resume()
    {
        lock (_lock)
        {
            _suspended = false;
        }
    }

    /// <summary>需要记进日志窗口的一句话。由界面层接上。</summary>
    public event Action<string>? Notice;"""),
])

# ---------------------------------------------------------------- 主窗：门禁、恢复、关闭对称
patch('app-windows/MainWindow.xaml.cs', [
(
"""    private WriteableBitmap? _mirrorBitmap;
    private bool _mirrorRenderLogged;""",
"""    private WriteableBitmap? _mirrorBitmap;
    private bool _mirrorRenderLogged;

    /// <summary>
    /// 用户点了「结束投屏」：本地画面收起来了，但发送端可能还在推。
    ///
    /// 它管两件事：投屏页收起之后不再往音频里送帧（不然声音会自己回来），
    /// 以及待机页上那条「还能回去」的横幅要不要显示。
    /// </summary>
    private bool _castingDismissedByUser;

    /// <summary>
    /// 最近一次投屏的地址。DLNA 这条路结束之后想「继续观看」就得靠它 ——
    /// 播放器的 Source 已经被清掉了，没有这个字段就再也起不来。
    /// </summary>
    private string? _castingUrl;

    /// <summary>关窗链是否已经在走。两个窗口会互相触发对方的关闭，用它防绕圈。</summary>
    private bool _closing;"""),
(
"""    private void OnAudioFrameReceived(float[] samples, int sampleRate, int channels)""",
"""    private void OnAudioFrameReceived(float[] samples, int sampleRate, int channels)
    {
        // 用户已经结束投屏：这一帧不该出声。音频播放端自己也有一道挂起门禁，
        // 这里是第二道 —— 两道都留着，因为「声音自己回来」是最难解释的一种坏法。
        if (_castingDismissedByUser)
        {
            return;
        }
        EnqueueAudioFrame(samples, sampleRate, channels);
    }

    private void EnqueueAudioFrame(float[] samples, int sampleRate, int channels)"""),
(
"""        _player.RealTimePlayback = false;
        _player.Source = null;
        // 伴音也停掉：会话结束了不该继续出声。
        _mirrorAudio.Stop();""",
"""        _player.RealTimePlayback = false;
        _player.Source = null;
        // 伴音停掉**并且挂起**：手机还在推流，不挂起的话下一帧就会把声音拉回来。
        _castingDismissedByUser = true;
        _mirrorAudio.Stop();"""),
(
"""        MirrorImage.Visibility = Visibility.Collapsed;
        PlayerElement.Visibility = Visibility.Visible;
        _mirrorRenderLogged = false;
        _renderedFrames = 0;
        _lastRenderDelayMs = 0;""",
"""        MirrorImage.Visibility = Visibility.Collapsed;
        PlayerElement.Visibility = Visibility.Visible;
        // 这里**不复位** _mirrorRenderLogged：帧还在继续画（只是画在隐藏的位图上），
        // 复位会让下一帧再走一次「首帧」分支，把可见性与播放器状态又翻一遍。
        // 它只在真正开始新一次镜像时复位，见 StartMirroring。"""),
(
"""    private void OnLogWindowClosed(object sender, WindowEventArgs args)
    {
        if (_logWindow != null)
        {
            _logWindow.Closed -= OnLogWindowClosed;
            _logWindow = null;
        }
    }""",
"""    private void OnLogWindowClosed(object sender, WindowEventArgs args)
    {
        if (_logWindow != null)
        {
            _logWindow.Closed -= OnLogWindowClosed;
            _logWindow = null;
        }

        // 反过来也成立：关掉日志窗就等于关掉主窗（也就是退出应用）。
        //
        // 用户对这两个窗口的预期是「一体」的。只关掉日志窗、主窗还留着，会让人以为
        // 程序还在跑，而它其实只剩一个空壳；更糟的是那个空壳的关闭按钮还可能被别的
        // 窗口压着点不到。加上这一句之后，无论从哪一边关，收尾都走同一条链。
        CloseMainWindowOnce();
    }

    /// <summary>
    /// 关主窗，但保证整条关闭链只走一次。
    ///
    /// 两个窗口会互相触发对方的关闭（主窗关闭会去关日志窗，日志窗关闭又会回来关
    /// 主窗），没有这道判断就会绕圈。
    /// </summary>
    private void CloseMainWindowOnce()
    {
        if (_closing)
        {
            return;
        }
        try
        {
            Close();
        }
        catch (Exception)
        {
            // 关不掉也不能让异常冒出去 —— 那会停在一个半关闭的状态上。
        }
    }"""),
(
"""    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        // 日志窗口是个独立的 Window，**不会**随主窗口一起消失。""",
"""    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        // 从这里开始就进入收尾，两个窗口之间的互相触发到此为止。
        _closing = true;

        // 日志窗口是个独立的 Window，**不会**随主窗口一起消失。"""),
])

# ---------------------------------------------------------------- 开始新投屏时复位
patch('app-windows/MainWindow.xaml.cs', [
(
"""        _player.RealTimePlayback = true;
        _player.Source = MediaSource.CreateFromUri(new Uri(server.Url));""",
"""        // 新一次镜像：把上一轮结束留下的状态清干净（含伴音的挂起）。
        _castingDismissedByUser = false;
        _mirrorRenderLogged = false;
        _renderedFrames = 0;
        _lastRenderDelayMs = 0;
        _mirrorAudio.Resume();

        _player.RealTimePlayback = true;
        _player.Source = MediaSource.CreateFromUri(new Uri(server.Url));"""),
])
