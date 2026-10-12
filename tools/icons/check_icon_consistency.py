# -*- coding: utf-8 -*-
"""三端图标一致性检查。

三端的图标用**同一套比例**画：Android 的两份矢量 drawable、Windows 的 .ico、
macOS 的 .icns。比例分别写在两份生成器里（各有一行 PROPORTIONS），这个脚本：

  1. 把两行 PROPORTIONS 对表 —— 改了一端忘了另一端就在这里报出来；
  2. 回读 Android 的两份 XML，按比例反算它实际用的数字（挡住手工改产物）；
  3. 确认两个桌面图标文件在、且不是空壳。

CI 里只跑这一个脚本（不重新渲染）：产物已提交，这里负责发现漂移。

用法：python3 tools/icons/check_icon_consistency.py
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SWIFT = os.path.join(ROOT, 'tools/icons/make_icon.swift')
ANDROID_GEN = os.path.join(ROOT, 'tools/icons/make_android_icons.py')
LAUNCHER = os.path.join(ROOT, 'app-tv/app/src/main/res/drawable/ic_launcher.xml')
BANNER = os.path.join(ROOT, 'app-tv/app/src/main/res/drawable/tv_banner.xml')
DESKTOP = [
    os.path.join(ROOT, 'app-windows/adisplay.ico'),
    os.path.join(ROOT, 'app-macos/adisplay.icns'),
]

# 生成时数字四舍五入到小数点后两位，留出余量。
TOL = 0.004


def read(path):
    return io.open(path, encoding='utf-8').read()


def proportions(path):
    """取出那行机器可读的比例：PROPORTIONS: key=value key=value ..."""
    match = re.search(r'PROPORTIONS:\s*([^\n]*)', read(path))
    if not match:
        sys.stderr.write('%s 里没有 PROPORTIONS 行\n' % path)
        sys.exit(1)
    pairs = {}
    for token in match.group(1).split():
        key, _, value = token.partition('=')
        pairs[key] = float(value)
    return pairs


def numbers(text):
    return [float(v) for v in re.findall(r'-?\d+\.?\d*', text)]


def rounded_rect(nums, stroke=0.0):
    """圆角矩形路径 → (x, y, w, h, 外圆角半径)。

    路径形如 M(x+r),y h(w-2r) a r,r 0 0 1 r,r v(h-2r) …：起点是「x + 圆角半径」，
    不是矩形的 x。数字序列：0=Mx 1=My 2=h 3=rx 4=ry 5=rot 6=large 7=sweep 8=dx 9=dy 10=v。
    """
    r = nums[3]
    return (nums[0] - r - stroke / 2, nums[1] - stroke / 2,
            nums[2] + 2 * r + stroke, nums[10] + 2 * r + stroke, r + stroke / 2)


def parse(path, scale, offset_x):
    """把一份矢量 drawable 反算成比例。"""
    got = {}
    full_bleed = False
    for block in re.findall(r'<path(.*?)/>', read(path), re.S):
        d = re.search(r'pathData="([^"]+)"', block).group(1)
        n = numbers(d)
        if 'fillColor="#101418"' in block:
            if abs(n[0]) < 0.01 and abs(n[1]) < 0.01 and 'a' not in d:
                full_bleed = True
                got['field_side'] = n[2] / scale
                continue
            x, y, w, _, r = rounded_rect(n)
            got['field_radius'] = r / scale
            got['field_inset'] = x / scale
            got['field_side'] = w / scale
        elif 'strokeColor' in block:
            stroke = float(re.search(r'strokeWidth="([^"]+)"', block).group(1))
            x, y, w, h, r = rounded_rect(n, stroke)
            got['screen_stroke'] = stroke / scale
            got['screen_x'] = (x - offset_x) / scale
            got['screen_y'] = y / scale
            got['screen_w'] = w / scale
            got['screen_h'] = h / scale
            got['screen_radius'] = r / scale
        elif 'fillColor="#FFFFFF"' in block:
            got['stand_x'] = (n[0] - offset_x) / scale
            got['stand_y'] = n[1] / scale
            got['stand_w'] = n[2] / scale
            got['stand_h'] = n[3] / scale
        elif 'fillColor="#4FC3F7"' in block:
            got['dot_x'] = (n[0] - offset_x) / scale
            got['dot_y'] = n[1] / scale
            got['dot_radius'] = abs(n[2]) / scale
    got['_full_bleed'] = full_bleed
    return got


def compare(name, got, want):
    bad = []
    for key, value in want.items():
        if key not in got:
            bad.append(key + ' 缺失')
        elif abs(got[key] - value) > TOL:
            bad.append('%s：%.4f，应为 %.4f' % (key, got[key], value))
    if bad:
        print('  [X] %s' % name)
        for line in bad:
            print('      ' + line)
        return False
    print('  [v] %s（%d 项对得上）' % (name, len(want)))
    return True


def main():
    print('三端图标一致性检查')
    ok = True

    swift = proportions(SWIFT)
    android = proportions(ANDROID_GEN)
    if swift != android:
        print('  [X] 两份生成器的比例不一致')
        for key in sorted(set(swift) | set(android)):
            if swift.get(key) != android.get(key):
                print('      %s：桌面 %.4f / Android %s'
                      % (key, swift.get(key, -1), android.get(key, '缺')))
        return 1
    print('  [v] 桌面生成器与 Android 生成器的比例一致（%d 项）' % len(swift))

    # 矢量里实际用的数字必须与比例相符（挡住「直接手改产物」）。
    want = dict(swift)
    want['field_side'] = 1 - 2 * swift['field_inset']
    ok &= compare('ic_launcher.xml（108 画布）',
                  parse(LAUNCHER, 108, 0), want)

    banner_want = dict(want)
    banner_want.pop('field_inset')
    banner_want.pop('field_radius')
    banner_want['field_side'] = 320.0 / 180.0   # 满幅：按 180 的高度尺度算就是 16:9
    banner = parse(BANNER, 180, 70)
    if not banner['_full_bleed']:
        print('  [X] tv_banner.xml 的底不是满幅')
        ok = False
    ok &= compare('tv_banner.xml（180 高度、图形水平居中）', banner, banner_want)

    for path, label, minimum in ((DESKTOP[0], 'app-windows/adisplay.ico', 2048),
                                 (DESKTOP[1], 'app-macos/adisplay.icns', 2048)):
        if not os.path.isfile(path):
            print('  [X] %s 不存在' % label)
            ok = False
        elif os.path.getsize(path) < minimum:
            print('  [X] %s 只有 %d 字节，不像是正常图标（跑 tools/icons/make_icons.sh 重做）'
                  % (label, os.path.getsize(path)))
            ok = False
        else:
            print('  [v] %s（%d 字节）' % (label, os.path.getsize(path)))

    if not ok:
        print('三端图标不一致 —— 改一端要改三端，或跑 tools/icons/make_icons.sh 重做')
        return 1
    print('三端图标一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())
