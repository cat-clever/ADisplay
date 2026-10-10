// ADisplay —— Windows 的镜像伴音输出
//
// 核心里已经把 AAC-ELD 解成了**交错 float32**（见 core/pipeline/AacEldDecoder），
// 这里只负责送进系统的音频管线。分工与视频那侧一致：核心解码、平台渲染。
//
// 用 AudioGraph + AudioFrameInputNode：它是 WinRT 里为「自己产生 PCM」设计的
// 接口，采样率转换与混音都由图负责，我们只按它要的量喂。
//
// 为什么要用**拉模式**（QuantumStarted 里按需送，而不是收到一帧就推一帧）：
// macOS 那边走了弯路 —— 每秒往引擎里推九十多个 480 帧的小缓冲，真机上多出
// 十几个点的 CPU，改成攒成大块才降下来。AudioFrameInputNode 的接口本身就是
// 拉的（它按音频量子来要），顺着它写就不会有这个问题。

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Windows.Foundation;
using Windows.Media;
using Windows.Media.Audio;
using Windows.Media.MediaProperties;
// AudioRenderCategory 不在 Windows.Media 里，而在 Windows.Media.Render。
using Windows.Media.Render;

namespace ADisplay.Windows.Interop;

public sealed class MirrorAudioPlayer
{
    // 待播样本的上限（按帧算）。落后于画面就该丢，而不是越积越久。
    private const int MaxPendingFrames = 16384;

    private readonly object _lock = new object();
    private readonly List<float> _pending = new List<float>();

    private AudioGraph? _graph;
    private AudioFrameInputNode? _input;
    private int _channels;
    private bool _startFailed;
    private bool _started;

    // 只用于诊断：进来多少、图取走多少。两个数一起看就能分清「核心没送」
    // 与「送了但图没在拉」—— 没有声音这两种原因长得一模一样。
    private long _enqueuedSamples;
    private long _consumedSamples;

    /// <summary>
    /// 转发一条提示。项目约定不用 ?. 空条件运算符，所以显式判一次。
    /// </summary>
    private void RaiseNotice(string message)
    {
        Action<string>? handler = Notice;
        if (handler != null)
        {
            handler(message);
        }
    }

    /// <summary>
    /// 收一帧交错 float32（LRLRLR…）。由核心的工作线程调用 —— 只入队，不做别的。
    /// </summary>
    public void Enqueue(float[] samples, int sampleRate, int channels)
    {
        if (samples.Length == 0 || channels <= 0)
        {
            return;
        }

        bool needStart = false;
        lock (_lock)
        {
            if (_startFailed)
            {
                return;
            }
            if (!_started)
            {
                _started = true;
                needStart = true;
            }
            _enqueuedSamples += samples.Length;
            if ((_pending.Count / channels) < MaxPendingFrames)
            {
                _pending.AddRange(samples);
            }
        }

        if (needStart)
        {
            // 起图是异步的，而这里在核心的工作线程上 —— 起不来只记一笔，不能抛。
            _ = StartAsync(sampleRate, channels);
        }
    }

    private async Task StartAsync(int sampleRate, int channels)
    {
        try
        {
            AudioGraphSettings settings = new AudioGraphSettings(AudioRenderCategory.Media);
            CreateAudioGraphResult created = await AudioGraph.CreateAsync(settings);
            if (created.Status != AudioGraphCreationStatus.Success)
            {
                RaiseNotice("镜像伴音：音频图起不来（" + created.Status + "），这一路没有声音。");
                MarkFailed();
                return;
            }

            AudioGraph graph = created.Graph;
            AudioEncodingProperties format =
                AudioEncodingProperties.CreatePcm((uint)sampleRate, (uint)channels, 32);
            format.Subtype = MediaEncodingSubtypes.Float;

            CreateAudioDeviceOutputNodeResult output = await graph.CreateDeviceOutputNodeAsync();
            if (output.Status != AudioDeviceNodeCreationStatus.Success)
            {
                RaiseNotice("镜像伴音：输出设备打不开（" + output.Status + "），这一路没有声音。");
                graph.Dispose();
                MarkFailed();
                return;
            }

            AudioFrameInputNode input = graph.CreateFrameInputNode(format);
            input.AddOutgoingConnection(output.DeviceOutputNode);
            input.QuantumStarted += OnQuantumStarted;

            lock (_lock)
            {
                _channels = channels;
                _graph = graph;
                _input = input;
            }

            graph.Start();
            RaiseNotice("镜像伴音：已开始播放（" + sampleRate + " Hz / " + channels + " 声道）。");
        }
        catch (Exception ex)
        {
            RaiseNotice("镜像伴音：启动失败 —— " + ex.Message + "，这一路没有声音。");
            MarkFailed();
        }
    }

    /// <summary>音频图按量子来要数据。要多少给多少，不够就补零（补零是静音）。</summary>
    private void OnQuantumStarted(AudioFrameInputNode sender, FrameInputNodeQuantumStartedEventArgs args)
    {
        // RequiredSamples 是**帧**数，不是 float 个数 —— 一个声道一个样本算一帧。
        // 官方文档写得很明确：多声道要乘声道数；他们的示例之所以不乘，是因为
        // 那个节点被显式建成了单声道。我们建的是立体声节点，AudioFrame 的字节数
        // 必须是 帧数 × 声道数 × 4，少算一半图拿到的是残帧，结果就是没有声音。
        int frames = args.RequiredSamples;
        int channels = _channels;
        if (frames <= 0 || channels <= 0)
        {
            return;
        }
        int needed = frames * channels;

        float[] samples = new float[needed];
        lock (_lock)
        {
            int available = _pending.Count;
            int take = available < samples.Length ? available : samples.Length;
            if (take > 0)
            {
                _pending.CopyTo(0, samples, 0, take);
                _pending.RemoveRange(0, take);
            }
            _consumedSamples += take;
            // 余下的保持 0：喂不满会让图停在原地等，补静音比卡住好。
        }

        AudioFrame frame = CreateFrame(samples);
        sender.AddFrame(frame);
    }

    private static unsafe AudioFrame CreateFrame(float[] samples)
    {
        int bytes = samples.Length * sizeof(float);
        AudioFrame frame = new AudioFrame((uint)bytes);
        using (AudioBuffer buffer = frame.LockBuffer(AudioBufferAccessMode.Write))
        using (IMemoryBufferReference reference = buffer.CreateReference())
        {
            ((IMemoryBufferByteAccess)reference).GetBuffer(out byte* target, out uint capacity);
            int copy = bytes <= (int)capacity ? bytes : (int)capacity;
            fixed (float* source = samples)
            {
                Buffer.MemoryCopy(source, target, copy, copy);
            }
        }
        return frame;
    }

    public void Stop()
    {
        AudioGraph? graph;
        AudioFrameInputNode? input;
        lock (_lock)
        {
            graph = _graph;
            input = _input;
            _graph = null;
            _input = null;
            _pending.Clear();
            _started = false;
            _channels = 0;
            _enqueuedSamples = 0;
            _consumedSamples = 0;
        }

        if (input != null)
        {
            input.QuantumStarted -= OnQuantumStarted;
        }
        if (graph != null)
        {
            graph.Stop();
            graph.Dispose();
        }
    }

    private void MarkFailed()
    {
        lock (_lock)
        {
            _startFailed = true;
            _pending.Clear();
        }
    }

    /// <summary>从核心收到的样本数（交错 float 个数）。只用于诊断。</summary>
    public long EnqueuedSamples
    {
        get { lock (_lock) { return _enqueuedSamples; } }
    }

    /// <summary>音频图已经取走的样本数。它不涨就说明图根本没在拉。</summary>
    public long ConsumedSamples
    {
        get { lock (_lock) { return _consumedSamples; } }
    }

    /// <summary>音频图的状态。只有「已运行」才说明这一路真的在播。</summary>
    public string State
    {
        get
        {
            lock (_lock)
            {
                if (_startFailed)
                {
                    return "起不来";
                }
                if (_graph == null)
                {
                    return "未启动";
                }
                return "已运行";
            }
        }
    }

    /// <summary>需要记进日志窗口的一句话。由界面层接上。</summary>
    public event Action<string>? Notice;
}

/// <summary>
/// 拿到 AudioFrame 底层缓冲的规范做法：WinRT 只给 IMemoryBufferReference，
/// 要转成这个 COM 接口才能取到裸指针。GUID 是它的定义。
/// </summary>
[ComImport]
[Guid("5B0D3235-4DBA-4D44-865E-8F1D0E4FD04D")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal unsafe interface IMemoryBufferByteAccess
{
    void GetBuffer(out byte* buffer, out uint capacity);
}
