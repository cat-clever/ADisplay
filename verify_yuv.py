# -*- coding: utf-8 -*-
# 验证 MirrorVideoDecoder.cpp 里那套 YUV→BGRA 定点系数：把同一份合成 YUV 帧
# 分别用「我的系数（Python 复刻）」和「ffmpeg 的 swscale（强制同一矩阵/范围）」
# 转换，逐像素比对。差在 ±2 以内算通过（定点舍入的正常差异）。
import io
import subprocess
import sys

W, H = 64, 64


def make_frame():
    """亮度铺满 0..255，色度在 U/V 方向各铺一遍 —— 极值、裁剪、舍入都能覆盖到。"""
    y = bytearray()
    for row in range(H):
        for col in range(W):
            y.append((col * 255) // (W - 1))
    u = bytearray()
    v = bytearray()
    for row in range(H // 2):
        for col in range(W // 2):
            u.append((col * 255) // (W // 2 - 1))
            v.append((row * 255) // (H // 2 - 1))
    return bytes(y) + bytes(u) + bytes(v)


# 与 C++ 里 pick_matrix 完全一致的系数表
TABLES = {
    ('601', 'limited'): (76283, 104595, -25675, -53279, 132203),
    ('709', 'limited'): (76283, 117489, -13975, -34925, 138438),
    ('601', 'full'):    (65536, 91881, -22554, -46802, 116130),
    ('709', 'full'):    (65536, 103206, -12275, -30678, 121609),
}


def clamp(v):
    if v < 0:
        return 0
    if v > 255:
        return 255
    return v


def convert(yuv, matrix_name, range_name):
    y_gain, r_cr, g_cb, g_cr, b_cb = TABLES[(matrix_name, range_name)]
    limited = range_name == 'limited'
    out = bytearray(W * H * 4)
    for row in range(H):
        for col in range(W):
            luma = yuv[row * W + col]
            if limited:
                luma = luma - 16
            u = yuv[W * H + (row // 2) * (W // 2) + (col // 2)] - 128
            v = yuv[W * H + W * H // 4 + (row // 2) * (W // 2) + (col // 2)] - 128
            base = y_gain * luma
            r = (base + r_cr * v + 32768) >> 16
            g = (base + g_cb * u + g_cr * v + 32768) >> 16
            b = (base + b_cb * u + 32768) >> 16
            i = (row * W + col) * 4
            out[i + 0] = clamp(b)
            out[i + 1] = clamp(g)
            out[i + 2] = clamp(r)
            out[i + 3] = 255
    return bytes(out)


def ffmpeg_convert(yuv, matrix_name, range_name, path_in, path_out):
    io.open(path_in, 'wb').write(yuv)
    in_range = 'pc' if range_name == 'full' else 'tv'
    src_fmt = 'yuvj420p' if range_name == 'full' else 'yuv420p'
    matrix = 'bt709' if matrix_name == '709' else 'bt601'
    vf = ('scale=in_color_matrix=%s:in_range=%s:out_color_matrix=%s:out_range=pc'
          % (matrix, in_range, matrix))
    subprocess.run(['ffmpeg', '-v', 'error', '-f', 'rawvideo', '-pix_fmt', src_fmt,
                    '-s', '%dx%d' % (W, H), '-i', path_in, '-vf', vf,
                    '-pix_fmt', 'bgra', '-f', 'rawvideo', path_out, '-y'], check=True)
    return io.open(path_out, 'rb').read()


if __name__ == '__main__':
    yuv = make_frame()
    ok = True
    for matrix_name in ('601', '709'):
        for range_name in ('limited', 'full'):
            mine = convert(yuv, matrix_name, range_name)
            reference = ffmpeg_convert(yuv, matrix_name, range_name,
                                       'yuv_in.raw', 'yuv_ref.bgra')
            worst = 0
            over2 = 0
            for i in range(0, len(mine), 4):
                for c in range(3):
                    d = abs(mine[i + c] - reference[i + c])
                    if d > worst:
                        worst = d
                    if d > 2:
                        over2 += 1
            verdict = '通过' if worst <= 2 else '不一致'
            if worst > 2:
                ok = False
            print('BT.%s %-8s  最大差 %d  超±2 的通道数 %d  -> %s'
                  % (matrix_name, range_name, worst, over2, verdict))
    print('整体：' + ('与 ffmpeg 一致' if ok else '有出入，需要修正系数'))
    sys.exit(0 if ok else 1)
