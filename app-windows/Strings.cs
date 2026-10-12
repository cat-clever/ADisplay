// ADisplay —— 界面文字表（Windows 端）
//
// 与电视端、macOS 同一套思路：自己拿一份表，语言一变就把界面上的文字重新填一遍。
// 用户的三个要求决定了不能走系统那套（.resw + x:Uid）：
//   * 切换语言**不能重启应用** —— 而 XAML 里的 x:Uid 是解析时取值的，重填要么重建
//     整棵树、要么改语言后重开窗口；
//   * 默认跟随系统语言、非英文一律中文；
//   * 界面上要能切换。
// 所以：XAML 里那些写死的字都给元素起名（见 MainWindow.xaml），语言一变由
// ApplyLanguage() 逐个填回去。
//
// 只放**界面上的字**。日志不在这里 —— 日志一律中文，那是给人排查用的（同一个决定）。
// Interop 层那些异常消息（AdEngine.ThrowIfFailed 里那句「切换画质档位」之类）也不在
// 这里：它们是诊断信息，最后都进日志或启动日志。

using System;
using System.IO;
using ADisplay.Windows.Interop;

namespace ADisplay.Windows;

/// <summary>界面语言。只有两种 —— 需求就是中英双语。</summary>
internal enum Language
{
    Zh,
    En,
}

/// <summary>
/// 一份语言对应的全部界面文字。
///
/// 写成一个类加两个常驻实例、每条属性用三元表达式选词，而不是抽象类加两个子类：
/// 这样每条只有一处，漏了或者改名了编译期就报，不会出现「英文那份忘了改」。
/// </summary>
internal sealed class Strings
{
    private readonly bool _en;

    public Language Language { get; }

    private Strings(Language language)
    {
        Language = language;
        _en = language == Language.En;
    }

    public static readonly Strings Chinese = new Strings(Language.Zh);
    public static readonly Strings English = new Strings(Language.En);

    public static Strings Of(Language language)
    {
        return language == Language.En ? English : Chinese;
    }

    // ---- 服务状态 ----
    public string StateStopped => _en ? "Stopped" : "未启动";
    public string StateStarting => _en ? "Starting…" : "正在启动…";
    public string StateRunning => _en ? "Broadcasting — waiting for a phone" : "正在广播，等待手机连接";
    public string StateStreaming => _en ? "Casting" : "正在投屏";
    public string StateStopping => _en ? "Stopping…" : "正在停止…";
    public string StateError => _en ? "Failed to start" : "启动失败";
    public string StateUnknown => _en ? "Unknown state" : "未知状态";

    /// <summary>服务状态的文字。状态码是核心给的，文字在这里选。</summary>
    public string StateText(AdServiceState state)
    {
        switch (state)
        {
            case AdServiceState.Stopped:
                return StateStopped;
            case AdServiceState.Starting:
                return StateStarting;
            case AdServiceState.Running:
                return StateRunning;
            case AdServiceState.Streaming:
                return StateStreaming;
            case AdServiceState.Stopping:
                return StateStopping;
            case AdServiceState.Error:
                return StateError;
            default:
                return StateUnknown;
        }
    }

    // ---- 服务开关 ----
    public string ToggleOn => _en ? "On" : "已开启";
    public string ToggleOff => _en ? "Off" : "已关闭";
    public string ReceiverService => _en ? "Receiver" : "接收服务";

    // ---- 结束投屏后的横幅 ----
    public string BannerCastEnded => _en
        ? "Casting ended, but the phone is still streaming."
        : "投屏已结束，但手机仍在推流。";
    public string BannerMirrorEnded => _en
        ? "Screen mirroring ended, but the phone is still streaming."
        : "屏幕镜像已结束，但手机仍在推流。";
    public string ResumeCasting => _en ? "Resume" : "继续观看";
    public string DisconnectCasting => _en ? "Disconnect" : "断开投屏";

    // ---- 投屏页（悬浮控件）----
    public string MirroringScreen => _en ? "Mirroring screen" : "正在镜像屏幕";
    public string ReceivingCast => _en ? "Receiving cast" : "正在接收投屏";
    public string EndCasting => _en ? "End casting" : "结束投屏";
    public string FullScreen => _en ? "Full screen" : "全屏";
    public string ExitFullScreen => _en ? "Exit full screen" : "退出全屏";
    public string PlaybackHintMouse => _en
        ? "Move the mouse to show the controls"
        : "移动鼠标可再显示控件";
    public string Buffering => _en ? "Buffering…" : "正在缓冲…";

    public string BufferingWithSpeed(string speed)
    {
        return _en ? "Buffering " + speed : "正在缓冲　" + speed;
    }

    // ---- 设置页 ----
    public string DeviceNameLabel => _en ? "Device name" : "设备名称";
    public string DeviceNamePlaceholder => _en ? "e.g. Living Room PC" : "例如：客厅电脑";
    public string Save => _en ? "Save" : "保存";
    public string MirrorQuality => _en ? "Quality" : "画质";
    public string QualitySmooth => _en ? "Smooth" : "流畅";
    public string QualityBalanced => _en ? "Balanced" : "均衡";
    public string QualitySharp => _en ? "Sharp" : "高清";

    public string QualityHint => _en
        ? "Sets how large a screen the phone encodes. Sharp is best for full-screen on a "
          + "desktop; Smooth is for low-end TV boxes. Takes effect the next time the phone connects."
        : "决定手机按多大的分辨率把屏幕编出来，桌面全屏看推荐「高清」。改动在手机下次连上时生效。";

    /// <summary>下拉项的文字：档位名 + 它的分辨率（分辨率与语言无关）。</summary>
    public string QualityItem(AdQualityPreset preset)
    {
        string name;
        switch (preset)
        {
            case AdQualityPreset.Smooth:
                name = QualitySmooth;
                break;
            case AdQualityPreset.Sharp:
                name = QualitySharp;
                break;
            default:
                name = QualityBalanced;
                break;
        }
        string resolution = ResolutionText(preset);
        return _en ? name + " (" + resolution + ")" : name + "（" + resolution + "）";
    }

    /// <summary>
    /// 档位对应的分辨率。与核心 display_suggestion_for_preset 里的一致
    /// （1920x1080 / 2560x1440 / 3840x2160）。数字与语言无关，所以不放进表里。
    /// </summary>
    public static string ResolutionText(AdQualityPreset preset)
    {
        switch (preset)
        {
            case AdQualityPreset.Smooth:
                return "1920×1080";
            case AdQualityPreset.Sharp:
                return "3840×2160";
            default:
                return "2560×1440";
        }
    }

    public string LocalInfo => _en ? "This machine" : "本机信息";
    public string InfoLocalAddress => _en ? "Local address: " : "局域网地址：";
    public string InfoDeviceId => _en ? "Device ID: " : "设备标识：";
    public string InfoCoreVersion => _en ? "Core version: " : "核心版本：";
    public string NoLocalAddress => _en
        ? "(no usable local address detected)"
        : "（未检测到可用的局域网地址）";
    public string AddressSeparator => _en ? ", " : "，";
    public string Unknown => _en ? "unknown" : "未知";

    // ---- 日志 ----
    public string LogLabel => _en ? "Log" : "日志";
    public string LogHint => _en
        ? "The log lives in its own window, so it can stay open while casting."
        : "日志在单独的窗口里显示，投屏时也能一直开着。";
    public string LogWindowTitle => _en ? "ADisplay Log" : "ADisplay 日志";
    public string CopyAll => _en ? "Copy all" : "复制全部";
    public string ClearLog => _en ? "Clear log" : "清除日志";
    public string ViewLog => _en ? "View log" : "查看日志";

    // ---- 界面语言 ----
    public string UiLanguage => _en ? "Language" : "界面语言";

    public string BroadcastLabel => _en ? "Broadcast interfaces" : "广播网卡";

    public string BroadcastAuto => _en
        ? "Automatic (LAN only)"
        : "自动（只在实际的局域网上）";

    public string BroadcastIncludeVirtual => _en
        ? "Include VPN / virtual adapters"
        : "也包含 VPN / 虚拟网卡";

    public string BroadcastHint => _en
        ? "Which adapters device discovery uses. \"Automatic\" also falls back to virtual "
          + "adapters when the machine has no real one (for example only a VPN). Note that "
          + "OpenVPN in tun mode does not forward multicast, so discovery still will not work over it."
        : "决定在哪几块网卡上做设备发现。「自动」在整机没有真实网卡时（例如只连着 VPN）"
          + "也会用虚拟网卡。注意 tun 模式的 OpenVPN 不转发组播，在它上面仍然发现不了设备。";
}

/// <summary>
/// 界面语言存哪、怎么读。
///
/// 存 `%LOCALAPPDATA%\ADisplay\language.txt`，**不用** ApplicationData.Current.LocalSettings：
/// 本应用是绿色版（csproj 里 WindowsPackageType=None），没有包身份，那个 API 在
/// 未打包应用上会直接抛异常。与启动日志（StartupLog）落在同一个目录下，同一套理由。
/// </summary>
internal static class LanguageStore
{
    private const string FileName = "language.txt";

    public static Language Read()
    {
        string? tag = ReadTag();
        if (tag == null)
        {
            return FromSystem();
        }
        return string.Equals(tag, "en", StringComparison.OrdinalIgnoreCase)
            ? Language.En
            : Language.Zh;
    }

    public static void Write(Language language)
    {
        try
        {
            string path = PathFor();
            if (path == null)
            {
                return;
            }
            string? directory = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(directory) && !Directory.Exists(directory))
            {
                Directory.CreateDirectory(directory);
            }
            File.WriteAllText(path, language == Language.En ? "en" : "zh");
        }
        catch (Exception)
        {
            // 存不下只是下次启动回到默认，不值得为此让程序多一个死法
            // （与 StartupLog 写不了日志时的处理一致）。
        }
    }

    /// <summary>
    /// 当前语言对应的那份文字表。
    ///
    /// 名字是 Current 而不是 Strings：方法名与类型名同名的话，类里那些
    /// `Strings.Of(...)` 会解析成这个方法，报 CS0119。
    /// </summary>
    public static Strings Current()
    {
        return Strings.Of(Read());
    }

    private static string? ReadTag()
    {
        try
        {
            string path = PathFor();
            if (path == null || !File.Exists(path))
            {
                return null;
            }
            string text = File.ReadAllText(path).Trim();
            return text.Length == 0 ? null : text;
        }
        catch (Exception)
        {
            return null;
        }
    }

    private static string? PathFor()
    {
        string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrEmpty(localAppData))
        {
            return null;
        }
        return Path.Combine(localAppData, "ADisplay", FileName);
    }

    /// <summary>
    /// 从系统语言推断默认值：**只有明确是英文才用英文，其余一律中文**
    /// （用户 2026-10-11：「默认跟随系统语言，非英文一律按中文」）。
    /// </summary>
    private static Language FromSystem()
    {
        string culture = System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;
        if (culture != null && culture.StartsWith("en", StringComparison.OrdinalIgnoreCase))
        {
            return Language.En;
        }
        return Language.Zh;
    }
}
