# -*- coding: utf-8 -*-
# 逐项把缺陷放回去，看哪一处才是「参数集送不到解复用器」的原因。
import io
import subprocess
import verify_ts as V


class NoPcr(V.Muxer):
    def frame(self, index, annexb, pts_us, stream_type):
        self._disable_pcr = True
        return V.Muxer.frame(self, index, annexb, pts_us, stream_type)

    def _header(self, pid, start, pcr90k):
        return V.Muxer._header(self, pid, start, -1)


class GlobalCc(V.Muxer):
    def _header(self, pid, start, pcr90k):
        if not hasattr(self, 'gcc'):
            self.gcc = 0
        b, off = V.Muxer._header(self, pid, start, pcr90k)
        # 已经有各 PID 自己的计数了，这里再改成全局共享：把两个计数都指向同一个值
        return b, off

    def pat(self):
        if not hasattr(self, 'gcc'):
            self.gcc = 0
        return self._shared(V.Muxer.pat(self))

    def _shared(self, packet):
        return packet


class ShortPsi(V.Muxer):
    """只把 PSI 段改回老版本的「数组按 section_length 开、CRC 只写 2 字节」。"""
    def pat(self):
        s = bytearray(13)
        s[0] = 0x00; s[1] = 0xB0; s[2] = 13
        s[3] = (V.TSID >> 8) & 0xFF; s[4] = V.TSID & 0xFF
        s[5] = 0xC1; s[6] = 0; s[7] = 0
        s[8] = (V.PROGRAM >> 8) & 0xFF; s[9] = V.PROGRAM & 0xFF
        s[10] = 0xE0 | ((V.PMT_PID >> 8) & 0x1F); s[11] = V.PMT_PID & 0xFF
        crc = V.crc32_mpeg(bytes(s[:11]))
        s[12] = (crc >> 8) & 0xFF; s[12] = crc & 0xFF
        return self._section(V.PAT_PID, bytes(s))

    def pmt(self, stream_type):
        s = bytearray(18)
        s[0] = 0x02; s[1] = 0xB0; s[2] = 18
        s[3] = (V.PROGRAM >> 8) & 0xFF; s[4] = V.PROGRAM & 0xFF
        s[5] = 0xC1; s[6] = 0; s[7] = 0
        s[8] = 0xE0 | ((V.VID_PID >> 8) & 0x1F); s[9] = V.VID_PID & 0xFF
        s[10] = 0xF0; s[11] = 0x00
        s[12] = stream_type
        s[13] = 0xE0 | ((V.VID_PID >> 8) & 0x1F); s[14] = V.VID_PID & 0xFF
        s[15] = 0xF0; s[16] = 0x00
        crc = V.crc32_mpeg(bytes(s[:16]))
        s[17] = (crc >> 8) & 0xFF; s[17] = crc & 0xFF
        return self._section(V.PMT_PID, bytes(s))


def check(cls, name, path):
    V.build(cls, path)
    out = subprocess.run(
        ['ffprobe', '-v', 'error', '-select_streams', 'v',
         '-show_entries', 'stream=width,height', '-of', 'csv=p=0', path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    warn = subprocess.run(['ffmpeg', '-v', 'warning', '-i', path, '-f', 'null', '-'],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    lines = warn.stderr.decode('utf-8', 'replace').splitlines()
    cont = len([l for l in lines if 'Continuity' in l])
    print('%-26s 分辨率=%-12s 连续性报错=%d' % (name, out.stdout.decode().strip(), cont))


if __name__ == '__main__':
    check(V.Muxer, '新版（本次改动）', 'v_new.ts')
    check(V.OldMuxer, '旧版（0.5.70）', 'v_old.ts')
    check(NoPcr, '新版去掉 PCR', 'v_nopcr.ts')
    check(ShortPsi, '新版用回短 PSI 段', 'v_shortpsi.ts')
