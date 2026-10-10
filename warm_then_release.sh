#!/bin/zsh
set -u
SHA=493a1dc7
WARM=38072188788

echo "=== 第一步：等养缓存这一轮（main，$SHA）==="
gh run watch $WARM --exit-status >/dev/null 2>&1
RC=$?
gh run view $WARM --json jobs --jq '.jobs[] | "  \(.conclusion)\t\(.name)"'

if [ $RC -ne 0 ]; then
  echo "养缓存没成功，不打 tag。失败详情："
  for J in $(gh run view $WARM --json jobs --jq '.jobs[] | select(.conclusion=="failure") | .databaseId'); do
    gh api "repos/cat-clever/ADisplay/actions/jobs/$J/logs" --allow-escape-sequences 2>/dev/null \
      | sed 's/\x1b\[[0-9;]*m//g' \
      | grep -oE "[A-Za-z0-9_.]+\.(cs|xaml|cpp|h)\([0-9]+[,:][0-9]*\)?: ?error [A-Z]+[0-9]+: [^|]{0,150}|CMake Error[^|]{0,160}|error [A-Z]+[0-9]+: [^|]{0,160}" \
      | sort -u | head -15
  done
  exit 1
fi

echo
echo "=== 第二步：养缓存成功，打 tag v0.5.81 并推送 ==="
git tag -a v0.5.81 $SHA -m "$(cat <<'MSG'
v0.5.81

swscale 放回去：YUV→BGRA 仍交给库，不自写。

上一版去掉它是因为每轮重编 FFmpeg 14 分钟，但真正的原因是 GitHub 缓存按 ref
分域 —— tag 上创建的缓存别的 ref 读不到，只有默认分支的缓存共享。改了依赖之后
只需先在 main 上跑一轮 ci.yml 把缓存养到默认分支，不必为此动依赖。这条规矩已写进
缓存步骤的注释。

本版包含：断线修复（手机周期性 /info 被当成抢占连接）、延迟两处（解码器单线程、
日志窗合并刷新）、日志窗摆位、关闭主窗即确定退出。
MSG
)" && git push origin v0.5.81 2>&1 | tail -1

sleep 20
REL=$(gh run list --limit 1 --json databaseId --jq '.[0].databaseId')
echo
echo "=== 第三步：等发布（$REL），并核对 FFmpeg 是否为秒装 ==="
gh run watch $REL --exit-status >/dev/null 2>&1
gh run view $REL --json jobs --jq '.jobs[] | "  \(.conclusion)\t\(.name)"'
echo "--- Windows 两个 job 的耗时 ---"
gh run view $REL --json jobs --jq '.jobs[] | select(.name | test("Windows")) | "  \(.name)  开始 \(.startedAt)  结束 \(.completedAt)"'
JID=$(gh run view $REL --json jobs --jq '.jobs[] | select(.name | contains("Windows x64")) | .databaseId')
gh api "repos/cat-clever/ADisplay/actions/jobs/$JID/logs" --allow-escape-sequences 2>/dev/null \
  | sed 's/\x1b\[[0-9;]*m//g' \
  | grep -iE "Elapsed time to handle ffmpeg|Building ffmpeg|Cache restored from key" | head -5
echo "--- 产物 ---"
gh release view v0.5.81 --json url,assets --jq '"\(.url)", (.assets[] | "  \(.name)  \(.size)")' 2>/dev/null || echo "（还没有 Release）"
