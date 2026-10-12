# -*- coding: utf-8 -*-
"""把各尺寸 PNG 打成 Windows 的 .ico。

不依赖任何第三方库：ICO 就是个简单容器 —— 6 字节文件头 + 每个尺寸 16 字节目录项
+ 各自的图像数据。这里每一项都塞 PNG（Vista 起支持），比 BMP 项小得多，也省掉
自己拼位图与掩码。

用法：python3 tools/icons/build_ico.py <PNG 所在目录> <输出 .ico>
"""
import io
import struct
import sys

# 资源管理器、任务栏、Alt-Tab 各自会用到的档位。
SIZES = [16, 24, 32, 48, 64, 128, 256]


def main():
    if len(sys.argv) != 3:
        sys.stderr.write('用法：build_ico.py <PNG 目录> <输出 .ico>\n')
        return 1
    png_dir, out_path = sys.argv[1], sys.argv[2]
    blobs = []
    for size in SIZES:
        path = '%s/icon-%d.png' % (png_dir, size)
        data = io.open(path, 'rb').read()
        if data[:8] != b'\x89PNG\r\n\x1a\n':
            sys.stderr.write('%s 不是 PNG\n' % path)
            return 1
        blobs.append((size, data))

    header = struct.pack('<HHH', 0, 1, len(blobs))   # 保留位, 类型=图标, 项数
    offset = 6 + 16 * len(blobs)
    entries, payload = b'', b''
    for size, data in blobs:
        # 目录项里 256 记作 0（宽度字段只有一个字节）。
        dim = 0 if size >= 256 else size
        entries += struct.pack('<BBBBHHII', dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        payload += data

    io.open(out_path, 'wb').write(header + entries + payload)
    print('已写出 %s（%d 个尺寸：%s）'
          % (out_path, len(blobs), ', '.join(str(s) for s, _ in blobs)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
