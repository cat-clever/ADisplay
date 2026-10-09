// ADisplay —— castcore 的托管封装
//
// 把 C ABI 包成 C# 用起来顺手的样子：字符串编解码、错误码转异常/消息、
// 回调委托保活、缓冲区大小协商都在这里处理掉，界面层只跟属性和事件打交道。
//
// 两个必须小心的地方：
//   1. 传给 C 的委托会被 GC 回收。委托实例必须由本对象持有（下面那些 _xxxCallback 字段），
//      否则 GC 之后 C 再回调就是野指针 —— 表现为随机闪退，极难复现。
//   2. 回调在 C 的工作线程上触发。WinUI 的控件只能在 UI 线程碰，
//      所以这里只负责把事件抛出去，由界面层自己 DispatcherQueue 切线程。

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace ADisplay.Windows.Interop;

internal sealed class AdEngine : IDisposable
{
    private const int ErrorBufferSize = 1024;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void StateChangedCallback(IntPtr userData, int state);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void LogCallback(IntPtr userData, int level, IntPtr message);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void MediaUrlCallback(IntPtr userData, uint sessionId, IntPtr url, IntPtr mimeType);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void PlaybackCommandCallback(IntPtr userData, uint sessionId, int command, long value);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void SessionClosedCallback(IntPtr userData, uint sessionId, int reason);
    private delegate void SessionOpenedCallback(IntPtr userData, uint sessionId, IntPtr peer, int streamKind);
    private delegate void MirrorFrameCallback(IntPtr userData, IntPtr frame);

    private IntPtr _handle = IntPtr.Zero;
    private bool _disposed;

    // 与 adisplay.h 的 AdStreamKind 对应。这里只关心镜像这一条 ——
    // 「媒体 URL」那条路走的是 on_media_url，不经过会话回调。
    private const int StreamKindMirrorVideo = 0;

    // 这些字段的唯一作用是让委托活到引擎销毁为止，不要删。
    private StateChangedCallback? _stateChangedCallback;
    private LogCallback? _logCallback;
    private MediaUrlCallback? _mediaUrlCallback;
    private PlaybackCommandCallback? _playbackCommandCallback;
    private SessionClosedCallback? _sessionClosedCallback;
    private SessionOpenedCallback? _sessionOpenedCallback;
    private MirrorFrameCallback? _mirrorFrameCallback;

    public event Action<AdServiceState>? StateChanged;
    public event Action<AdLogLevel, string>? LogEmitted;

    /// <summary>手机推来了一个媒体地址。界面层据此起播 —— 核心不播放。</summary>
    public event Action<uint, string>? MediaUrlReceived;

    /// <summary>手机发来的播放控制意图。command 见 AdPlaybackCommand。</summary>
    public event Action<int, long>? PlaybackCommandReceived;

    /// <summary>这次投屏结束了（手机推来新地址把旧会话抢占，或接收服务停止）。</summary>
    public event Action? CastingEnded;

    /// <summary>iPhone 开始屏幕镜像。界面据此把播放源换成镜像流。</summary>
    public event Action? MirrorStarted;

    /// <summary>
    /// 收到一帧镜像视频。data 是 AVCC 格式的 H.264/H.265，已解密。
    ///
    /// 刻意不在这里切回 UI 线程：帧率是每秒几十帧，每帧跳一次会把 UI 线程压满。
    /// 而 MediaStreamSource 本来就是**拉取式**的 —— 帧先进队列，平台要的时候
    /// 自己来取，所以这条回调只需要把字节拷进队列就返回。
    /// </summary>
    /// width/height 是发送端报来的画面尺寸（核心从镜像流头部读出来的），
    /// 为 0 表示还没收到尺寸信息。ptsUs 是发送端报的显示时间戳，0 表示它没给。
    public event Action<byte[], bool, uint, uint, long>? MirrorFrameReceived;

    public bool IsRunning
    {
        get
        {
            if (_handle == IntPtr.Zero)
            {
                return false;
            }
            AdResult result = AdNative.ad_engine_get_state(_handle, out int state);
            if (result != AdResult.Ok)
            {
                return false;
            }
            return state == (int)AdServiceState.Running || state == (int)AdServiceState.Streaming;
        }
    }

    /// <summary>创建引擎并注册回调。失败时抛 InvalidOperationException，消息可直接显示给用户。</summary>
    public void Create(string deviceName, string logFilePath, string configFilePath)
    {
        if (_handle != IntPtr.Zero)
        {
            throw new InvalidOperationException("引擎已经创建过了");
        }

        AdConfig config = default;
        AdResult result = AdNative.ad_engine_get_default_config(ref config);
        ThrowIfFailed(result, "读取默认配置");

        config.DeviceName = deviceName;
        config.LogFilePath = logFilePath;
        config.ConfigFilePath = configFilePath;
        config.LogLevel = (int)AdLogLevel.Info;

        // 收发组播与监听端口都需要本地网络权限；
        // 文档 2.1 第 5 条要求首次连接弹窗确认，这里保持开启。
        config.RequireConfirmation = 1;

        result = AdNative.ad_engine_create(ref config, out _handle);
        ThrowIfFailed(result, "创建引擎");

        RegisterCallbacks();
    }

    private void RegisterCallbacks()
    {
        _stateChangedCallback = OnStateChangedFromCore;
        _logCallback = OnLogFromCore;
        _mediaUrlCallback = OnMediaUrlFromCore;
        _playbackCommandCallback = OnPlaybackCommandFromCore;
        _sessionClosedCallback = OnSessionClosedFromCore;
        _sessionOpenedCallback = OnSessionOpenedFromCore;
        _mirrorFrameCallback = OnMirrorFrameFromCore;

        AdCallbacks callbacks = default;
        callbacks.StructSize = (uint)Marshal.SizeOf<AdCallbacks>();
        callbacks.Reserved = 0;
        callbacks.OnStateChanged = Marshal.GetFunctionPointerForDelegate(_stateChangedCallback);
        callbacks.OnLog = Marshal.GetFunctionPointerForDelegate(_logCallback);
        callbacks.OnMediaUrl = Marshal.GetFunctionPointerForDelegate(_mediaUrlCallback);
        callbacks.OnPlaybackCommand = Marshal.GetFunctionPointerForDelegate(_playbackCommandCallback);
        callbacks.OnSessionClosed = Marshal.GetFunctionPointerForDelegate(_sessionClosedCallback);
        callbacks.OnSessionOpened = Marshal.GetFunctionPointerForDelegate(_sessionOpenedCallback);
        callbacks.OnMirrorFrame = Marshal.GetFunctionPointerForDelegate(_mirrorFrameCallback);

        AdResult result = AdNative.ad_engine_set_callbacks(_handle, ref callbacks, IntPtr.Zero);
        ThrowIfFailed(result, "注册回调");
    }

    private void OnStateChangedFromCore(IntPtr userData, int state)
    {
        // 先拷到局部变量再判空：回调在工作线程上触发，
        // 直接读字段的话，判空与调用之间订阅者可能已被摘掉。
        Action<AdServiceState>? handler = StateChanged;
        if (handler != null)
        {
            handler((AdServiceState)state);
        }
    }

    private void OnLogFromCore(IntPtr userData, int level, IntPtr message)
    {
        string text = Marshal.PtrToStringUTF8(message) ?? string.Empty;

        Action<AdLogLevel, string>? handler = LogEmitted;
        if (handler != null)
        {
            handler((AdLogLevel)level, text);
        }
    }

    private void OnMediaUrlFromCore(IntPtr userData, uint sessionId, IntPtr url, IntPtr mimeType)
    {
        string text = Marshal.PtrToStringUTF8(url) ?? string.Empty;
        Action<uint, string>? handler = MediaUrlReceived;
        if (handler != null)
        {
            handler(sessionId, text);
        }
    }

    private void OnPlaybackCommandFromCore(IntPtr userData, uint sessionId, int command, long value)
    {
        Action<int, long>? handler = PlaybackCommandReceived;
        if (handler != null)
        {
            handler(command, value);
        }
    }

    private void OnSessionOpenedFromCore(IntPtr userData, uint sessionId, IntPtr peer, int streamKind)
    {
        // 只有镜像会话要走渲染面；「媒体 URL」那条路是 on_media_url 的事。
        if (streamKind != StreamKindMirrorVideo)
        {
            return;
        }
        Action? handler = MirrorStarted;
        if (handler != null)
        {
            handler();
        }
    }

    private void OnMirrorFrameFromCore(IntPtr userData, IntPtr frame)
    {
        if (frame == IntPtr.Zero)
        {
            return;
        }

        Action<byte[], bool, uint, uint, long>? handler = MirrorFrameReceived;
        if (handler == null)
        {
            return;
        }

        // 指针只在本次回调期间有效，必须拷走 —— 而拷贝在这里做（核心的工作线程上）
        // 而不是在 UI 线程上做，是为了不让每帧都过一次线程调度。
        AdMirrorFrame value = Marshal.PtrToStructure<AdMirrorFrame>(frame);
        if (value.Data == IntPtr.Zero || value.Size <= 0)
        {
            return;
        }
        byte[] copy = new byte[value.Size];
        Marshal.Copy(value.Data, copy, 0, value.Size);
        handler(copy, value.IsH265 != 0, value.Width, value.Height, value.PtsUs);
    }

    private void OnSessionClosedFromCore(IntPtr userData, uint sessionId, int reason)
    {
        Action? handler = CastingEnded;
        if (handler != null)
        {
            handler();
        }
    }

    /// <summary>
    /// 把播放器的真实状态回报给核心。不回报的话手机的 Get*Info 永远停在旧值，
    /// 进度条不动、音量滑块还会弹回去。
    ///
    /// transportState 每次都给真实值即可 —— 核心只在它变化时才给手机推事件。
    /// volume / muted 传 -1 表示「这项没变」。
    /// </summary>
    public void ReportPlayback(uint sessionId, int transportState, long positionMs,
                               long durationMs, int volume, int muted)
    {
        if (_handle == IntPtr.Zero)
        {
            return;
        }

        AdPlaybackStatus status = default;
        status.StructSize = (uint)Marshal.SizeOf<AdPlaybackStatus>();
        status.AbiVersion = AdNative.ad_abi_version();
        status.SessionId = sessionId;
        status.TransportState = transportState;
        status.PositionMs = positionMs;
        status.DurationMs = durationMs;
        status.Volume = volume;
        status.Muted = muted;

        AdNative.ad_engine_report_playback(_handle, ref status);
    }

    public void Start()
    {
        EnsureCreated();
        AdResult result = AdNative.ad_engine_start(_handle);
        if (result != AdResult.Ok)
        {
            throw new InvalidOperationException(DescribeFailure(result, "启动接收服务"));
        }
    }

    public void Stop()
    {
        if (_handle == IntPtr.Zero)
        {
            return;
        }
        AdNative.ad_engine_stop(_handle);
    }

    public string DeviceName
    {
        get => ReadString((byte[] buffer, UIntPtr size, out UIntPtr length) =>
            AdNative.ad_engine_get_device_name(_handle, buffer, size, out length));
        set
        {
            EnsureCreated();
            AdResult result = AdNative.ad_engine_set_device_name(_handle, value);
            if (result == AdResult.InvalidArg)
            {
                throw new InvalidOperationException(ReadDeviceNameRejection(value));
            }
            ThrowIfFailed(result, "修改设备名称");
        }
    }

    public string DeviceId =>
        ReadString((byte[] buffer, UIntPtr size, out UIntPtr length) =>
            AdNative.ad_engine_get_device_id(_handle, buffer, size, out length));

    /// <summary>本机在局域网中的地址，换行分隔。</summary>
    public string LocalAddresses =>
        ReadString((byte[] buffer, UIntPtr size, out UIntPtr length) =>
            AdNative.ad_engine_get_local_addresses(_handle, buffer, size, out length));

    public void SetQualityPreset(AdQualityPreset preset)
    {
        EnsureCreated();
        ThrowIfFailed(AdNative.ad_engine_set_quality_preset(_handle, (int)preset), "切换画质档位");
    }

    public void SaveConfig()
    {
        if (_handle == IntPtr.Zero)
        {
            return;
        }
        AdNative.ad_engine_save_config(_handle);
    }

    /// <summary>
    /// 校验设备名称是否合规。合规返回 null，不合规返回可以直接显示给用户的原因。
    /// 先校验再提交，能让界面在用户输入时就给出提示，而不是等点了保存才报错。
    /// </summary>
    public static string? ValidateDeviceName(string name)
    {
        byte[] reason = new byte[ErrorBufferSize];
        AdResult result = AdNative.ad_device_name_validate(name, reason, (UIntPtr)reason.Length);
        if (result == AdResult.Ok)
        {
            return null;
        }
        string text = DecodeUtf8(reason);
        return string.IsNullOrEmpty(text) ? "设备名称不合规" : text;
    }

    private string ReadDeviceNameRejection(string attempted)
    {
        string? reason = ValidateDeviceName(attempted);
        return reason ?? "设备名称不合规";
    }

    // ---------------------------------------------------------------------
    // 缓冲区协商
    //
    // C 侧约定：缓冲区不够时返回 BufferTooSmall 并把所需字节数填进 outLength。
    // 所以先用小缓冲探一次，再按需要的尺寸重来。
    // ---------------------------------------------------------------------

    private delegate AdResult BufferReader(byte[] buffer, UIntPtr bufferSize, out UIntPtr outLength);

    private static string ReadString(BufferReader reader)
    {
        byte[] probe = new byte[256];
        AdResult result = reader(probe, (UIntPtr)probe.Length, out UIntPtr needed);

        if (result == AdResult.Ok)
        {
            return DecodeUtf8(probe);
        }
        if (result != AdResult.BufferTooSmall)
        {
            return string.Empty;
        }

        ulong required = needed.ToUInt64();
        if (required == 0 || required > 1024 * 1024)
        {
            return string.Empty;   // 尺寸离谱，判定为异常，避免无节制分配
        }

        byte[] buffer = new byte[required];
        result = reader(buffer, (UIntPtr)buffer.Length, out _);
        return result == AdResult.Ok ? DecodeUtf8(buffer) : string.Empty;
    }

    private static string DecodeUtf8(byte[] buffer)
    {
        int length = Array.IndexOf(buffer, (byte)0);
        if (length < 0)
        {
            length = buffer.Length;
        }
        return length == 0 ? string.Empty : Encoding.UTF8.GetString(buffer, 0, length);
    }

    private string DescribeFailure(AdResult result, string action)
    {
        string? detail = ReadLastError();
        if (!string.IsNullOrEmpty(detail))
        {
            return $"{action}失败：{detail}";
        }
        IntPtr text = AdNative.ad_result_string(result);
        string description = Marshal.PtrToStringAnsi(text) ?? result.ToString();
        return $"{action}失败：{description}";
    }

    private string? ReadLastError()
    {
        byte[] probe = new byte[ErrorBufferSize];
        AdResult result = AdNative.ad_engine_get_last_error(
            _handle, probe, (UIntPtr)probe.Length, out UIntPtr needed);

        if (result == AdResult.Ok)
        {
            string text = DecodeUtf8(probe);
            return string.IsNullOrEmpty(text) ? null : text;
        }
        if (result != AdResult.BufferTooSmall)
        {
            return null;
        }

        ulong required = needed.ToUInt64();
        if (required == 0 || required > 1024 * 1024)
        {
            return null;
        }
        byte[] buffer = new byte[required];
        result = AdNative.ad_engine_get_last_error(_handle, buffer, (UIntPtr)buffer.Length, out _);
        return result == AdResult.Ok ? DecodeUtf8(buffer) : null;
    }

    private static void ThrowIfFailed(AdResult result, string action)
    {
        if (result != AdResult.Ok)
        {
            throw new InvalidOperationException($"{action}失败（{result}）");
        }
    }

    private void EnsureCreated()
    {
        if (_handle == IntPtr.Zero)
        {
            throw new InvalidOperationException("引擎尚未创建");
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;

        if (_handle != IntPtr.Zero)
        {
            AdNative.ad_engine_destroy(_handle);
            _handle = IntPtr.Zero;
        }

        // 引擎销毁后 C 侧不会再回调，这时才能安全放手让委托被回收。
        _stateChangedCallback = null;
        _logCallback = null;
    }
}
