// ADisplay —— castcore 的 P/Invoke 声明
//
// 这一层与 include/adisplay/adisplay.h 严格一一对应。改任何一边都必须同步改另一边，
// 结构体的字段顺序、类型宽度、对齐方式都要对上，否则会在运行期读到垃圾数据 ——
// 而且往往不崩溃，只是行为诡异，非常难查。
//
// 注意：C 侧用 __cdecl（AD_CALL），所以这里的 CallingConvention 必须是 Cdecl。
// 默认的 Winapi 在 x64 上恰好也是同一套调用约定，但在 ARM64 上不是，必须显式写。

using System;
using System.Runtime.InteropServices;

namespace ADisplay.Windows.Interop;

/// <summary>返回码，对应 C 的 AdResult。</summary>
internal enum AdResult
{
    Ok = 0,
    InvalidArg = 1,
    NotInitialized = 2,
    AlreadyRunning = 3,
    NotRunning = 4,
    PortInUse = 5,
    PermissionDenied = 6,
    Network = 7,
    Unsupported = 8,
    BufferTooSmall = 9,
    NotFound = 10,
    Internal = 11,
}

/// <summary>接收服务状态，对应 C 的 AdServiceState。</summary>
internal enum AdServiceState
{
    Stopped = 0,
    Starting = 1,
    Running = 2,
    Streaming = 3,
    Stopping = 4,
    Error = 5,
}

/// <summary>画质档位，对应 C 的 AdQualityPreset。</summary>
internal enum AdQualityPreset
{
    Smooth = 0,
    Balanced = 1,
    Sharp = 2,
}

/// <summary>日志级别，对应 C 的 AdLogLevel。</summary>
internal enum AdLogLevel
{
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
}

/// <summary>播放状态，对应 C 的 AdTransportState。取值与 UPnP 的 TransportState 一一对应。</summary>
internal enum AdTransportState
{
    NoMediaPresent = 0,
    Stopped = 1,
    Playing = 2,
    Paused = 3,
    Transitioning = 4,
}

/// <summary>手机发来的播放控制意图，对应 C 的 AdPlaybackCommand。</summary>
internal enum AdPlaybackCommand
{
    Play = 0,
    Pause = 1,
    Stop = 2,
    Seek = 3,        // value 是目标位置（毫秒）
    SetVolume = 4,   // value 是 0..100
    SetMute = 5,     // value 是 0 或 1
}

[StructLayout(LayoutKind.Sequential)]
internal struct AdConfig
{
    public uint StructSize;
    public uint AbiVersion;

    [MarshalAs(UnmanagedType.LPUTF8Str)]
    public string? DeviceName;

    public ushort AirplayPort;
    public ushort DlnaPort;
    public ushort CastpcPort;

    public int EnableAirplay;
    public int EnableDlna;
    public int EnableCastpc;

    public int QualityPreset;
    public int RequireConfirmation;
    public int LogLevel;

    [MarshalAs(UnmanagedType.LPUTF8Str)]
    public string? LogFilePath;

    [MarshalAs(UnmanagedType.LPUTF8Str)]
    public string? ConfigFilePath;

    public IntPtr BindInterfaces;
    public uint BindInterfaceCount;
}

/// <summary>
/// 事件回调。用 IntPtr 而不是委托类型，由调用方保证委托实例在注册期间不被回收 ——
/// 委托被 GC 掉之后再回调就是访问已释放内存，Windows 上通常表现为闪退。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct AdCallbacks
{
    public uint StructSize;
    public uint Reserved;

    public IntPtr OnStateChanged;
    public IntPtr OnConnectRequest;
    public IntPtr OnSessionOpened;
    public IntPtr OnSessionClosed;
    public IntPtr OnVideoFrame;
    public IntPtr OnAudioFrame;
    // 镜像流的压缩视频帧（AirPlay 屏幕镜像）。Windows 端还没接渲染，
    // 但这个字段必须在 —— 这个结构体是手写的布局映射，少一个字段后面
    // 全部错位，而那种错只在运行到回调时才炸，且看不出原因。
    public IntPtr OnMirrorFrame;
    public IntPtr OnMediaUrl;
    public IntPtr OnPlaybackState;
    public IntPtr OnPlaybackCommand;
    public IntPtr OnLog;
    // 镜像伴音的**压缩**帧。这一项必须存在，哪怕 Windows 这边不用它：
    // 它是 C 结构体的**最后一个**字段，少了它尺寸就比核心认的 sizeof(AdCallbacks)
    // 小 8 字节，ad_engine_set_callbacks 会直接返回 InvalidArg（核心的校验是
    // struct_size < sizeof(AdCallbacks)）。这正是 0.5.61 上「注册回调失败
    // （InvalidArg）」的原因 —— 手写布局镜像就是这么漂移的。
    //
    // 留 Zero 是有意的：注册它等于告诉核心「这一帧我自己解」，而 Windows 这条
    // 走的是核心解码后的 PCM（见 OnAudioFrame），不需要压缩帧。
    public IntPtr OnMirrorAudioFrame;
}

/// <summary>
/// 一帧压缩的镜像视频，对应 C 的 AdMirrorFrame。
///
/// 与 AdVideoFrame 分开：那个是「核心解码、界面渲染」那条路用的（交出来的是
/// NV12/I420 平面），而镜像是把压缩帧直接交给平台的解码器
/// （Windows 这边是 MediaStreamSource + MediaPlayerElement）。
///
/// Data 是 AVCC 格式（每个 NALU 前 4 字节长度前缀），已解密。
/// 它只在回调期间有效 —— 托管侧要留存必须自己拷一份。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
/// <summary>
/// 一帧解码后的视频（对应 C 的 AdVideoFrame）。走「核心解码、界面渲染」那条路
/// 的镜像画面就是这个。
///
/// 各平面用固定长度的四个字段写出来，而不是 ByValArray：数组字段在
/// PtrToStructure 下的行为容易踩坑，而这里布局必须和 C 那边逐字节对上 ——
/// 错一个字段后面全部错位，且只在运行到回调时才炸。
/// 目前只用 plane_count = 1、format = BGRA8、data0/linesize0。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct AdVideoFrame
{
    public uint StructSize;
    public uint SessionId;
    public long PtsUs;
    public uint Width;
    public uint Height;
    public int Format;
    public int PlaneCount;
    public IntPtr Data0;
    public IntPtr Data1;
    public IntPtr Data2;
    public IntPtr Data3;
    public int LineSize0;
    public int LineSize1;
    public int LineSize2;
    public int LineSize3;
    public int RotationDegrees;
    public int Reserved;
}

internal struct AdMirrorFrame
{
    public uint StructSize;
    public uint SessionId;
    public IntPtr Data;
    public int Size;
    public int IsH265;
    public uint Width;
    public uint Height;
    public long PtsUs;
    public int Reserved;
}

/// <summary>
/// 一帧解码后的镜像伴音，对应 C 的 AdAudioFrame。
///
/// Data 是**交错** float32（LRLRLR…），已经解码 —— AAC-ELD 在 Windows 的
/// Media Foundation 上不保证支持，所以解码放在核心里做（FFmpeg），平台只播放。
/// 它只在回调期间有效，托管侧要留存必须自己拷一份。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct AdAudioFrame
{
    public uint StructSize;
    public uint SessionId;
    public long PtsUs;
    public uint SampleRate;
    public uint Channels;
    public uint FrameCount;
    public uint Reserved;
    public IntPtr Data;
}

/// <summary>
/// 界面层播放器的当前状态，对应 C 的 AdPlaybackStatus。
///
/// 界面层是播放状态的唯一权威来源：核心不碰播放器，手机的 GetTransportInfo /
/// GetPositionInfo / GetMediaInfo / GetVolume 全部从这份回报取答案。
/// 位置与时长填 -1 表示「还不知道」；volume 与 muted 填 -1 表示「这项没变」——
/// 用 -1 而不是 0，因为 0 是合法值（音量 0、未静音）。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct AdPlaybackStatus
{
    public uint StructSize;
    public uint AbiVersion;

    public uint SessionId;
    public uint Reserved;

    public int TransportState;
    public long PositionMs;
    public long DurationMs;
    public int Volume;
    public int Muted;
}

internal static class AdNative
{
    /// <summary>核心库文件名（不含扩展名），Windows 上是 castcore.dll。</summary>
    private const string Library = "castcore";

    private const CallingConvention Convention = CallingConvention.Cdecl;

    // ---------------------------------------------------------------------
    // 版本与工具
    // ---------------------------------------------------------------------

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern IntPtr ad_version_string();

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern uint ad_abi_version();

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern IntPtr ad_result_string(AdResult result);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern void ad_string_free(IntPtr text);

    // ---------------------------------------------------------------------
    // 生命周期
    // ---------------------------------------------------------------------

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_get_default_config(ref AdConfig config);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_create(ref AdConfig config, out IntPtr engine);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern void ad_engine_destroy(IntPtr engine);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_start(IntPtr engine);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern void ad_engine_stop(IntPtr engine);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_get_state(IntPtr engine, out int state);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_set_callbacks(IntPtr engine, ref AdCallbacks callbacks, IntPtr userData);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_report_playback(IntPtr engine, ref AdPlaybackStatus status);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_get_last_error(
        IntPtr engine, [Out] byte[] buffer, UIntPtr bufferSize, out UIntPtr outLength);

    // ---------------------------------------------------------------------
    // 配置与设备名称
    // ---------------------------------------------------------------------

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_device_name_validate(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? name, [Out] byte[] reason, UIntPtr reasonSize);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_set_device_name(
        IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_get_device_name(
        IntPtr engine, [Out] byte[] buffer, UIntPtr bufferSize, out UIntPtr outLength);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_set_quality_preset(IntPtr engine, int preset);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_get_quality_preset(IntPtr engine, out int preset);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_save_config(IntPtr engine);

    // ---------------------------------------------------------------------
    // 会话
    // ---------------------------------------------------------------------

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_respond_connect_request(
        IntPtr engine, uint requestId, int allow, int remember);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_disconnect_session(IntPtr engine, uint sessionId);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_get_session_count(IntPtr engine, out uint count);

    // ---------------------------------------------------------------------
    // 本地信息
    // ---------------------------------------------------------------------

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_get_local_addresses(
        IntPtr engine, [Out] byte[] buffer, UIntPtr bufferSize, out UIntPtr outLength);

    [DllImport(Library, CallingConvention = Convention)]
    internal static extern AdResult ad_engine_get_device_id(
        IntPtr engine, [Out] byte[] buffer, UIntPtr bufferSize, out UIntPtr outLength);
}
