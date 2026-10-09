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

    // 等着答复的那次拉取。队列空的时候挂在这里，下一帧到了再完成它。
    private MediaStreamSourceSampleRequest? _waitingRequest;
    private MediaStreamSourceSampleRequestDeferral? _waitingDeferral;
    // 答复挂起的那次拉取时，顺手把这一帧的时间戳带出去。
    private TimeSpan _waitingTimestamp = TimeSpan.Zero;

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
            RaiseNoticeOnce(ref _noticedNoNalu, true,
                "镜像渲染：收到一帧，但里面切不出 NALU。");
            return;
        }

        bool firstFrame = false;
        lock (_lock)
        {
            if (!_isReadyLocked())
            {
                if (!ParameterSets(units, isH265, out byte[]? sps, out byte[]? pps))
                {
                    // 还没有编码参数，这一帧解不了。核心会给每个关键帧补齐参数集，
                    // 所以正常情况下这里最多等到下一个关键帧。
                    RaiseNoticeOnce(ref _noticedNoParameterSets, true,
                        "镜像渲染：还没有拿到编码参数（SPS/PPS），等待下一个关键帧。");
                    return;
                }
                _isH265 = isH265;
                BuildSourceLocked(sps!, pps!, width, height);
                firstFrame = true;
            }
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
        if (keyFrame)
        {
            RaiseNoticeOnce(ref _noticedFirstSample, true,
                "镜像渲染：首个关键帧已交给媒体管线。");
        }

        MediaStreamSourceSampleRequest? request = null;
        MediaStreamSourceSampleRequestDeferral? deferral = null;
        lock (_lock)
        {
            _pending.Enqueue(avcc);
            _pendingKeyFrame.Enqueue(keyFrame);
            _pendingTimestamp.Enqueue(TimestampFrom(ptsUs));
            while (_pending.Count > MaxQueuedFrames)
            {
                _pending.Dequeue();
                _pendingKeyFrame.Dequeue();
                _pendingTimestamp.Dequeue();
            }

            // 有人在等就立刻给它，不用等下一次拉取。
            if (_waitingRequest != null && _pending.Count > 0)
            {
                request = _waitingRequest;
                deferral = _waitingDeferral;
                _waitingRequest = null;
                _waitingDeferral = null;
            }
        }

        if (request != null)
        {
            request.Sample = MakeSample(avcc, keyFrame, _waitingTimestamp);
            if (deferral != null)
            {
                deferral.Complete();
            }
        }
    }

    public void Dispose()
    {
        MediaStreamSourceSampleRequestDeferral? deferral;
        lock (_lock)
        {
            _pending.Clear();
            _pendingKeyFrame.Clear();
            _pendingTimestamp.Clear();
            deferral = _waitingDeferral;
            _waitingDeferral = null;
            _waitingRequest = null;
            _source = null;
        }
        // 挂着的 deferral 必须完成，否则管线会一直等下去。
        if (deferral != null)
        {
            deferral.Complete();
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
    private void RaiseNoticeOnce(ref bool alreadyRaised, bool raise, string message)
    {
        if (!raise || alreadyRaised)
        {
            return;
        }
        alreadyRaised = true;
        Action<string>? handler = Notice;
        if (handler != null)
        {
            handler(message);
        }
    }

    private void BuildSourceLocked(byte[] sps, byte[] pps, uint width, uint height)
    {
        VideoEncodingProperties properties = _isH265
            ? VideoEncodingProperties.CreateHevc()
            : VideoEncodingProperties.CreateH264();
        // 尺寸用发送端报来的（核心从镜像流的头部读出来的），不自己再解一遍 SPS。
        properties.Width = width;
        properties.Height = height;
        // 编码器私有数据：avcC 记录。少了它，解码器不知道该怎么解。
        properties.Properties[MpegSequenceHeader] = BuildAvcC(sps, pps);

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
                _waitingRequest = args.Request;
                _waitingDeferral = args.Request.GetDeferral();
                _waitingTimestamp = timestamp;
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
                                      out byte[]? sps, out byte[]? pps)
    {
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
                if (type == 33) { sps = unit; }
                if (type == 34) { pps = unit; }
            }
            else
            {
                if (type == 7) { sps = unit; }
                if (type == 8) { pps = unit; }
            }
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
