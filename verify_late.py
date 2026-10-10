# -*- coding: utf-8 -*-
# 验证「补发参数集」：封装器一直在跑（缓存里有 SPS/PPS），但客户端从半路
# 才接进来，它收到的那一段里没有关键帧 —— 只看补发的参数集够不够解复用器认。
import io
import subprocess
import verify_ts as V

SPS_PPS = V.START + V.SPS + V.START + V.PPS


class PrependMuxer(V.Muxer):
    """在 V.Muxer 之上加「缓存并补发参数集」，与 C# 里 ScanParameterSets 同义。"""

    def __init__(self):
        V.Muxer.__init__(self)
        self.sets = None

    def frame(self, index, annexb, pts_us, stream_type):
        has_sps = SPS_PPS in annexb
        if has_sps:
            self.sets = SPS_PPS
        elif self.sets is not None:
            annexb = self.sets + annexb
        return V.Muxer.frame(self, index, annexb, pts_us, stream_type)


def build(keep_from, path):
    m = PrependMuxer()
    data = bytearray()
    for i in range(90):
        if i == 0:
            body = SPS_PPS + V.slice_nal(5, 8800, 0x88)
        else:
            body = V.slice_nal(1, 320, 0x11 + (i % 7))
        piece = m.frame(i, body, i * 33333, 0x1B)
        if i >= keep_from:          # 只留客户端真正会收到的那一段
            data += piece
    io.open(path, 'wb').write(bytes(data))


def check(path, label):
    out = subprocess.run(
        ['ffprobe', '-v', 'error', '-select_streams', 'v',
         '-show_entries', 'stream=width,height', '-of', 'csv=p=0', path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    print('%-36s 解复用器认出的分辨率 = %s' % (label, out.stdout.decode().strip()))


if __name__ == '__main__':
    build(0, 'late_full.ts')
    build(20, 'late_mid.ts')
    build(60, 'late_late.ts')
    check('late_full.ts', '从头接（第 0 帧是关键帧）')
    check('late_mid.ts', '从第 20 帧接（无关键帧，靠补发）')
    check('late_late.ts', '从第 60 帧接（无关键帧，靠补发）')
