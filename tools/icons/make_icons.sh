#!/usr/bin/env bash
#
# 一条命令重做三端图标。
#
#   tools/icons/make_icons.sh
#
# 做的事：Swift 渲染各尺寸 PNG → 打包成 Windows 的 .ico 与 macOS 的 .icns →
# 生成 Android 的两份矢量 drawable → 跑一致性检查。
# 生成的产物都是仓库里的文件，日常改图标只要跑这一条，别手工改产物。
#
# 依赖：swift（随命令行工具）、iconutil（macOS 自带）、python3。都在开发机上，
# CI 不需要跑它 —— 产物已经提交，CI 只做一致性检查（见 check_icon_consistency.py）。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUT_DIR="${ROOT_DIR}/.icons-build"

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

echo "1/4 渲染各尺寸 PNG"
swift "${SCRIPT_DIR}/make_icon.swift" "${OUT_DIR}" >/dev/null

echo "2/4 打包 Windows .ico"
python3 "${SCRIPT_DIR}/build_ico.py" "${OUT_DIR}" "${ROOT_DIR}/app-windows/adisplay.ico"

echo "3/4 打包 macOS .icns"
ICONSET="${OUT_DIR}/ADisplay.iconset"
mkdir -p "${ICONSET}"
# 名字是 iconutil 认死的，映射关系不能改：@2x 就是两倍像素的那一档。
cp "${OUT_DIR}/icon-16.png"   "${ICONSET}/icon_16x16.png"
cp "${OUT_DIR}/icon-32.png"   "${ICONSET}/icon_16x16@2x.png"
cp "${OUT_DIR}/icon-32.png"   "${ICONSET}/icon_32x32.png"
cp "${OUT_DIR}/icon-64.png"   "${ICONSET}/icon_32x32@2x.png"
cp "${OUT_DIR}/icon-128.png"  "${ICONSET}/icon_128x128.png"
cp "${OUT_DIR}/icon-256.png"  "${ICONSET}/icon_128x128@2x.png"
cp "${OUT_DIR}/icon-256.png"  "${ICONSET}/icon_256x256.png"
cp "${OUT_DIR}/icon-512.png"  "${ICONSET}/icon_256x256@2x.png"
cp "${OUT_DIR}/icon-512.png"  "${ICONSET}/icon_512x512.png"
cp "${OUT_DIR}/icon-1024.png" "${ICONSET}/icon_512x512@2x.png"
iconutil -c icns "${ICONSET}" -o "${ROOT_DIR}/app-macos/adisplay.icns"

echo "4/4 生成 Android 矢量并检查三端一致性"
python3 "${SCRIPT_DIR}/make_android_icons.py"
python3 "${SCRIPT_DIR}/check_icon_consistency.py"

rm -rf "${OUT_DIR}"
echo "完成：Windows / macOS / Android TV 三端图标已更新"
