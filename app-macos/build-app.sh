#!/usr/bin/env bash
#
# 把 SwiftPM 产物组装成 ADisplay.app。
#
# 用脚本而不是 Xcode 工程：文档 1.1 说明本项目仅个人自用，不需要公证与上架，
# 本机编译 + ad-hoc 签名即可（文档 4.5）。脚本化的好处是 CI 里一眼能看懂
# 每一步做了什么，出问题好定位。
#
# 用法：build-app.sh <可执行文件路径> <libcastcore.dylib 路径> <输出目录>

set -euo pipefail

EXECUTABLE_PATH="${1:?需要可执行文件路径}"
CORE_LIBRARY_PATH="${2:?需要 libcastcore.dylib 路径}"
OUTPUT_DIR="${3:?需要输出目录}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APP_BUNDLE="${OUTPUT_DIR}/ADisplay.app"
CONTENTS_DIR="${APP_BUNDLE}/Contents"
MACOS_DIR="${CONTENTS_DIR}/MacOS"
FRAMEWORKS_DIR="${CONTENTS_DIR}/Frameworks"

rm -rf "${APP_BUNDLE}"
mkdir -p "${MACOS_DIR}" "${FRAMEWORKS_DIR}"

# 可执行文件
cp "${EXECUTABLE_PATH}" "${MACOS_DIR}/ADisplay"
chmod +x "${MACOS_DIR}/ADisplay"

# 核心库放进 Frameworks 并改写安装名，让可执行文件按 @rpath 找它。
# 不这么做的话，用户机器上必须把 dylib 装到固定路径才能启动。
if [ ! -f "${CORE_LIBRARY_PATH}" ]; then
    echo "找不到核心库：${CORE_LIBRARY_PATH}" >&2
    echo "构建目录内容：" >&2
    ls -l "$(dirname "${CORE_LIBRARY_PATH}")" >&2 || true
    exit 1
fi
cp "${CORE_LIBRARY_PATH}" "${FRAMEWORKS_DIR}/libcastcore.dylib"
chmod +w "${FRAMEWORKS_DIR}/libcastcore.dylib"

install_name_tool -id "@rpath/libcastcore.dylib" "${FRAMEWORKS_DIR}/libcastcore.dylib"

# 可执行文件可能以多种形式引用核心库，逐个改写（不存在的方式会报错，忽略即可）。
for old_ref in \
    "$(otool -L "${MACOS_DIR}/ADisplay" | grep -o '[^ ]*libcastcore[^ ]*' | head -n 1)" \
    "libcastcore.dylib"
do
    if [ -n "${old_ref}" ]; then
        install_name_tool -change "${old_ref}" "@rpath/libcastcore.dylib" \
            "${MACOS_DIR}/ADisplay" 2>/dev/null || true
    fi
done

install_name_tool -add_rpath "@executable_path/../Frameworks" "${MACOS_DIR}/ADisplay" 2>/dev/null || true

# Info.plist
cp "${SCRIPT_DIR}/Info.plist" "${CONTENTS_DIR}/Info.plist"

# 自检：可执行文件引用的 castcore 名字，必须和包里实际存在的文件对得上。
#
# 这个检查是为了挡住 libcastcore.0.dylib 与 libcastcore.dylib 那种
# 「链接时的名字和拷进去的名字不一致」—— 它不会让构建失败，
# 但 App 一启动就崩，而且用户看到的是一份很难读懂的 dyld 报告。
REFERENCED="$(otool -L "${MACOS_DIR}/ADisplay" | grep -o '@rpath/libcastcore[^ ]*' | head -n 1)"
if [ -n "${REFERENCED}" ]; then
    EXPECTED="${FRAMEWORKS_DIR}/$(basename "${REFERENCED}")"
    if [ ! -f "${EXPECTED}" ]; then
        echo "包内缺少可执行文件引用的核心库：${REFERENCED}" >&2
        echo "Frameworks 目录内容：" >&2
        ls -l "${FRAMEWORKS_DIR}" >&2
        exit 1
    fi
    echo "核心库引用一致：${REFERENCED}"
fi

# ad-hoc 签名。文档 4.5：自用无需开发者账号与公证，拷到自己的其他 Mac 时
# 在「隐私与安全性」里放行即可。
codesign --force --deep --sign - "${APP_BUNDLE}"

echo "已生成 ${APP_BUNDLE}"
