# -*- coding: utf-8 -*-
"""按与桌面图标同一组比例生成 Android 的矢量图标。

比例（以正方形画布为单位，与 apk/make_icon.swift 里的一致）：
  底       inset 0.051 圆角 0.195
  屏幕外框 (0.190, 0.264, 0.620, 0.420) 圆角 0.042，描边宽 0.048
  底座     (0.420, 0.684, 0.160, 0.052)
  蓝点     圆心 (0.5, 0.474) 半径 0.075
"""
# PROPORTIONS: field_inset=0.051 field_radius=0.195 screen_x=0.190 screen_y=0.264 screen_w=0.620 screen_h=0.420 screen_radius=0.042 screen_stroke=0.048 stand_x=0.420 stand_y=0.684 stand_w=0.160 stand_h=0.052 dot_x=0.500 dot_y=0.474 dot_radius=0.075

import io

FIELD = "#101418"
GLYPH = "#FFFFFF"
SIGNAL = "#4FC3F7"

# 比例
FIELD_INSET, FIELD_RADIUS = 0.051, 0.195
SCREEN = (0.190, 0.264, 0.620, 0.420)
SCREEN_RADIUS, SCREEN_STROKE = 0.042, 0.048
STAND = (0.420, 0.684, 0.160, 0.052)
DOT = (0.500, 0.474, 0.075)


def rect_path(x, y, w, h, r):
    """矢量 drawable / SVG 通用的圆角矩形路径。"""
    return ("M%.2f,%.2f h%.2f a%.2f,%.2f 0 0 1 %.2f,%.2f v%.2f "
            "a%.2f,%.2f 0 0 1 -%.2f,%.2f h-%.2f a%.2f,%.2f 0 0 1 -%.2f,-%.2f v-%.2f "
            "a%.2f,%.2f 0 0 1 %.2f,-%.2f z"
            % (x + r, y, w - 2 * r, r, r, r, r, h - 2 * r,
               r, r, r, r, w - 2 * r, r, r, r, r, h - 2 * r,
               r, r, r, r))


def circle_path(cx, cy, radius):
    return ("M%.2f,%.2f m-%.2f,0 a%.2f,%.2f 0 1,0 %.2f,0 a%.2f,%.2f 0 1,0 -%.2f,0"
            % (cx, cy, radius, radius, radius, radius * 2, radius, radius, radius * 2))


def build(canvas, scale, offset_x, offset_y, with_field, rounded_field):
    def px(*values):
        """把比例换算成画布坐标：前两个按偏移平移，其余（宽高、半径）只乘缩放。"""
        scaled = [v * scale for v in values]
        return (offset_x + scaled[0], offset_y + scaled[1]) + tuple(scaled[2:])

    parts = []
    if with_field:
        if rounded_field:
            fx, fy = px(FIELD_INSET, FIELD_INSET)
            side = (1 - 2 * FIELD_INSET) * scale
            parts.append('    <path\n        android:fillColor="%s"\n        android:pathData="%s" />'
                         % (FIELD, rect_path(fx, fy, side, side, FIELD_RADIUS * scale)))
        else:
            parts.append('    <path\n        android:fillColor="%s"\n'
                         '        android:pathData="M0,0 h%d v%d h-%d z" />'
                         % (FIELD, canvas[0], canvas[1], canvas[0]))

    sx, sy, sw, sh = px(*SCREEN)
    stroke = SCREEN_STROKE * scale
    inset = stroke / 2
    parts.append('    <path\n        android:strokeColor="%s"\n        android:strokeWidth="%.2f"\n'
                 '        android:pathData="%s" />'
                 % (GLYPH, stroke,
                    rect_path(sx + inset, sy + inset, sw - stroke, sh - stroke,
                              max(0.5, SCREEN_RADIUS * scale - inset))))

    tx, ty, tw, th = px(*STAND)
    # 底座是直角矩形：不用圆角那条路径 —— 它会生成半径 0 的弧线（a0,0），
    # 按规范等价于直线，但没必要给解析器出难题。
    parts.append('    <path\n        android:fillColor="%s"\n        android:pathData="%s" />'
                 % (GLYPH, "M%.2f,%.2f h%.2f v%.2f h-%.2f z" % (tx, ty, tw, th, tw)))

    cx, cy, radius = px(*DOT)   # px 已经乘过 scale，这里不能再乘一次
    parts.append('    <path\n        android:fillColor="%s"\n        android:pathData="%s" />'
                 % (SIGNAL, circle_path(cx, cy, radius)))
    return "\n".join(parts)


HEAD = '''<?xml version="1.0" encoding="utf-8"?>
<!--
    %s

    图形与 Windows / macOS 的图标是同一套比例（底 inset 5.1%%、屏幕 62%%×42%%、
    底座 16%%、蓝点半径 7.5%%），改这里就要一起改另外两端 —— 桌面那两份由
    tools/icons/make_icon.swift 渲染，比例写在那个文件的注释里。
-->
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="%s"
    android:height="%s"
    android:viewportWidth="%d"
    android:viewportHeight="%d">

%s
</vector>
'''

# 启动器图标：方形画布 + 圆角深底（原来只有白描边、透明底，放到浅色背景上就看不见）
launcher = HEAD % ("应用图标：投屏到屏幕的简笔示意图。",
                   "108dp", "108dp", 108, 108,
                   build((108, 108), 108, 0, 0, True, True))
io.open('app-tv/app/src/main/res/drawable/ic_launcher.xml', 'w', encoding='utf-8').write(launcher)

# 电视横幅：320×180 满幅深底，图形按高度缩放后居中（电视桌面上显示的就是它）
banner = HEAD % ("电视桌面上显示的横幅图标（文档 4.5 要求 320×180 横幅）。",
                 "320dp", "180dp", 320, 180,
                 build((320, 180), 180, 70, 0, True, False))
io.open('app-tv/app/src/main/res/drawable/tv_banner.xml', 'w', encoding='utf-8').write(banner)
print('两份 Android 矢量图标已按同一比例重写')
