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

    private IntPtr _handle = IntPtr.Zero;
    private bool _disposed;

    // 这些字段的唯一作用是让委托活到引擎销毁为止，不要删。
    private StateChangedCallback? _stateChangedCallback;
    private LogCallback? _logCallback;

    public event Action<AdServiceState>? StateChanged;
    public event Action<AdLogLevel, string>? LogEmitted;

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

        AdCallbacks callbacks = default;
        callbacks.StructSize = (uint)Marshal.SizeOf<AdCallbacks>();
        callbacks.Reserved = 0;
        callbacks.OnStateChanged = Marshal.GetFunctionPointerForDelegate(_stateChangedCallback);
        callbacks.OnLog = Marshal.GetFunctionPointerForDelegate(_logCallback);

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
