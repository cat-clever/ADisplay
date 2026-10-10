// ADisplay —— 把镜像的 H.264 帧封成 MPEG-TS，用本地 HTTP 流式喂给播放器
//
// 为什么走这条路：AirPlay 镜像的帧在我们手里是 **Annex B**（每个 NALU 前是
// 00 00 01 起始码），而 MPEG-TS 要的正是 Annex B —— 一个字节都不用转。
// 更关键的是 **H.264-in-TS 是 Media Foundation 原生支持的组合**（HLS 全是它），
// 于是播放器走的是 DLNA 那条已经验证过的路（CreateFromUri），不再经过
// MediaStreamSource 那层黑箱契约。
//
// 封装上有四处是「看起来能跑、实际会被解复用器丢掉」的坑，都在这里踩过了，
// 改之前先看清注释：
//   1. PSI 段的 section_length 是「段体中段的长度」，不含前面 3 个字节 ——
//      数组大小要按 3 + section_length 开，CRC32 必须占满末尾 4 个字节。
//   2. 连续计数（continuity_counter）**按 PID 各自计数**，不是全局一个。
//      混用一个计数器会在视频 PID 上制造出跳号，被当成丢包。
//   3. 必须有 PCR。PMT 里声明了 PCR_PID 却没有一个包携带 PCR 时，播放器
//      建不起播放时钟，表现就是永远「正在缓冲」。
//   4. PTS 不能回退。发送端的时间戳来自 NTP 同步，中途的同步校正可能让它
//      倒退，倒退的时间戳会让解复用器卡住。

using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace ADisplay.Windows
{
    /// <summary>
    /// 单路 H.264/H.265 的 MPEG-TS 封装 + 把它当无尽 HTTP 流发出去。
    ///
    /// 线程：WriteFrame 由核心的工作线程调用（只入队，不阻塞）；一个专门的发送
    /// 线程负责写 socket。慢客户端会被丢掉，而不是拖住所有人 —— 镜像宁可按最新
    /// 画面走。
    /// </summary>
    internal sealed class LiveTsServer : IDisposable
    {
        // TS 里的几个固定编号。单节目单流，写死即可。
        private const ushort PatPid = 0x0000;
        private const ushort PmtPid = 0x1000;
        private const ushort VideoPid = 0x1001;

        // PMT 里的流类型：0x1B 是 H.264，0x24 是 H.265。发送端选哪个由它决定
        // （我们的 /info 里报了支持 H.265），所以不能写死。
        private const byte StreamTypeH264 = 0x1B;
        private const byte StreamTypeH265 = 0x24;

        private const int TsPacketSize = 188;
        private const ushort ProgramNumber = 1;
        private const byte TransportStreamId = 1;

        /// <summary>PAT/PMT 段的长度（3 字节头 + 段体）。</summary>
        private const int PatSectionLength = 13;
        private const int PmtSectionLength = 18;

        /// <summary>PSI（PAT/PMT）重发的间隔帧数。播放器从中间接入时靠它找到节目。</summary>
        private const int PsiIntervalFrames = 8;

        /// <summary>
        /// PTS / PCR 的起点：1 秒（90 kHz）。
        /// 不从 0 起是因为有些解复用器把 0 当成「没有时间戳」。
        /// </summary>
        private const long PtsBase90k = 90000;

        /// <summary>
        /// PCR 比同一帧的 PTS 早多少：0.2 秒。
        /// 规范要求 PCR 不晚于同一路数据的时间戳，留出提前量最保险。
        /// </summary>
        private const long PcrLead90k = 18000;

        /// <summary>
        /// 待发队列上限（TS 字节块），约 10 秒的量。
        ///
        /// 留这么长不是给慢客户端兜底 —— 那是反的，慢客户端本来该丢。留长是为了
        /// 播放器接得晚的时候，队列里还存着上一个关键帧（入队时按它裁剪，
        /// 见 AcceptLoop）。
        /// </summary>
        private const int MaxQueuedChunks = 300;

        private readonly object _lock = new object();
        private readonly List<NetworkStream> _clients = new List<NetworkStream>();
        private readonly Queue<WaitingChunk> _outgoing = new Queue<WaitingChunk>();

        /// <summary>缓存下来的 SPS/PPS（含起始码），用来给没带参数集的帧补发。</summary>
        private byte[]? _parameterSets;
        private readonly AutoResetEvent _hasData = new AutoResetEvent(false);

        /// <summary>
        /// 同一份 TS 也写一份到临时文件。
        ///
        /// 纯为排障：万一播放器还是不出画面，把这份文件拿给 ffprobe 一跑，
        /// 就能分清是「我们封错了」还是「播放器没吃」。写不进去不影响播放。
        /// </summary>
        private readonly object _dumpLock = new object();
        private FileStream? _dump;
        private long _dumpBytes;
        private const long MaxDumpBytes = 24L * 1024 * 1024;

        private TcpListener? _listener;
        private Thread? _acceptThread;
        private Thread? _sendThread;
        private volatile bool _running;

        // 连续计数按 PID 各算各的。
        private byte _ccPat;
        private byte _ccPmt;
        private byte _ccVideo;

        private byte _streamType = StreamTypeH264;
        private long _bytesSent;
        private int _frameIndex;
        private long _firstPtsUs = -1;
        private long _lastPts90k = -1;
        private bool _noticedClient;
        private int _lastReportedFrames;

        /// <summary>队列里的一个待发块。是不是从关键帧起的要记着 —— 见 AcceptLoop。</summary>
        private sealed class WaitingChunk
        {
            public byte[] Data = new byte[0];
            public bool Keyframe;
        }

        /// <summary>收帧时出问题就说一句；界面上要能看到。</summary>
        public event Action<string>? Notice;

        public int Port { get; private set; }

        /// <summary>已封进 TS 的帧数。给界面上的诊断看。</summary>
        public int FramesQueued
        {
            get { lock (_lock) { return _frameIndex; } }
        }

        /// <summary>真正写进 socket 的字节数。它不涨就说明播放器没在拉。</summary>
        public long BytesSent
        {
            get { lock (_lock) { return _bytesSent; } }
        }

        public int ClientCount
        {
            get { lock (_lock) { return _clients.Count; } }
        }

        public bool Start()
        {
            if (_running)
            {
                return true;
            }

            try
            {
                // 端口交给系统分配：固定端口会跟别的程序撞，而我们要的只是
                // 「本机自己能访问到的一个地址」。
                _listener = new TcpListener(IPAddress.Loopback, 0);
                _listener.Start();
                Port = ((IPEndPoint)_listener.LocalEndpoint).Port;
            }
            catch (Exception error)
            {
                RaiseNotice("镜像中转：本地流服务起不来：" + error.Message);
                return false;
            }

            _running = true;
            _acceptThread = new Thread(AcceptLoop) { IsBackground = true, Name = "adisplay-ts-accept" };
            _acceptThread.Start();
            _sendThread = new Thread(SendLoop) { IsBackground = true, Name = "adisplay-ts-send" };
            _sendThread.Start();

            try
            {
                string path = Path.Combine(Path.GetTempPath(), "adisplay-mirror.ts");
                _dump = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.Read);
                RaiseNotice("镜像中转：同时落一份到 " + path + "（排障用，可忽略）");
            }
            catch (Exception)
            {
                _dump = null;
            }

            RaiseNotice("镜像中转：本地流已就绪 " + Url);
            return true;
        }

        /// <summary>播放器该拿到的地址。</summary>
        public string Url
        {
            get { return "http://127.0.0.1:" + Port + "/live.ts"; }
        }

        /// <summary>
        /// 收一帧（Annex B，核心交过来的原始形态）。只入队，不阻塞调用线程。
        /// </summary>
        public void WriteFrame(byte[] annexB, bool isH265, long ptsUs)
        {
            if (!_running || annexB == null || annexB.Length < 5)
            {
                return;
            }

            byte[] chunk;
            string? notice = null;
            lock (_lock)
            {
                if (_frameIndex == 0)
                {
                    _streamType = isH265 ? StreamTypeH265 : StreamTypeH264;
                }
                _frameIndex++;
                if (_firstPtsUs < 0)
                {
                    _firstPtsUs = ptsUs;
                }

                // 时间戳换成 90 kHz（TS 的规定），并以第一帧为基准 —— 发送端给的是
                // 「开机以来的微秒」，直接用会是个天文数字。
                long relativeUs = ptsUs - _firstPtsUs;
                if (relativeUs < 0)
                {
                    relativeUs = 0;
                }
                long pts90k = PtsBase90k + relativeUs * 9 / 100;
                // 时间戳只许前进：NTP 同步校正会让它倒退，倒退的 PTS 会让
                // 解复用器以为流坏了，然后一直等一个永远不来的时间点。
                if (_lastPts90k >= 0 && pts90k < _lastPts90k)
                {
                    pts90k = _lastPts90k;
                }
                _lastPts90k = pts90k;

                // 这一帧自己带了参数集吗？带了就顺手缓存；没带就用缓存的补上。
                //
                // 解复用器要先看到 SPS/PPS 才能把媒体类型建起来，而它只看接进来
                // 之后的那几个包。镜像是从中间接的（播放器起得比流晚），补发能
                // 保证它一定拿得到 —— 注意这不能替代 I 帧：没有 I 帧照样出不了
                // 画面，所以这只是把「缺参数集」这一个死法去掉。
                bool keyframe = ScanParameterSets(annexB, isH265);
                byte[] payload = annexB;
                if (!keyframe && _parameterSets != null)
                {
                    payload = new byte[_parameterSets.Length + annexB.Length];
                    Array.Copy(_parameterSets, 0, payload, 0, _parameterSets.Length);
                    Array.Copy(annexB, 0, payload, _parameterSets.Length, annexB.Length);
                }

                MemoryStream buffer = new MemoryStream();
                // PSI 定期重发：播放器从中间接入也能找到节目。
                if (_frameIndex == 1 || _frameIndex % PsiIntervalFrames == 0)
                {
                    WritePat(buffer);
                    WritePmt(buffer);
                }
                WritePes(buffer, payload, pts90k);
                chunk = buffer.ToArray();

                _outgoing.Enqueue(new WaitingChunk { Data = chunk, Keyframe = keyframe });
                while (_outgoing.Count > MaxQueuedChunks)
                {
                    _outgoing.Dequeue();
                }

                // 每约 5 秒（≈150 帧）报一次送出去多少：能区分「播放器没来拉」
                // 与「拉了但不显示」—— 后者在这里会看到字节数在涨。
                if (_frameIndex - _lastReportedFrames >= 150)
                {
                    _lastReportedFrames = _frameIndex;
                    notice = "镜像中转：已封 " + _frameIndex + " 帧，"
                        + (_bytesSent / 1024) + " KB，客户端 " + _clients.Count + " 个。";
                }
            }

            // 写转储文件、发通知都不在锁里做：文件 IO 万一卡住，
            // 不能连累正在收帧的核心线程。
            WriteDump(chunk);
            if (notice != null)
            {
                RaiseNotice(notice);
            }
            _hasData.Set();
        }

        public void Stop()
        {
            _running = false;
            lock (_dumpLock)
            {
                if (_dump != null)
                {
                    try
                    {
                        _dump.Dispose();
                    }
                    catch (Exception)
                    {
                    }
                    _dump = null;
                }
            }
            _hasData.Set();

            try
            {
                if (_listener != null)
                {
                    _listener.Stop();
                }
            }
            catch (Exception)
            {
                // 停的时候出什么错都无所谓，下面会把状态清干净。
            }
            _listener = null;

            lock (_lock)
            {
                foreach (NetworkStream client in _clients)
                {
                    try
                    {
                        client.Dispose();
                    }
                    catch (Exception)
                    {
                    }
                }
                _clients.Clear();
                _outgoing.Clear();
                _parameterSets = null;
                _frameIndex = 0;
                _lastReportedFrames = 0;
                _bytesSent = 0;
                _firstPtsUs = -1;
                _lastPts90k = -1;
                _ccPat = 0;
                _ccPmt = 0;
                _ccVideo = 0;
                _noticedClient = false;
            }
        }

        public void Dispose()
        {
            Stop();
        }

        private void WriteDump(byte[] chunk)
        {
            lock (_dumpLock)
            {
                if (_dump == null || _dumpBytes >= MaxDumpBytes)
                {
                    return;
                }
                try
                {
                    _dump.Write(chunk, 0, chunk.Length);
                    _dump.Flush();
                    _dumpBytes += chunk.Length;
                }
                catch (Exception)
                {
                    _dump = null;
                }
            }
        }

        private void AcceptLoop()
        {
            while (_running)
            {
                TcpListener? listener = _listener;
                if (listener == null)
                {
                    return;
                }
                try
                {
                    TcpClient client = listener.AcceptTcpClient();
                    client.NoDelay = true;
                    NetworkStream stream = client.GetStream();
                    if (!Respond(stream))
                    {
                        stream.Dispose();
                        continue;
                    }
                    bool first = false;
                    lock (_lock)
                    {
                        _clients.Add(stream);
                        // 播放器接晚了的话，队列里可能攒着一堆旧帧。从**最新的
                        // 关键帧**开始送：更早的帧缺了参考帧，送过去也解不出来，
                        // 而这个新客户端要是从头追，画面会永远落后好几秒。
                        int lastKeyframe = -1;
                        int position = 0;
                        foreach (WaitingChunk queued in _outgoing)
                        {
                            if (queued.Keyframe)
                            {
                                lastKeyframe = position;
                            }
                            position++;
                        }
                        if (lastKeyframe < 0)
                        {
                            // 一个关键帧都没有，留着也没用。
                            _outgoing.Clear();
                        }
                        else
                        {
                            for (int i = 0; i < lastKeyframe; i++)
                            {
                                _outgoing.Dequeue();
                            }
                        }

                        if (!_noticedClient)
                        {
                            _noticedClient = true;
                            first = true;
                        }
                    }
                    if (first)
                    {
                        RaiseNotice("镜像中转：播放器已连上，开始送流。");
                    }
                }
                catch (Exception)
                {
                    // 监听被 Stop 掉时会抛 —— 正常退出路径。
                    if (!_running)
                    {
                        return;
                    }
                }
            }
        }

        /// <summary>
        /// 回应播放器那一次请求。返回 true 表示「这条连接要一直送流」。
        /// </summary>
        private static bool Respond(NetworkStream stream)
        {
            try
            {
                // 请求要读一下，不能不理：播放器的 HTTP 栈往往会先发一个 HEAD
                // 探一下类型和长度，而 HEAD 是「只问不取」—— 照样回它一整个流，
                // 它的解析就乱了。读到请求头结束就停。
                bool probeOnly = false;
                try
                {
                    stream.ReadTimeout = 2000;
                    byte[] request = new byte[4096];
                    int total = 0;
                    while (total < request.Length)
                    {
                        int read = stream.Read(request, total, request.Length - total);
                        if (read <= 0)
                        {
                            break;
                        }
                        total += read;
                        if (HeadersComplete(request, total))
                        {
                            break;
                        }
                    }
                    probeOnly = IsHeadRequest(request, total);
                }
                catch (Exception)
                {
                    // 读请求超时或者对端还没发，不当作错误：当成 GET 照常送流。
                    // 最坏的结果是比原来多等两秒，而不是少送一路流。
                    probeOnly = false;
                }

                // 刻意**不给 Content-Length** —— 这是一条没有尽头的直播流，
                // 给了长度播放器会在读满之后断开。
                //
                // Content-Type 用 Windows 自己给 .ts 注册的那个
                // （vnd.dlna.mpeg-tts），而不是通用的 video/mp2t：
                // Media Foundation 的 HTTP 字节流要靠 MIME 找到解复用器，
                // 没登记过的类型它是不认的。
                byte[] header = Encoding.ASCII.GetBytes(
                    "HTTP/1.1 200 OK\r\n"
                    + "Content-Type: video/vnd.dlna.mpeg-tts\r\n"
                    + "Cache-Control: no-store\r\n"
                    + "Accept-Ranges: none\r\n"
                    + "Connection: close\r\n"
                    + "\r\n");
                stream.Write(header, 0, header.Length);
                stream.Flush();

                // HEAD：头给完就收工，这条连接不算客户端，也不送流。
                return !probeOnly;
            }
            catch (Exception)
            {
                return false;
            }
        }

        /// <summary>请求头是不是已经读完了（空行）？</summary>
        private static bool HeadersComplete(byte[] buffer, int length)
        {
            for (int i = 3; i < length; i++)
            {
                if (buffer[i - 3] == 0x0D && buffer[i - 2] == 0x0A
                    && buffer[i - 1] == 0x0D && buffer[i] == 0x0A)
                {
                    return true;
                }
            }
            return false;
        }

        /// <summary>这一行是不是 HEAD 请求。</summary>
        private static bool IsHeadRequest(byte[] buffer, int length)
        {
            return length >= 4
                && buffer[0] == (byte)'H' && buffer[1] == (byte)'E'
                && buffer[2] == (byte)'A' && buffer[3] == (byte)'D';
        }

        private void SendLoop()
        {
            while (_running)
            {
                _hasData.WaitOne(200);
                while (true)
                {
                    byte[]? chunk = null;
                    List<NetworkStream>? targets = null;
                    lock (_lock)
                    {
                        if (_outgoing.Count > 0)
                        {
                            chunk = _outgoing.Dequeue().Data;
                            if (_clients.Count > 0)
                            {
                                targets = new List<NetworkStream>(_clients);
                            }
                        }
                    }
                    if (chunk == null)
                    {
                        break;
                    }
                    if (targets == null)
                    {
                        continue;
                    }

                    // 写 socket 放在锁外面：客户端一卡，写调用就会阻塞，
                    // 而收帧线程正等着这把锁 —— 那不成了「播放器卡住、
                    // 连画面都收不进来」吗。
                    List<NetworkStream>? dead = null;
                    foreach (NetworkStream client in targets)
                    {
                        try
                        {
                            client.Write(chunk, 0, chunk.Length);
                            lock (_lock)
                            {
                                _bytesSent += chunk.Length;
                            }
                        }
                        catch (Exception)
                        {
                            if (dead == null)
                            {
                                dead = new List<NetworkStream>();
                            }
                            dead.Add(client);
                        }
                    }

                    if (dead != null)
                    {
                        lock (_lock)
                        {
                            foreach (NetworkStream client in dead)
                            {
                                _clients.Remove(client);
                                try
                                {
                                    client.Dispose();
                                }
                                catch (Exception)
                                {
                                }
                            }
                        }
                    }
                }
            }
        }

        private void RaiseNotice(string message)
        {
            Action<string>? handler = Notice;
            if (handler != null)
            {
                handler(message);
            }
        }

        // ------------------------------------------------------------------
        // 参数集（SPS/PPS）的缓存与补发
        //
        // Annex B 就是一串「起始码 + NAL」，这里只做一件小事：挑出参数集那两个
        // NAL 缓存下来，好在后续帧上补发。
        // ------------------------------------------------------------------

        /// <summary>
        /// 扫这一帧的 NAL，遇到参数集就缓存。
        /// 返回「这一帧自己带了参数集吗」—— 发送端只在关键帧上带。
        ///
        /// 两种编码的 NAL 类型完全不同，不能只按 H.264 认：
        ///   H.264：参数集是 SPS(7)/PPS(8)，切片是 1..5
        ///   H.265：参数集是 VPS(32)/SPS(33)/PPS(34)，切片是 0..31
        /// 只认 H.264 的话，走到 H.265 上一个参数集都找不到，于是既不会缓存，
        /// 也不会把任何一帧标成关键帧 —— 播放器接晚时队列会被整个清掉。
        /// </summary>
        private bool ScanParameterSets(byte[] annexB, bool isH265)
        {
            int firstSetType = isH265 ? 32 : 7;
            int lastSetType = isH265 ? 34 : 8;
            int sliceTypeMax = isH265 ? 31 : 5;

            bool sawSets = false;
            int setsStart = -1;
            int setsEnd = -1;
            int index = 0;
            while (index + 3 < annexB.Length)
            {
                int startCodeLength = StartCodeLengthAt(annexB, index);
                if (startCodeLength == 0)
                {
                    index++;
                    continue;
                }
                int nalStart = index + startCodeLength;
                if (nalStart >= annexB.Length)
                {
                    break;
                }
                // NAL 头的布局两种编码不一样，类型别按同一种位取：
                //   H.264 是 1 个字节，类型在低 5 位
                //   H.265 是 2 个字节，类型在第一个字节的 6 位上
                // 拿 H.264 的位去读 H.265，VPS 的 32 会变成 0 —— 看着像个切片，
                // 扫描当场就停了，参数集一个都认不出来。
                int type = isH265 ? ((annexB[nalStart] >> 1) & 0x3F)
                                  : (annexB[nalStart] & 0x1F);
                if (type <= sliceTypeMax)
                {
                    // 切片已经开始，后面不会再有参数集了。
                    break;
                }
                int nalEnd = NextStartCodeIndex(annexB, nalStart);
                if (nalEnd < 0)
                {
                    nalEnd = annexB.Length;
                }
                if (type == firstSetType)
                {
                    sawSets = true;
                    setsStart = index;
                    setsEnd = nalEnd;
                }
                else if (sawSets && type <= lastSetType)
                {
                    setsEnd = nalEnd;
                }
                index = nalEnd > index ? nalEnd : index + 1;
            }

            if (setsStart >= 0 && setsEnd > setsStart)
            {
                byte[] sets = new byte[setsEnd - setsStart];
                Array.Copy(annexB, setsStart, sets, 0, sets.Length);
                _parameterSets = sets;
                return true;
            }
            return false;
        }

        /// <summary>这一位是起始码吗？是就返回它的长度（3 或 4），不是返回 0。</summary>
        private static int StartCodeLengthAt(byte[] data, int index)
        {
            if (data[index] != 0 || data[index + 1] != 0)
            {
                return 0;
            }
            if (data[index + 2] == 1)
            {
                return 3;
            }
            if (data[index + 2] == 0 && index + 3 < data.Length && data[index + 3] == 1)
            {
                return 4;
            }
            return 0;
        }

        /// <summary>从 from 开始找下一个起始码，找不到返回 -1。</summary>
        private static int NextStartCodeIndex(byte[] data, int from)
        {
            for (int i = from; i + 3 < data.Length; i++)
            {
                if (data[i] == 0 && data[i + 1] == 0
                    && (data[i + 2] == 1 || (data[i + 2] == 0 && data[i + 3] == 1)))
                {
                    return i;
                }
            }
            return -1;
        }

        // ------------------------------------------------------------------
        // TS 封装：结构固定三段 —— PAT（节目在哪）、PMT（节目里有什么）、PES（视频）。
        // 单路 H.264，表里的内容全写死。
        // ------------------------------------------------------------------

        private void WritePat(Stream output)
        {
            // 3 个头字节 + 13 个段体字节：段体 = tsid(2) + 版本(1) + 段号(1)
            // + 末段号(1) + 节目(4) + CRC(4)。CRC 必须占满最后 4 个字节。
            byte[] section = new byte[3 + PatSectionLength];
            section[0] = 0x00;                        // table_id：PAT
            section[1] = 0xB0;                        // 段语法标记 + 保留位
            section[2] = (byte)PatSectionLength;      // section_length = 13
            section[3] = (byte)(TransportStreamId >> 8);
            section[4] = (byte)(TransportStreamId & 0xFF);
            section[5] = 0xC1;                        // 版本 0、current_next 1
            section[6] = 0x00;                        // section_number
            section[7] = 0x00;                        // last_section_number
            section[8] = (byte)(ProgramNumber >> 8);  // 节目号 → PMT 的 PID
            section[9] = (byte)(ProgramNumber & 0xFF);
            section[10] = (byte)(0xE0 | ((PmtPid >> 8) & 0x1F));
            section[11] = (byte)(PmtPid & 0xFF);
            uint crc = Crc32Mpeg(section, 0, section.Length - 4);
            section[12] = (byte)(crc >> 24);
            section[13] = (byte)(crc >> 16);
            section[14] = (byte)(crc >> 8);
            section[15] = (byte)crc;
            WriteSection(output, PatPid, section, ref _ccPat);
        }

        private void WritePmt(Stream output)
        {
            // 3 个头字节 + 18 个段体字节：段体 = 节目号(2) + 版本(1) + 段号(1)
            // + 末段号(1) + PCR_PID(2) + 节目信息长度(2) + 流(5) + CRC(4)。
            byte[] section = new byte[3 + PmtSectionLength];
            section[0] = 0x02;                        // table_id：PMT
            section[1] = 0xB0;
            section[2] = (byte)PmtSectionLength;      // section_length = 18
            section[3] = (byte)(ProgramNumber >> 8);
            section[4] = (byte)(ProgramNumber & 0xFF);
            section[5] = 0xC1;
            section[6] = 0x00;
            section[7] = 0x00;
            section[8] = (byte)(0xE0 | ((VideoPid >> 8) & 0x1F));   // PCR 放在视频 PID 上
            section[9] = (byte)(VideoPid & 0xFF);
            section[10] = 0xF0;                       // program_info_length = 0
            section[11] = 0x00;
            section[12] = _streamType;                // 0x1B = H.264，0x24 = H.265
            section[13] = (byte)(0xE0 | ((VideoPid >> 8) & 0x1F));
            section[14] = (byte)(VideoPid & 0xFF);
            section[15] = 0xF0;                       // ES_info_length = 0
            section[16] = 0x00;
            uint crc = Crc32Mpeg(section, 0, section.Length - 4);
            section[17] = (byte)(crc >> 24);
            section[18] = (byte)(crc >> 16);
            section[19] = (byte)(crc >> 8);
            section[20] = (byte)crc;
            WriteSection(output, PmtPid, section, ref _ccPmt);
        }

        private static void WriteSection(Stream output, ushort pid, byte[] section, ref byte continuity)
        {
            // 我们的表都小于 180 字节，一包装得下，不用分片。
            byte[] packet = new byte[TsPacketSize];
            packet[0] = 0x47;
            packet[1] = (byte)(0x40 | ((pid >> 8) & 0x1F));   // payload_unit_start = 1
            packet[2] = (byte)(pid & 0xFF);
            packet[3] = (byte)(0x10 | (continuity & 0x0F));
            continuity = (byte)((continuity + 1) & 0x0F);
            packet[4] = 0x00;                                  // pointer_field
            Array.Copy(section, 0, packet, 5, section.Length);
            for (int i = 5 + section.Length; i < TsPacketSize; i++)
            {
                packet[i] = 0xFF;                              // TS 的空填充字节
            }
            output.Write(packet, 0, packet.Length);
        }

        private void WritePes(Stream output, byte[] annexB, long pts90k)
        {
            // PES 头：起始码 00 00 01 E0（视频），带 PTS。
            // 只打 PTS（flags = 10）：镜像没有 B 帧重排，DTS 与 PTS 相同。
            //
            // 长度字段算的是「长度字段之后的全部字节」：标记位与 PTS 那 8 个字节，
            // 加上负载。负载里那个 AUD 也算 —— 漏掉它会让解复用器报
            // "PES packet size mismatch"，然后把这些包当成坏包丢掉（0.5.70 里
            // 就是这么写的，短了 6 个字节）。
            const int pesHeaderLength = 14;
            byte[] aud = new byte[] { 0x00, 0x00, 0x00, 0x01, 0x09, 0xF0 };
            int esLength = aud.Length + annexB.Length;
            int packetLength = (pesHeaderLength - 6) + esLength;
            bool unbounded = packetLength > 0xFFFF;

            byte[] pes = new byte[pesHeaderLength];
            pes[0] = 0x00;
            pes[1] = 0x00;
            pes[2] = 0x01;
            pes[3] = 0xE0;
            if (unbounded)
            {
                // 超过 65535 字节的 PES 长度字段写 0，表示「不定长」。镜像的
                // 关键帧（带 SPS/PPS）会超，必须走这一支。
                pes[4] = 0x00;
                pes[5] = 0x00;
            }
            else
            {
                pes[4] = (byte)((packetLength >> 8) & 0xFF);
                pes[5] = (byte)(packetLength & 0xFF);
            }
            pes[6] = 0x80;   // 标记位
            pes[7] = 0x80;   // PTS only
            pes[8] = 5;      // PES_header_data_length
            // PTS 按 TS 规定的位拆成 33 位。
            pes[9] = (byte)(0x21 | (((pts90k >> 30) & 0x07) << 1));
            pes[10] = (byte)((pts90k >> 22) & 0xFF);
            pes[11] = (byte)(0x01 | (((pts90k >> 15) & 0x7F) << 1));
            pes[12] = (byte)((pts90k >> 7) & 0xFF);
            pes[13] = (byte)(0x01 | ((pts90k & 0x7F) << 1));

            // 负载：先插一个访问单元分隔符（AUD，NAL 类型 9），再是这一帧的
            // Annex B —— 分隔符让解复用器能干净地切出「一个访问单元」。
            MemoryStream assembled = new MemoryStream();
            assembled.Write(pes, 0, pes.Length);
            assembled.Write(aud, 0, aud.Length);
            assembled.Write(annexB, 0, annexB.Length);
            byte[] data = assembled.ToArray();

            // 这一帧的 PCR 挂在它的第一个包上：PMT 声明了 PCR 放在视频 PID，
            // 那就必须真有一个包带着它，否则播放器建不起时钟。
            long pcr90k = pts90k - PcrLead90k;
            if (pcr90k < 0)
            {
                pcr90k = 0;
            }

            int offset = 0;
            bool first = true;
            while (offset < data.Length)
            {
                byte[] packet = new byte[TsPacketSize];
                int payloadOffset = FillPacketHeader(packet, VideoPid, first, _ccVideo,
                                                     first ? pcr90k : -1);
                _ccVideo = (byte)((_ccVideo + 1) & 0x0F);

                int capacity = TsPacketSize - payloadOffset;
                int take = Math.Min(capacity, data.Length - offset);
                Array.Copy(data, offset, packet, payloadOffset, take);
                for (int i = payloadOffset + take; i < TsPacketSize; i++)
                {
                    packet[i] = 0xFF;
                }
                output.Write(packet, 0, packet.Length);
                offset += take;
                first = false;
            }
        }

        /// <summary>
        /// 填 TS 包头，返回负载的起始偏移（带 PCR 时是 12，否则是 4）。
        /// </summary>
        private static int FillPacketHeader(byte[] packet, ushort pid, bool payloadStart,
                                            byte continuity, long pcr90k)
        {
            packet[0] = 0x47;
            packet[1] = (byte)((payloadStart ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
            packet[2] = (byte)(pid & 0xFF);

            if (pcr90k >= 0)
            {
                // 自适应字段与负载都有：字段长 7（1 个标志字节 + 6 个 PCR 字节）。
                packet[3] = (byte)(0x30 | (continuity & 0x0F));
                packet[4] = 7;
                packet[5] = 0x10;                  // PCR_flag
                WritePcr(packet, 6, pcr90k);
                return 12;
            }

            packet[3] = (byte)(0x10 | (continuity & 0x0F));
            return 4;
        }

        /// <summary>把 90 kHz 的时钟值写成 6 个字节的 PCR 字段。</summary>
        private static void WritePcr(byte[] packet, int offset, long pcr90k)
        {
            long baseValue = pcr90k & 0x1FFFFFFFFL;      // 33 位
            packet[offset] = (byte)((baseValue >> 25) & 0xFF);
            packet[offset + 1] = (byte)((baseValue >> 17) & 0xFF);
            packet[offset + 2] = (byte)((baseValue >> 9) & 0xFF);
            packet[offset + 3] = (byte)((baseValue >> 1) & 0xFF);
            // 第 5 个字节：base 的最低位 + 6 位保留位（全 1）+ 扩展的最高位。
            packet[offset + 4] = (byte)(((baseValue & 0x01) << 7) | 0x7E);
            packet[offset + 5] = 0x00;                   // 9 位扩展：0
        }

        /// <summary>MPEG-2 的 CRC32（多项式 0x04C11DB7，初值全 1，不做收尾异或）。</summary>
        private static uint Crc32Mpeg(byte[] data, int offset, int length)
        {
            uint crc = 0xFFFFFFFF;
            for (int i = offset; i < offset + length; i++)
            {
                crc ^= (uint)(data[i] << 24);
                for (int bit = 0; bit < 8; bit++)
                {
                    if ((crc & 0x80000000) != 0)
                    {
                        crc = (crc << 1) ^ 0x04C11DB7;
                    }
                    else
                    {
                        crc <<= 1;
                    }
                }
            }
            return crc;
        }
    }
}
