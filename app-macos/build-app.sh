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

# ad-hoc 签名。文档 4.5：自用无需开发者账号与公证，拷到自己的其他 Mac 时
# 在「隐私与安全性」里放行即可。
codesign --force --deep --sign - "${APP_BUNDLE}"

echo "已生成 ${APP_BUNDLE}"
