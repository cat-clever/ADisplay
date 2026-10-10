# -*- coding: utf-8 -*-
# 本机验证：把 LiveTsServer.cs 的字节布局逐字节复刻一遍，喂合成帧，
# 再用 ffprobe 检查产物。新版与旧版（0.5.70）都复刻，好做对照。
import io
import subprocess
import sys

TS = 188
PAT_PID = 0x0000
PMT_PID = 0x1000
VID_PID = 0x1001
PROGRAM = 1
TSID = 1

# 日志里那台 iPhone 真实交上来的参数集，ffprobe 能解析出分辨率，
# 用它才能一并验证「参数集有没有原样带着走」。
SPS = bytes([0x27, 0x64, 0x00, 0x1F, 0xAC, 0x13, 0x14, 0x50, 0x20, 0x02,
             0x27, 0x88, 0x96, 0x6E, 0x02, 0x1A, 0x02, 0x04])
PPS = bytes([0x28, 0xEE, 0x3C, 0xB0])
START = bytes([0x00, 0x00, 0x00, 0x01])


def crc32_mpeg(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= (byte << 24) & 0xFFFFFFFF
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc & 0xFFFFFFFF


class Muxer(object):
    """新版：分段长度正确、每个 PID 各自的连续计数、带 PCR、PTS 只许前进。"""

    def __init__(self):
        self.cc = {PAT_PID: 0, PMT_PID: 0, VID_PID: 0}
        self.first_pts_us = None
        self.last_pts90k = -1

    def _header(self, pid, start, pcr90k):
        b = bytearray(TS)
        b[0] = 0x47
        b[1] = (0x40 if start else 0x00) | ((pid >> 8) & 0x1F)
        b[2] = pid & 0xFF
        if pcr90k is not None and pcr90k >= 0:
            b[3] = 0x30 | (self.cc[pid] & 0x0F)
            b[4] = 7
            b[5] = 0x10
            base = pcr90k & 0x1FFFFFFFF
            b[6] = (base >> 25) & 0xFF
            b[7] = (base >> 17) & 0xFF
            b[8] = (base >> 9) & 0xFF
            b[9] = (base >> 1) & 0xFF
            b[10] = ((base & 0x01) << 7) | 0x7E
            b[11] = 0x00
            off = 12
        else:
            b[3] = 0x10 | (self.cc[pid] & 0x0F)
            off = 4
        self.cc[pid] = (self.cc[pid] + 1) & 0x0F
        return b, off

    def _section(self, pid, section):
        b, off = self._header(pid, True, None)
        b[off] = 0x00
        b[off + 1:off + 1 + len(section)] = section
        return bytes(b[:off + 1 + len(section)]) + b"\xff" * (TS - off - 1 - len(section)) \
            if False else self._pad(b, off + 1 + len(section))

    @staticmethod
    def _pad(packet, used):
        out = bytearray(packet)
        for i in range(used, TS):
            out[i] = 0xFF
        return bytes(out)

    def pat(self):
        s = bytearray(3 + 13)
        s[0] = 0x00
        s[1] = 0xB0
        s[2] = 13
        s[3] = (TSID >> 8) & 0xFF
        s[4] = TSID & 0xFF
        s[5] = 0xC1
        s[6] = 0
        s[7] = 0
        s[8] = (PROGRAM >> 8) & 0xFF
        s[9] = PROGRAM & 0xFF
        s[10] = 0xE0 | ((PMT_PID >> 8) & 0x1F)
        s[11] = PMT_PID & 0xFF
        crc = crc32_mpeg(bytes(s[:12]))
        s[12] = (crc >> 24) & 0xFF
        s[13] = (crc >> 16) & 0xFF
        s[14] = (crc >> 8) & 0xFF
        s[15] = crc & 0xFF
        return self._section(PAT_PID, bytes(s))

    def pmt(self, stream_type):
        s = bytearray(3 + 18)
        s[0] = 0x02
        s[1] = 0xB0
        s[2] = 18
        s[3] = (PROGRAM >> 8) & 0xFF
        s[4] = PROGRAM & 0xFF
        s[5] = 0xC1
        s[6] = 0
        s[7] = 0
        s[8] = 0xE0 | ((VID_PID >> 8) & 0x1F)
        s[9] = VID_PID & 0xFF
        s[10] = 0xF0
        s[11] = 0x00
        s[12] = stream_type
        s[13] = 0xE0 | ((VID_PID >> 8) & 0x1F)
        s[14] = VID_PID & 0xFF
        s[15] = 0xF0
        s[16] = 0x00
        crc = crc32_mpeg(bytes(s[:17]))
        s[17] = (crc >> 24) & 0xFF
        s[18] = (crc >> 16) & 0xFF
        s[19] = (crc >> 8) & 0xFF
        s[20] = crc & 0xFF
        return self._section(PMT_PID, bytes(s))

    def frame(self, index, annexb, pts_us, stream_type):
        if self.first_pts_us is None:
            self.first_pts_us = pts_us
        rel = pts_us - self.first_pts_us
        if rel < 0:
            rel = 0
        pts = 90000 + rel * 9 // 100
        if self.last_pts90k >= 0 and pts < self.last_pts90k:
            pts = self.last_pts90k
        self.last_pts90k = pts

        out = bytearray()
        if index == 0 or index % 8 == 0:
            out += self.pat()
            out += self.pmt(stream_type)

        pes = bytearray(14)
        pes[0] = 0
        pes[1] = 0
        pes[2] = 1
        pes[3] = 0xE0
        length = 8 + 6 + len(annexb)
        if length > 0xFFFF:
            pes[4] = 0
            pes[5] = 0
        else:
            pes[4] = (length >> 8) & 0xFF
            pes[5] = length & 0xFF
        pes[6] = 0x80
        pes[7] = 0x80
        pes[8] = 5
        pes[9] = 0x21 | (((pts >> 30) & 0x07) << 1)
        pes[10] = (pts >> 22) & 0xFF
        pes[11] = 0x01 | (((pts >> 15) & 0x7F) << 1)
        pes[12] = (pts >> 7) & 0xFF
        pes[13] = 0x01 | ((pts & 0x7F) << 1)
        data = bytes(pes) + bytes([0, 0, 0, 1, 0x09, 0xF0]) + annexb
        assert len(data) - 6 == length, (len(data) - 6, length)

        offset = 0
        first = True
        pcr = pts - 18000
        if pcr < 0:
            pcr = 0
        while offset < len(data):
            b, off = self._header(VID_PID, first, pcr if first else -1)
            cap = TS - off
            take = min(cap, len(data) - offset)
            b[off:off + take] = data[offset:offset + take]
            out += self._pad(b, off + take)
            offset += take
            first = False
        return bytes(out)


class OldMuxer(Muxer):
    """0.5.70 的老版本，只把有问题的三处按原样复刻：段长度、全局连续计数、无 PCR。"""

    def __init__(self):
        Muxer.__init__(self)
        self.global_cc = 0
        self.last_pts90k = -1

    def _header(self, pid, start, pcr90k):
        b = bytearray(TS)
        b[0] = 0x47
        b[1] = (0x40 if start else 0x00) | ((pid >> 8) & 0x1F)
        b[2] = pid & 0xFF
        b[3] = 0x10 | (self.global_cc & 0x0F)
        self.global_cc = (self.global_cc + 1) & 0x0F
        return b, 4

    def pat(self):
        s = bytearray(13)          # 老版本：数组按 section_length 开，少了 3 个字节
        s[0] = 0x00
        s[1] = 0xB0
        s[2] = 13
        s[3] = (TSID >> 8) & 0xFF
        s[4] = TSID & 0xFF
        s[5] = 0xC1
        s[6] = 0
        s[7] = 0
        s[8] = (PROGRAM >> 8) & 0xFF
        s[9] = PROGRAM & 0xFF
        s[10] = 0xE0 | ((PMT_PID >> 8) & 0x1F)
        s[11] = PMT_PID & 0xFF
        crc = crc32_mpeg(bytes(s[:11]))
        s[12] = (crc >> 8) & 0xFF
        s[12] = crc & 0xFF        # 老版本：32 位 CRC 只写了低 16 位，还自己盖自己
        return self._section(PAT_PID, bytes(s))

    def pmt(self, stream_type):
        s = bytearray(18)          # 同样少了 3 个字节
        s[0] = 0x02
        s[1] = 0xB0
        s[2] = 18
        s[3] = (PROGRAM >> 8) & 0xFF
        s[4] = PROGRAM & 0xFF
        s[5] = 0xC1
        s[6] = 0
        s[7] = 0
        s[8] = 0xE0 | ((VID_PID >> 8) & 0x1F)
        s[9] = VID_PID & 0xFF
        s[10] = 0xF0
        s[11] = 0x00
        s[12] = stream_type
        s[13] = 0xE0 | ((VID_PID >> 8) & 0x1F)
        s[14] = VID_PID & 0xFF
        s[15] = 0xF0
        s[16] = 0x00
        crc = crc32_mpeg(bytes(s[:16]))
        s[17] = (crc >> 8) & 0xFF
        s[17] = crc & 0xFF
        return self._section(PMT_PID, bytes(s))


def slice_nal(nal_type, payload_len, filled):
    """伪造一个切片 NAL：只为了把容器填满，ffmpeg 解不通它无所谓。"""
    return START + bytes([0x60 | nal_type]) + bytes([filled]) * payload_len


def build(muxer_cls, path):
    muxer = muxer_cls()
    stream_type = 0x1B
    out = bytearray()
    for i in range(90):
        if i == 0:
            # 关键帧：参数集 + IDR，跟核心交下来的形态一致
            body = START + SPS + START + PPS + slice_nal(5, 8800, 0x88)
        elif i == 40:
            # 一个大帧，走 PES 的不定长分支
            body = slice_nal(1, 70000, 0x55)
        else:
            body = slice_nal(1, 320, 0x11 + (i % 7))
        pts_us = i * 33333
        out += muxer.frame(i, body, pts_us, stream_type)
    with io.open(path, 'wb') as f:
        f.write(bytes(out))
    return len(out)


def probe(path, label):
    print('=' * 70)
    print('%s  (%s)' % (label, path))
    print('=' * 70)
    r = subprocess.run(
        ['ffprobe', '-v', 'error', '-show_entries',
         'stream=index,codec_name,codec_type,width,height,id',
         '-show_entries', 'program=program_id,pmt_pid,nb_streams',
         '-of', 'default=noprint_wrappers=1', path],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(r.stdout.decode('utf-8', 'replace').strip())

    err = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-f', 'null', '-'],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    text = err.stderr.decode('utf-8', 'replace').strip()
    lines = [l for l in text.splitlines() if l.strip()]
    print('--- ffmpeg 报的错（前 8 条，共 %d 条）---' % len(lines))
    for line in lines[:8]:
        print('   ', line)

    pk = subprocess.run(
        ['ffprobe', '-v', 'error', '-select_streams', 'v',
         '-show_entries', 'packet=pts_time,dts_time,size', '-of', 'csv=p=0', path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    rows = [r for r in pk.stdout.decode('utf-8', 'replace').splitlines() if r.strip()]
    print('--- 包数：%d ---' % len(rows))
    times = []
    for row in rows:
        parts = row.split(',')
        try:
            times.append(float(parts[0]))
        except ValueError:
            pass
    if len(times) > 1:
        mono = all(times[i] <= times[i + 1] for i in range(len(times) - 1))
        print('    PTS 起点=%s 终点=%s 单调=%s' % (times[0], times[-1], mono))
    print()


if __name__ == '__main__':
    n_new = build(Muxer, 'verify_new.ts')
    n_old = build(OldMuxer, 'verify_old.ts')
    print('新版 %d 字节，旧版 %d 字节\n' % (n_new, n_old))
    probe('verify_old.ts', '旧版（0.5.70 的布局）')
    probe('verify_new.ts', '新版（本次改动）')
