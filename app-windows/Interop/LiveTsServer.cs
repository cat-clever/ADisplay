// ADisplay —— 把镜像的 H.264 帧封成 MPEG-TS，用本地 HTTP 流式喂给播放器
//
// 为什么走这条路：AirPlay 镜像的帧在我们手里是 **Annex B**（每个 NALU 前是
// 00 00 01 起始码），而 MPEG-TS 要的正是 Annex B —— 一个字节都不用转。
// 更关键的是 **H.264-in-TS 是 Media Foundation 原生支持的组合**（HLS 全是它），
// 于是播放器走的是 DLNA 那条已经验证过的路（CreateFromUri），不再经过
// MediaStreamSource 那层黑箱契约。
//
// 为什么不复用核心那个 MediaRelay：它是「给一个远端 URL、我去拉回来换封装」的
// 拉模型，而镜像是推过来的帧，接不进去。

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
    /// 单路 H.264 的 MPEG-TS 封装 + 把它当无尽 HTTP 流发出去。
    ///
    /// 线程：WriteFrame 由核心的工作线程调用（只入队，不阻塞）；一个专门的发送
    /// 线程负责写 socket。慢客户端会被丢掉，而不是拖住所有人 —— 镜像宁可按最新
    /// 画面走。
    /// </summary>
    internal sealed class LiveTsServer : IDisposable
    {
        // TS 里的几个固定编号。单节目单流，写死即可。
        private const ushort PmtPid = 0x1000;
        private const ushort VideoPid = 0x1001;
        // PMT 里的流类型：0x1B 是 H.264，0x24 是 H.265。发送端选哪个由它决定
        // （我们的 /info 里报了支持 H.265），所以不能写死。
        private const byte StreamTypeH264 = 0x1B;
        private const byte StreamTypeH265 = 0x24;
        private const int TsPacketSize = 188;
        private const int TsPayloadSize = 184;
        private const ushort ProgramNumber = 1;
        private const byte TransportStreamId = 1;

        /// <summary>PSI（PAT/PMT）重发的间隔帧数。播放器从中间接入时靠它找到节目。</summary>
        private const int PsiIntervalFrames = 12;

        /// <summary>待发队列上限（TS 字节块）。满了丢最旧的整块 —— 镜像不追旧帧。</summary>
        private const int MaxQueuedChunks = 32;

        private readonly object _lock = new object();
        private readonly List<NetworkStream> _clients = new List<NetworkStream>();
        private readonly Queue<byte[]> _outgoing = new Queue<byte[]>();
        private readonly AutoResetEvent _hasData = new AutoResetEvent(false);

        /// <summary>
        /// 同一份 TS 也写一份到临时文件。
        ///
        /// 纯为排障：万一播放器还是不出画面，把这份文件拿给 ffprobe 一跑，
        /// 就能分清是「我们封错了」还是「播放器没吃」。写不进去不影响播放。
        /// </summary>
        private FileStream? _dump;
        private long _dumpBytes;
        private const long MaxDumpBytes = 24L * 1024 * 1024;

        private TcpListener? _listener;
        private Thread? _acceptThread;
        private Thread? _sendThread;
        private volatile bool _running;

        private byte _continuityCounter;
        private byte _streamType = StreamTypeH264;
        private string? _pendingNotice;
        private long _bytesSent;
        private int _frameIndex;
        private long _firstPtsUs = -1;
        private bool _noticedClient;
        private int _lastReportedFrames;

        /// <summary>收帧时出问题就说一句；界面上要能看到。</summary>
        public event Action<string>? Notice;

        public int Port { get; private set; }

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
                long pts90k = relativeUs * 9 / 100;
                if (pts90k <= 0)
                {
                    // 第 0 帧的 PTS 不能是 0：有些解复用器把 0 当成「没有时间戳」。
                    pts90k = 1;
                }

                MemoryStream buffer = new MemoryStream();
                // PSI 定期重发：播放器从中间接入也能找到节目。
                if (_frameIndex == 1 || _frameIndex % PsiIntervalFrames == 0)
                {
                    WritePat(buffer);
                    WritePmt(buffer);
                }
                WritePes(buffer, annexB, pts90k);
                chunk = buffer.ToArray();

                // 每约 5 秒（≈150 帧）报一次送出去多少：能区分「播放器没来拉」
                // 与「拉了但不显示」—— 后者在这里会看到字节数在涨。
                if (_frameIndex - _lastReportedFrames >= 150)
                {
                    _lastReportedFrames = _frameIndex;
                    int clients = _clients.Count;
                    long sent = _bytesSent;
                    _pendingNotice = "镜像中转：已封 " + _frameIndex + " 帧，"
                        + (sent / 1024) + " KB，客户端 " + clients + " 个。";
                }

                _outgoing.Enqueue(chunk);
                while (_outgoing.Count > MaxQueuedChunks)
                {
                    _outgoing.Dequeue();
                }

                if (_dump != null && _dumpBytes < MaxDumpBytes)
                {
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
            string? notice = _pendingNotice;
            _pendingNotice = null;
            if (notice != null)
            {
                RaiseNotice(notice);
            }
            _hasData.Set();
        }

        public void Stop()
        {
            _running = false;
            try
            {
                if (_dump != null)
                {
                    _dump.Dispose();
                    _dump = null;
                }
            }
            catch (Exception)
            {
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
                _frameIndex = 0;
                _lastReportedFrames = 0;
                _bytesSent = 0;
                _firstPtsUs = -1;
                _continuityCounter = 0;
                _noticedClient = false;
            }
        }

        public void Dispose()
        {
            Stop();
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
                    lock (_lock)
                    {
                        _clients.Add(stream);
                        if (!_noticedClient)
                        {
                            _noticedClient = true;
                            RaiseNotice("镜像中转：播放器已连上，开始送流。");
                        }
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

        private static bool Respond(NetworkStream stream)
        {
            try
            {
                // 刻意**不给 Content-Length** —— 这是一条没有尽头的直播流，
                // 给了长度播放器会在读满之后断开。
                byte[] header = Encoding.ASCII.GetBytes(
                    "HTTP/1.1 200 OK\r\n"
                    + "Content-Type: video/mp2t\r\n"
                    + "Cache-Control: no-store\r\n"
                    + "Connection: close\r\n"
                    + "\r\n");
                stream.Write(header, 0, header.Length);
                stream.Flush();
                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }

        private void SendLoop()
        {
            while (_running)
            {
                _hasData.WaitOne(200);
                while (true)
                {
                    byte[]? chunk = null;
                    lock (_lock)
                    {
                        if (_outgoing.Count > 0)
                        {
                            chunk = _outgoing.Dequeue();
                        }
                    }
                    if (chunk == null)
                    {
                        break;
                    }

                    List<NetworkStream>? dead = null;
                    lock (_lock)
                    {
                        foreach (NetworkStream client in _clients)
                        {
                            try
                            {
                                client.Write(chunk, 0, chunk.Length);
                                _bytesSent += chunk.Length;
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
        // TS 封装：结构固定三段 —— PAT（节目在哪）、PMT（节目里有什么）、PES（视频）。
        // 单路 H.264，表里的内容全写死。
        // ------------------------------------------------------------------

        private void WritePat(Stream output)
        {
            byte[] section = new byte[13];
            section[0] = 0x00;                        // table_id：PAT
            section[1] = 0xB0;                        // 段语法标记 + 保留位
            section[2] = 0x0D;                        // section_length = 13
            section[3] = (byte)(TransportStreamId >> 8);
            section[4] = (byte)(TransportStreamId & 0xFF);
            section[5] = 0xC1;                        // 版本 0、current_next 1
            section[6] = 0x00;
            section[7] = 0x00;
            section[8] = (byte)(ProgramNumber >> 8);  // 一条节目：节目号 → PMT 的 PID
            section[9] = (byte)(ProgramNumber & 0xFF);
            section[10] = (byte)(0xE0 | ((PmtPid >> 8) & 0x1F));
            section[11] = (byte)(PmtPid & 0xFF);
            ushort crc = Crc32Mpeg(section, 0, 11);
            section[12] = (byte)(crc >> 8);
            section[13 - 1] = (byte)(crc & 0xFF);
            WriteSection(output, 0x00, section);
        }

        private void WritePmt(Stream output)
        {
            byte[] section = new byte[18];
            section[0] = 0x02;                        // table_id：PMT
            section[1] = 0xB0;
            section[2] = 0x12;                        // section_length = 18
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
            ushort crc = Crc32Mpeg(section, 0, 16);
            section[17] = (byte)(crc >> 8);
            section[18 - 1] = (byte)(crc & 0xFF);
            WriteSection(output, PmtPid, section);
        }

        private void WriteSection(Stream output, ushort pid, byte[] section)
        {
            // 我们的表都小于 184 字节，一包装得下，不用分片。
            byte[] packet = NewPacket(pid, 0x40);   // payload_unit_start = 1
            packet[4] = 0x00;                        // pointer_field
            Array.Copy(section, 0, packet, 5, section.Length);
            for (int i = 5 + section.Length; i < TsPacketSize; i++)
            {
                packet[i] = 0xFF;                    // TS 的空填充字节
            }
            output.Write(packet, 0, packet.Length);
        }

        private void WritePes(Stream output, byte[] annexB, long pts90k)
        {
            // PES 头：起始码 00 00 01 E0（视频），带 PTS。
            // 只打 PTS（flags = 10）：镜像没有 B 帧重排，DTS 与 PTS 相同。
            const int headerLength = 14;
            int packetLength = headerLength - 6 + annexB.Length;
            bool unbounded = packetLength > 0xFFFF;

            byte[] pes = new byte[headerLength];
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
            byte[] aud = new byte[] { 0x00, 0x00, 0x00, 0x01, 0x09, 0xF0 };

            MemoryStream assembled = new MemoryStream();
            assembled.Write(pes, 0, pes.Length);
            assembled.Write(aud, 0, aud.Length);
            assembled.Write(annexB, 0, annexB.Length);
            byte[] data = assembled.ToArray();

            int offset = 0;
            bool first = true;
            while (offset < data.Length)
            {
                int take = Math.Min(TsPayloadSize, data.Length - offset);
                byte[] packet = NewPacket(VideoPid, first ? (byte)0x40 : (byte)0x00);
                Array.Copy(data, offset, packet, 4, take);
                for (int i = 4 + take; i < TsPacketSize; i++)
                {
                    packet[i] = 0xFF;
                }
                output.Write(packet, 0, packet.Length);
                offset += take;
                first = false;
            }
        }

        /// <summary>开一个 TS 包：同步字节 + PID/标志 + 连续计数。前 4 字节是包头。</summary>
        private byte[] NewPacket(ushort pid, byte payloadUnitStart)
        {
            byte[] packet = new byte[TsPacketSize];
            packet[0] = 0x47;
            packet[1] = (byte)(payloadUnitStart | ((pid >> 8) & 0x1F));
            packet[2] = (byte)(pid & 0xFF);
            // 有负载、无自适应字段；连续计数按 PID 递增（单路视频，一个计数器够）。
            packet[3] = (byte)(0x10 | (_continuityCounter & 0x0F));
            _continuityCounter = (byte)((_continuityCounter + 1) & 0x0F);
            return packet;
        }

        /// <summary>MPEG-2 的 CRC32（多项式 0x04C11DB7，初值全 1，不做收尾异或）。</summary>
        private static ushort Crc32Mpeg(byte[] data, int offset, int length)
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
            return (ushort)(crc & 0xFFFF);
        }
    }
}
