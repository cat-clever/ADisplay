# -*- coding: utf-8 -*-
# 验两件事：
#   1) 参数集扫描（ScanParameterSets 的 Python 复刻）在 H.264 / H.265 上都认得出来
#   2) 用真的 H.265 码流封出来的 TS，ffprobe 能不能认出 hevc 与分辨率
import io
import subprocess
import verify_ts as V

START4 = b'\x00\x00\x00\x01'


def split_annex_b(data):
    """按起始码切开，返回 [(起始码, NAL 整段)]。"""
    units = []
    i = 0
    starts = []
    while i + 3 < len(data):
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                starts.append((i, 3)); i += 3; continue
            if data[i + 2] == 0 and data[i + 3] == 1:
                starts.append((i, 4)); i += 4; continue
        i += 1
    for k, (pos, ln) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else len(data)
        units.append(data[pos:end])
    return units


def scan_parameter_sets(annexb, is_h265):
    """C# 里 ScanParameterSets 的逐行复刻。返回 (这一帧带没带参数集, 缓存下来的那一段)。"""
    first_set = 32 if is_h265 else 7
    last_set = 34 if is_h265 else 8
    slice_max = 31 if is_h265 else 5

    saw_sets = False
    sets_start = -1
    sets_end = -1
    index = 0
    while index + 3 < len(annexb):
        if annexb[index] == 0 and annexb[index + 1] == 0:
            if annexb[index + 2] == 1:
                start_len = 3
            elif annexb[index + 2] == 0 and index + 3 < len(annexb) and annexb[index + 3] == 1:
                start_len = 4
            else:
                index += 1
                continue
        else:
            index += 1
            continue

        nal_start = index + start_len
        if nal_start >= len(annexb):
            break
        nal_type = ((annexb[nal_start] >> 1) & 0x3F) if is_h265 else (annexb[nal_start] & 0x1F)
        if nal_type <= slice_max:
            break
        nal_end = -1
        for j in range(nal_start, len(annexb) - 3):
            if annexb[j] == 0 and annexb[j + 1] == 0 and \
               (annexb[j + 2] == 1 or (annexb[j + 2] == 0 and annexb[j + 3] == 1)):
                nal_end = j
                break
        if nal_end < 0:
            nal_end = len(annexb)
        if nal_type == first_set:
            saw_sets = True
            sets_start = index
            sets_end = nal_end
        elif saw_sets and nal_type <= last_set:
            sets_end = nal_end
        index = nal_end if nal_end > index else index + 1

    if sets_start >= 0 and sets_end > sets_start:
        return True, annexb[sets_start:sets_end]
    return False, None


STREAM_TYPE = {'h264': 0x1B, 'h265': 0x24}


def build_from(codec, units, key_index, path, frames=60):
    """units[key_index] 是自带参数集的关键帧；其余帧拿一个切片 NAL 顶上。"""
    m = V.Muxer()
    muxer = V.Muxer()
    out = bytearray()
    cached = None
    st = STREAM_TYPE[codec]
    for i in range(frames):
        if i == key_index:
            body = units[0]
        else:
            body = units[1]
        has, sets = scan_parameter_sets(body, codec == 'h265')
        if has:
            cached = sets
        elif cached is not None:
            body = cached + body
        out += muxer.frame(i, body, i * 33333, st)
    io.open(path, 'wb').write(bytes(out))


def h264_units():
    return [START4 + V.SPS + START4 + V.PPS + START4 + bytes([0x65]) + b'\x88' * 2000,
            START4 + bytes([0x41]) + b'\x11' * 300]


def h265_units():
    data = io.open('ref265.hevc', 'rb').read()
    raw = split_annex_b(data)
    vps = sps = pps = None
    slices = []
    for u in raw:
        t = (u[4] if u[3:4] == b'\x01' else u[3]) if False else None
        # 找 NAL 头：起始码 3 或 4 字节
        off = 4 if u[2] == 0 else 3
        t = u[off] >> 1
        if t == 32 and vps is None:
            vps = u
        elif t == 33 and sps is None:
            sps = u
        elif t == 34 and pps is None:
            pps = u
        elif t <= 31:
            slices.append(u)
    head = vps + sps + pps + slices[0]
    return [head, slices[-1]]


if __name__ == '__main__':
    h264 = h264_units()
    h265 = h265_units()
    print('--- 参数集扫描 ---')
    for name, units, is265 in (('H.264', h264, False), ('H.265', h265, True)):
        has, sets = scan_parameter_sets(units[0], is265)
        has2, _ = scan_parameter_sets(units[1], is265)
        print('  %s 关键帧：认出参数集=%s，缓存 %d 字节；普通帧：%s'
              % (name, has, len(sets) if sets else 0, has2))
        assert has is True and has2 is False, name

    print('--- 封出来的 TS 让 ffprobe 判断 ---')
    for name, units, codec in (('H.264', h264, 'h264'), ('H.265', h265, 'h265')):
        path = 'v265_%s.ts' % codec
        build_from(codec, units, 0, path)
        r = subprocess.run(
            ['ffprobe', '-v', 'error', '-show_entries',
             'stream=codec_name,width,height', '-of', 'csv=p=0', path],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        print('  %s -> %s' % (name, r.stdout.decode().strip()))
