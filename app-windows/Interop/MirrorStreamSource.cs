// ADisplay —— Windows 的镜像渲染：把压缩帧喂进平台管线
//
// 与 macOS 交给 AVSampleBufferDisplayLayer 是同一个思路：核心里不养解码器，
// 压缩帧直接交给平台的解码器。Windows 的入口是 MediaStreamSource ——
// 它把样本喂给 MediaPlayerElement 背后的 Media Foundation 管线，
// 解码、硬解、渲染与显示同步全都是现成的。
//
// 三件必须做对的事，任何一件错了都表现为「界面正常、就是没有画面」：
//
//   1. 样本数据要 AVCC（每个 NALU 前 4 字节长度前缀），而核心给的是 Annex B
//      （起始码分隔）。看着像、完全不兼容。
//   2. 解码器需要编码参数（SPS/PPS），而且必须在建流之前就位 ——
//      所以第一帧到齐之前不建流。好在核心会把参数集挂在第一帧前面。
//   3. 拉取式接口在队列空时不能直接回 null —— 那会被当成「流结束」，
//      画面就此断掉。要用 deferral 把这次请求挂住，等下一帧到了再答复。

using System;
using System.Collections.Generic;
using Windows.Media.Core;
using Windows.Media.MediaProperties;
using Windows.Security.Cryptography;
using Windows.Storage.Streams;

namespace ADisplay.Windows.Interop;

public sealed class MirrorStreamSource
{
    // MF_MT_MPEG_SEQUENCE_HEADER —— Media Foundation 认的「编码器私有数据」属性。
    // H.264 放的是 avcC 记录，正好就是 iOS 自己发过来的那种格式。
    private static readonly Guid MpegSequenceHeader =
        new Guid("05589F81-C356-11CE-BF01-00AA0055595A");

    // 队列上限。镜像宁可按最新画面走，也不要越积越久 —— 积压只会让画面越来越滞后。
    private const int MaxQueuedFrames = 8;

    private readonly object _lock = new object();
    private readonly Queue<byte[]> _pending = new Queue<byte[]>();
    private readonly Queue<bool> _pendingKeyFrame = new Queue<bool>();
    private readonly Queue<TimeSpan> _pendingTimestamp = new Queue<TimeSpan>();

    private MediaStreamSource? _source;
    private bool _isH265;

    // 发送端的时间戳是「开机以来的微秒」，是个很大的数 —— 以第一帧为基准重排到
    // 0 起点，否则管线会以为第一帧要在 27 小时后才播。同时记下上一次的值，
    // 保证严格递增（相等或倒退的样本会被丢掉）。
    private long _firstPtsUs = -1;
    private long _lastTimestampTicks = -1;

    // 等着答复的拉取。队列空的时候挂在这里，下一帧到了再逐个完成。
    //
    // **必须是队列，不能是一个槽位**：媒体管线会同时挂起多个取样请求（视频管线
    // 会预取好几帧）。只有一个槽位的话，第二次请求会把第一次的 deferral 覆盖掉，
    // 那一个就永远不会被完成 —— 管线一直等它。表现是「短暂就绪（Paused 100%）
    // 之后又变回缓冲，然后再也不动」，正是我们看到的那个样子。
    private readonly Queue<MediaStreamSourceSampleRequest> _waitingRequests =
        new Queue<MediaStreamSourceSampleRequest>();
    private readonly Queue<MediaStreamSourceSampleRequestDeferral> _waitingDeferrals =
        new Queue<MediaStreamSourceSampleRequestDeferral>();

    /// <summary>流建好了（编码参数到齐）。界面拿到它就可以设播放源了。</summary>
    public event Action<MediaStreamSource>? Ready;

    /// <summary>
    /// 需要记进日志窗口的一句话。
    /// 这条路上的失败全是静默的，而表现都是同一个「界面正常、就是没有画面」——
    /// 所以每一步的判断依据都要能说出来，否则只能靠猜。
    /// </summary>
    public event Action<string>? Notice;

    // 同一类提示只说一次：逐帧报会把日志淹掉。
    private bool _noticedNoNalu;
    private bool _noticedNoParameterSets;
    private bool _noticedFirstSample;
    private bool _noticedFirstRequest;
    /// <summary>已经成功投递过样本没有。没投递过之前绝不裁剪队列。</summary>
    private bool _deliveredAny;
    /// <summary>头几帧打出来（时间戳 / 关键帧 / 字节数），只打几帧。</summary>
    private int _loggedSamples;

    /// <summary>
    /// 收一帧（Annex B，核心交过来的原始形态）。由核心的工作线程调用。
    /// </summary>
    public void Push(byte[] annexB, bool isH265, uint width, uint height, long ptsUs)
    {
        if (annexB == null || annexB.Length <= 4)
        {
            return;
        }

        List<byte[]> units = SplitAnnexB(annexB);
        if (units.Count == 0)
        {
            RaiseNotice(TakeNoticeOnce(ref _noticedNoNalu,
                "镜像渲染：收到一帧，但里面切不出 NALU。"));
            return;
        }

        bool firstFrame = false;
        bool waitingForParameters = false;
        // 提示消息在锁内只取不报：这个类回调出去会打到界面层，绝不能持着锁调用。
        string? pendingNotice = null;
        lock (_lock)
        {
            if (!_isReadyLocked())
            {
                if (!ParameterSets(units, isH265, out byte[]? vps, out byte[]? sps, out byte[]? pps))
                {
                    // 还没有编码参数，这一帧解不了。核心会给每个关键帧补齐参数集，
                    // 所以正常情况下这里最多等到下一个关键帧。
                    pendingNotice = TakeNoticeOnce(ref _noticedNoParameterSets,
                        "镜像渲染：还没有拿到编码参数（SPS/PPS），等待下一个关键帧。");
                    waitingForParameters = true;
                }
                else
                {
                    _isH265 = isH265;
                    BuildSourceLocked(vps, sps!, pps!, width, height);
                    firstFrame = true;
                }
            }
        }

        RaiseNotice(pendingNotice);
        if (waitingForParameters)
        {
            return;
        }

        if (firstFrame)
        {
            // 通知界面设播放源。建流与设源都在拿锁之后做，避免 Ready 的处理函数
            // 回头调用这里造成死锁。
            Action<MediaStreamSource>? readyHandler = Ready;
            if (readyHandler != null && _source != null)
            {
                readyHandler(_source);
            }
        }

        byte[] avcc = AnnexBToAvcc(units);
        if (avcc.Length == 0)
        {
            return;
        }
        bool keyFrame = ContainsKeyFrame(units, isH265);
        if (keyFrame && !_noticedFirstSample)
        {
            _noticedFirstSample = true;
            RaiseNotice("镜像渲染：已收到首个关键帧，开始向媒体管线送帧。");
        }

        // 攒下要答复的请求，出了锁再送 —— 送的时候会回调进管线，不该持着锁。
        List<MediaStreamSourceSampleRequest>? requests = null;
        List<MediaStreamSourceSampleRequestDeferral>? deferrals = null;
        List<byte[]>? frames = null;
        List<bool>? keyFrames = null;
        List<TimeSpan>? timestamps = null;
        List<string>? notes = null;

        lock (_lock)
        {
            _pending.Enqueue(avcc);
            _pendingKeyFrame.Enqueue(keyFrame);
            _pendingTimestamp.Enqueue(TimestampFrom(ptsUs));
            // 裁剪只在「已经投递过样本」之后做。
            //
            // 镜像的帧是一阵一阵来的，起播时手机往往一口气推好几帧，而管线还没来得及
            // 开口要 —— 这时候按「最旧优先」裁掉，被裁掉的正是**队首那个关键帧**。
            // 解码器没有关键帧就不会开始，而且不报错，表现就是一直缓冲。
            while (_deliveredAny && _pending.Count > MaxQueuedFrames)
            {
                _pending.Dequeue();
                _pendingKeyFrame.Dequeue();
                _pendingTimestamp.Dequeue();
            }

            // 有多少帧就答复多少个挂起的请求（可能不止一个）。
            //
            // 给的是**队首那一帧**（先进先出），并且连它自己的时间戳一起取出来：
            //   * 不能用挂起时记下的时间戳 —— 那一刻还没有帧，值是 0，于是每一帧
            //     都带着 0 送出去，管线看到时间戳不推进就永远停在「打开中」；
            //   * 也不能只把它交给请求却留在队列里 —— 那样这一帧会被送两遍。
            while (_waitingRequests.Count > 0 && _pending.Count > 0)
            {
                if (requests == null)
                {
                    requests = new List<MediaStreamSourceSampleRequest>();
                    deferrals = new List<MediaStreamSourceSampleRequestDeferral>();
                    frames = new List<byte[]>();
                    keyFrames = new List<bool>();
                    timestamps = new List<TimeSpan>();
                    notes = new List<string>();
                }

                requests.Add(_waitingRequests.Dequeue());
                deferrals.Add(_waitingDeferrals.Dequeue());
                frames.Add(_pending.Dequeue());
                keyFrames.Add(_pendingKeyFrame.Dequeue());
                timestamps.Add(_pendingTimestamp.Dequeue());
                notes.Add(string.Empty);
            }

            if (requests != null && frames != null && notes != null)
            {
                for (int i = 0; i < frames.Count && _loggedSamples < 3; i++)
                {
                    _loggedSamples++;
                    notes[i] = "镜像渲染：送出第 " + _loggedSamples + " 帧，时间戳 "
                        + timestamps![i].TotalMilliseconds.ToString("F0") + " ms，"
                        + (keyFrames![i] ? "关键帧" : "非关键帧") + "，"
                        + frames[i].Length + " 字节。";
                }
            }
        }

        if (requests != null && deferrals != null && frames != null && keyFrames != null
            && timestamps != null && notes != null)
        {
            for (int i = 0; i < requests.Count; i++)
            {
                requests[i].Sample = MakeSample(frames[i], keyFrames[i], timestamps[i]);
                deferrals[i].Complete();
            }
            _deliveredAny = true;
            for (int i = 0; i < notes.Count; i++)
            {
                if (notes[i].Length > 0)
                {
                    RaiseNotice(notes[i]);
                }
            }
        }
    }

    public void Dispose()
    {
        List<MediaStreamSourceSampleRequestDeferral>? deferrals = null;
        lock (_lock)
        {
            _pending.Clear();
            _pendingKeyFrame.Clear();
            _pendingTimestamp.Clear();
            if (_waitingDeferrals.Count > 0)
            {
                deferrals = new List<MediaStreamSourceSampleRequestDeferral>(_waitingDeferrals);
                _waitingDeferrals.Clear();
                _waitingRequests.Clear();
            }
            _source = null;
        }
        // 挂着的 deferral 必须完成，否则管线会一直等下去。
        if (deferrals != null)
        {
            foreach (MediaStreamSourceSampleRequestDeferral pending in deferrals)
            {
                pending.Complete();
            }
        }
    }

    // ---------------------------------------------------------------------
    // 建流
    // ---------------------------------------------------------------------

    private bool _isReadyLocked()
    {
        return _source != null;
    }

    // 同类提示只报一次。长跑时每次丢弃都报会把日志窗口刷满，
    // 真正要看的那一行反而被埋掉。
    // 分成「取」与「发」两步，是为了让调用点能在锁内取、锁外发。
    private static string? TakeNoticeOnce(ref bool alreadyRaised, string message)
    {
        if (alreadyRaised)
        {
            return null;
        }
        alreadyRaised = true;
        return message;
    }

    private void RaiseNotice(string? message)
    {
        if (message == null)
        {
            return;
        }
        Action<string>? handler = Notice;
        if (handler != null)
        {
            handler(message);
        }
    }

    private void BuildSourceLocked(byte[]? vps, byte[] sps, byte[] pps, uint width, uint height)
    {
        VideoEncodingProperties properties = _isH265
            ? VideoEncodingProperties.CreateHevc()
            : VideoEncodingProperties.CreateH264();
        // 尺寸用发送端报来的（核心从镜像流的头部读出来的），不自己再解一遍 SPS。
        properties.Width = width;
        properties.Height = height;
        // 编码器私有数据：H.264 放 avcC，H.265 放 hvcC。两者完全不兼容 ——
        // 喂错了解码器一个 NALU 都认不出来，表现同样是「界面正常、没有画面」。
        // 走 H.265 时 vps 一定在（上面 ParameterSets 只在一套齐了时才返回 true）。
        // 编码器私有数据就放这里，这是**唯一**的通道。
        //
        // 别再去找别的写法了：UWP 的 MediaStreamSource 没有
        // VideoStreamDescriptor.MediaHeader，也没有 MediaStreamAttributeKeys
        // （那两个是 Silverlight 那套老 API 的东西，编译器会直接报 CS1061 / CS0246）。
        // 下面这个 Guid 就是 MF 的 MF_MT_MPEG_SEQUENCE_HEADER。
        byte[] codecPrivate = _isH265 ? BuildHvcC(vps!, sps, pps) : BuildAvcC(sps, pps);
        properties.Properties[MpegSequenceHeader] = codecPrivate;
        RaiseNotice("镜像渲染：编码器私有数据 " + codecPrivate.Length + " 字节（"
            + (_isH265 ? "hvcC" : "avcC") + "）。");

        VideoStreamDescriptor descriptor = new VideoStreamDescriptor(properties);
        MediaStreamSource source = new MediaStreamSource(descriptor);
        source.Starting += OnStarting;
        source.SampleRequested += OnSampleRequested;
        _source = source;
    }

    private static void OnStarting(MediaStreamSource sender, MediaStreamSourceStartingEventArgs args)
    {
        args.Request.SetActualStartPosition(TimeSpan.Zero);
    }

    private void OnSampleRequested(MediaStreamSource sender, MediaStreamSourceSampleRequestedEventArgs args)
    {
        // 第一次来要样本就说明格式被管线接受了（否则它连要都不会来要）。
        // 这一行与「已收到首个关键帧」配合起来能直接把故障分成两类：
        // 只有前者 ⇒ 解码器解不出来（多半是系统缺 HEVC 解码器）；
        // 连前者都没有 ⇒ 我们声明的编码格式就不对。
        if (!_noticedFirstRequest)
        {
            _noticedFirstRequest = true;
            RaiseNotice("镜像渲染：媒体管线开始索取样本（编码格式已被接受）。");
        }
        byte[]? frame = null;
        bool keyFrame = false;
        TimeSpan timestamp = TimeSpan.Zero;
        lock (_lock)
        {
            if (_pending.Count > 0)
            {
                frame = _pending.Dequeue();
                keyFrame = _pendingKeyFrame.Dequeue();
                timestamp = _pendingTimestamp.Dequeue();
            }
            else
            {
                // 队列空：把这次请求挂住。直接回 null 会被当成「流结束」，
                // 画面会就此断掉 —— 而镜像的帧本来就是一阵一阵来的。
                // 时间戳等真拿到帧时再说（那时才存在）。挂起的是**队列**，
                // 因为管线可能同时挂好几个（见上面那个字段的说明）。
                _waitingRequests.Enqueue(args.Request);
                _waitingDeferrals.Enqueue(args.Request.GetDeferral());
                return;
            }
        }
        args.Request.Sample = MakeSample(frame, keyFrame, timestamp);
    }

    // 这里刻意不写全限定名：本项目自己的命名空间里有 ADisplay.Windows，
    // 而 C# 解析 `Windows.Media.Core.X` 这种写法时先从当前命名空间找起 ——
    // 它会命中 ADisplay.Windows 然后在里面找 Media，报「ADisplay.Windows 里
    // 没有 Media」。文件顶部的 using 不受影响（那里是从全局命名空间解析的）。
    /// <summary>
    /// 把发送端的时间戳换算成样本时间戳：以第一帧为基准重排到 0 起点，并保证严格递增。
    /// </summary>
    private TimeSpan TimestampFrom(long ptsUs)
    {
        long ticks;
        if (ptsUs > 0)
        {
            if (_firstPtsUs < 0)
            {
                _firstPtsUs = ptsUs;
            }
            // 1 微秒 = 10 个 100ns 的 tick。
            ticks = (ptsUs - _firstPtsUs) * 10;
        }
        else
        {
            // 发送端没给时间戳就按 60fps 兜底推进。
            ticks = _lastTimestampTicks < 0 ? 0 : _lastTimestampTicks + TimeSpan.TicksPerSecond / 60;
        }
        if (_lastTimestampTicks >= 0 && ticks <= _lastTimestampTicks)
        {
            ticks = _lastTimestampTicks + TimeSpan.TicksPerMillisecond;
        }
        _lastTimestampTicks = ticks;
        return TimeSpan.FromTicks(ticks);
    }

    private MediaStreamSample MakeSample(byte[] avcc, bool keyFrame, TimeSpan timestamp)
    {
        IBuffer buffer =
            CryptographicBuffer.CreateFromByteArray(avcc);
        MediaStreamSample sample = MediaStreamSample.CreateFromBuffer(buffer, timestamp);
        // 时长只是个提示，真正的节奏由上面那个时间戳定。
        sample.Duration = TimeSpan.FromMilliseconds(1000.0 / 30.0);
        // 关键帧标记必须准：把每一帧都标成关键帧，解码器会把它们都当随机访问点，
        // 画面会花。
        sample.KeyFrame = keyFrame;
        return sample;
    }

    // ---------------------------------------------------------------------
    // Annex B 解析（与 macOS 那边同一套逻辑）
    // ---------------------------------------------------------------------

    private static List<byte[]> SplitAnnexB(byte[] data)
    {
        // 先记下每个起始码的位置：prefix 是起始码本身的下标，content 是它后面
        // 第一个字节。一段 NALU 的结束就是下一个起始码的 prefix —— 这样 4 字节
        // 起始码前面多余的 0 会被自动排除在外。
        List<int> prefixes = new List<int>();
        List<int> contents = new List<int>();
        int index = 0;
        while (index + 3 <= data.Length)
        {
            if (data[index] == 0 && data[index + 1] == 0)
            {
                if (data[index + 2] == 1)
                {
                    prefixes.Add(index);
                    contents.Add(index + 3);
                    index += 3;
                    continue;
                }
                if (index + 4 <= data.Length && data[index + 2] == 0 && data[index + 3] == 1)
                {
                    prefixes.Add(index);
                    contents.Add(index + 4);
                    index += 4;
                    continue;
                }
            }
            index++;
        }

        List<byte[]> units = new List<byte[]>();
        for (int i = 0; i < contents.Count; i++)
        {
            int start = contents[i];
            int end = i + 1 < prefixes.Count ? prefixes[i + 1] : data.Length;
            while (end > start && data[end - 1] == 0)
            {
                end--;
            }
            if (end <= start)
            {
                continue;
            }
            byte[] unit = new byte[end - start];
            Array.Copy(data, start, unit, 0, unit.Length);
            units.Add(unit);
        }
        return units;
    }

    private static byte[] AnnexBToAvcc(List<byte[]> units)
    {
        int total = 0;
        foreach (byte[] unit in units)
        {
            total += unit.Length + 4;
        }
        byte[] output = new byte[total];
        int offset = 0;
        foreach (byte[] unit in units)
        {
            output[offset] = (byte)((unit.Length >> 24) & 0xFF);
            output[offset + 1] = (byte)((unit.Length >> 16) & 0xFF);
            output[offset + 2] = (byte)((unit.Length >> 8) & 0xFF);
            output[offset + 3] = (byte)(unit.Length & 0xFF);
            offset += 4;
            Array.Copy(unit, 0, output, offset, unit.Length);
            offset += unit.Length;
        }
        return output;
    }

    private static bool ParameterSets(List<byte[]> units, bool isH265,
                                      out byte[]? vps, out byte[]? sps, out byte[]? pps)
    {
        vps = null;
        sps = null;
        pps = null;
        foreach (byte[] unit in units)
        {
            if (unit.Length == 0)
            {
                continue;
            }
            int type = isH265 ? (unit[0] >> 1) & 0x3F : unit[0] & 0x1F;
            if (isH265)
            {
                if (type == 32) { vps = unit; }
                if (type == 33) { sps = unit; }
                if (type == 34) { pps = unit; }
            }
            else
            {
                if (type == 7) { sps = unit; }
                if (type == 8) { pps = unit; }
            }
        }
        // H.265 要三个都齐：hvcC 里 VPS 是必需的，少一个就建不出配置记录。
        if (isH265)
        {
            return vps != null && sps != null && pps != null;
        }
        return sps != null && pps != null;
    }

    private static bool ContainsKeyFrame(List<byte[]> units, bool isH265)
    {
        foreach (byte[] unit in units)
        {
            if (unit.Length == 0)
            {
                continue;
            }
            int type = isH265 ? (unit[0] >> 1) & 0x3F : unit[0] & 0x1F;
            // H.264 的 IDR 是 5；H.265 的 IDR 是 19/20。
            if (isH265 ? (type == 19 || type == 20) : type == 5)
            {
                return true;
            }
        }
        return false;
    }

    /// <summary>按 avcC 的格式拼编码器私有数据。</summary>
    // HEVCDecoderConfigurationRecord（ISO/IEC 14496-15 里的 hvcC）。
    //
    //   0        configurationVersion = 1
    //   1..12    general_profile_space/tier/profile_idc、profile_compatibility_flags、
    //            constraint_indicator_flags、level_idc —— 直接从 SPS 的第 1..12 字节照抄
    //            （SPS 第 0 字节是 NAL 头）
    //   13..14   min_spatial_segmentation_idc（高 4 位保留）
    //   15       parallelismType（高 6 位保留）
    //   16       chromaFormat（高 6 位保留）
    //   17       bitDepthLumaMinus8（高 5 位保留）
    //   18       bitDepthChromaMinus8（高 5 位保留）
    //   19..20   avgFrameRate
    //   21       constantFrameRate / numTemporalLayers / temporalIdNested / 长度前缀 4 字节
    //   22       numOfArrays = 3
    //   之后     每个数组：<类型 1 字节> <该类型 NALU 个数 2 字节>，每个 NALU：<长度 2 字节> <数据>
    private static byte[] BuildHvcC(byte[] vps, byte[] sps, byte[] pps)
    {
        const int headerLength = 23;
        int arraysLength = (5 + vps.Length) + (5 + sps.Length) + (5 + pps.Length);
        byte[] record = new byte[headerLength + arraysLength];
        int offset = 0;

        record[offset++] = 0x01;   // configurationVersion

        // 这一段是照抄 SPS 的头部（profile / 兼容位 / 约束位 / level）。
        //
        // 偏移是 **+2**，不是 +1：HEVC 的 NAL 头是 2 字节（forbidden(1) +
        // type(6) + layerId(6) + temporalId(3)），H.264 才是 1 字节。少算这一格，
        // 整段 profile/level 就整体错位 —— 而 SPS 里连着好几个 0x01，
        // 抄错一格看着「差不多」，很难从肉眼发现，解码器却可能直接拒绝这条流
        // （表现就是播放器一直缓冲）。长度不足时退到 0：宁可解不出来，
        // 也不要写出一段越界的记录。
        for (int i = 0; i < 12; i++)
        {
            int source = i + 2;
            record[offset++] = source < sps.Length ? sps[source] : (byte)0x00;
        }

        record[offset++] = 0xF0;   // min_spatial_segmentation_idc：保留 4 位 + 0
        record[offset++] = 0x00;
        record[offset++] = 0xFC;   // parallelismType：保留 6 位 + 0
        record[offset++] = 0xFD;   // chromaFormat：保留 6 位 + 1（4:2:0）
        record[offset++] = 0xF8;   // bitDepthLumaMinus8：保留 5 位 + 0（8 位）
        record[offset++] = 0xF8;   // bitDepthChromaMinus8：同上
        record[offset++] = 0x00;   // avgFrameRate
        record[offset++] = 0x00;
        // 常量帧率 0、时间层数 1、时域嵌套 1、NALU 长度前缀 4 字节
        record[offset++] = 0x0F;
        record[offset++] = 0x03;   // numOfArrays = 3（VPS / SPS / PPS）

        offset = AppendHvcCArray(record, offset, 32, vps);
        offset = AppendHvcCArray(record, offset, 33, sps);
        offset = AppendHvcCArray(record, offset, 34, pps);
        return record;
    }

    private static int AppendHvcCArray(byte[] record, int offset, int nalType, byte[] data)
    {
        record[offset++] = (byte)(0x80 | (nalType & 0x3F));   // array_completeness = 1
        record[offset++] = 0x00;
        record[offset++] = 0x01;                              // 该类型一个 NALU
        record[offset++] = (byte)((data.Length >> 8) & 0xFF);
        record[offset++] = (byte)(data.Length & 0xFF);
        Array.Copy(data, 0, record, offset, data.Length);
        return offset + data.Length;
    }

    private static byte[] BuildAvcC(byte[] sps, byte[] pps)
    {
        // 01 <profile> <compat> <level> ff e1 <SPS 长度> <SPS> 01 <PPS 长度> <PPS>
        // profile/compat/level 直接取 SPS 的第 1..3 字节（SPS 第 0 字节是 NAL 头）。
        byte profile = sps.Length > 1 ? sps[1] : (byte)0x64;
        byte compat = sps.Length > 2 ? sps[2] : (byte)0x00;
        byte level = sps.Length > 3 ? sps[3] : (byte)0x1F;

        byte[] record = new byte[11 + sps.Length + pps.Length];
        int offset = 0;
        record[offset++] = 0x01;
        record[offset++] = profile;
        record[offset++] = compat;
        record[offset++] = level;
        record[offset++] = 0xFF;   // 保留位全 1 + 长度前缀 4 字节
        record[offset++] = 0xE1;   // 保留位全 1 + 1 个 SPS
        record[offset++] = (byte)((sps.Length >> 8) & 0xFF);
        record[offset++] = (byte)(sps.Length & 0xFF);
        Array.Copy(sps, 0, record, offset, sps.Length);
        offset += sps.Length;
        record[offset++] = 0x01;   // 1 个 PPS
        record[offset++] = (byte)((pps.Length >> 8) & 0xFF);
        record[offset++] = (byte)(pps.Length & 0xFF);
        Array.Copy(pps, 0, record, offset, pps.Length);
        return record;
    }
}
