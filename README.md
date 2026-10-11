# ADisplay

跨平台投屏接收端：把 Windows、macOS、Android TV 变成 AirPlay / DLNA 接收端，
手机与电脑在同一局域网时，无需连线即可把屏幕、声音、视频投过来。

**仅个人自用，不对外分发。** 依据仓库根目录的
[开发文档](docs/跨平台投屏接收端软件开发文档v2界面使用原生界面.md) 实现。

## 支持范围

| 手机端 | 投屏方式 | 协议 | 优先级 |
| --- | --- | --- | --- |
| iOS / iPadOS | 控制中心「屏幕镜像」 | AirPlay 镜像（含音频） | P0 |
| iOS / iPadOS | 视频 App 内投屏按钮 | AirPlay 视频（HLS 推送） | P1 |
| Android | 视频 / 音乐 App 内「投屏」 | DLNA / UPnP | P0 |
| Android | 整屏镜像 | 自研协议（配套客户端） | P1 |

| 接收端 | 形态 | 界面 |
| --- | --- | --- |
| Windows 10 / 11（x64、ARM64） | 桌面软件 | C# + WinUI 3 |
| macOS 12+（Apple Silicon + Intel 通用包） | 桌面软件（菜单栏常驻） | Swift + SwiftUI / AppKit |
| Android TV / 电视盒子（arm64-v8a、armeabi-v7a） | 电视 App（APK 侧载） | Kotlin + Compose for TV |
| Linux | 桌面软件 | GTK 4（P2，暂缓） |

Android 手机**系统自带的**「投屏」（小米/澎湃、三星 Smart View 等）走的是 **Miracast**，
本程序**不做** —— 它不是应用能力而是平台能力（能收 Miracast 的设备无一例外是系统的拥有者），
逐平台的证据见 [进度与已知问题](docs/进度与已知问题.md)。想用系统投屏，请在这台 Windows 上
启用系统自带的「无线显示器」（设置 → 系统 → 投影到此电脑）。

## 架构

```
┌─────────────────────────────────────────────────────┐
│ 界面层（各平台原生，彼此独立）                          │
│  WinUI 3 (C#)  │  SwiftUI/AppKit (Swift)  │ Compose  │
└──────────────────────┬──────────────────────────────┘
                       │  ★ 唯一的跨语言边界：纯 C ABI
                       │    C# → P/Invoke   Swift → bridging header   Kotlin → JNI
┌──────────────────────┴──────────────────────────────┐
│ castcore（C++17 跨平台核心库，导出 C 接口）             │
│  会话管理 · 协议模块 · 媒体管线 · 平台适配              │
└─────────────────────────────────────────────────────┘
```

界面层与核心库之间只有一份契约：[`include/adisplay/adisplay.h`](include/adisplay/adisplay.h)。
只传 POD 结构体、UTF-8 字符串和不透明句柄，不传 C++ 类型。
这样界面技术可以随意更换而不动核心（文档 5.2 节的设计前提）。

## 目录结构

```
ADisplay/
├─ include/adisplay/adisplay.h   ★ C ABI，界面与核心之间唯一的契约
├─ core/                          castcore
│  ├─ api/                        C 接口实现
│  └─ common/                     日志、配置、字节序、线程、网络工具
├─ protocols/                     airplay · dlna · castpc
├─ platform/                      windows · macos · linux · android 薄适配
├─ app-windows/                   C# + WinUI 3
├─ app-macos/                     Swift + SwiftUI / AppKit
├─ app-tv/                        Kotlin + Compose for TV
├─ tests/                         单元测试（ctest）
├─ triplets/                      自定义 vcpkg 三元组（macOS 通用二进制）
└─ docs/                          开发文档
```

## 当前进度

| 批次 | 内容 | 状态 |
| --- | --- | --- |
| 0 | 工程骨架、C ABI、核心基础设施、三端界面、四平台 CI 与自动发版 | 已完成 |
| 1 | 设备发现：mDNS 广播与 SSDP 应答，设备名称即时生效 | 已完成 |
| 2 | DLNA 接收：SSDP + SOAP + GENA 事件 | 已完成 |
| 3 | 媒体管线：解码、硬解、音画同步、渲染 | 已完成（路线见下） |
| 4 | AirPlay 镜像：配对、FairPlay、镜像流解密、RAOP 音频 | **已完成** |
| 5 | Android TV：遥控器界面、屏幕 PIN、开机自启、多 ABI | 主体完成（剩**屏幕 PIN**；二维码已决定与自研协议一起做） |
| 6 | 体验与安全：中英双语（界面可切换，三端） | ✅ **已完成**（其余条目按你的要求不做，见进度文档） |

批次 3 的**目标**（投屏能出画面、有声音、能拖动进度条）已经达成，但实现路线与
最初设想不同，而且**两条路不一样**：

- **DLNA（一条 URL）整条交给平台播放器** —— macOS 是 AVPlayer，Windows 是
  Media Foundation，Android 是 ExoPlayer。硬解与音画同步都是现成的，自己再养一套
  是净亏。
- **AirPlay 镜像反过来，在核心里自解码** —— 它是实时流，要求「最新一帧立刻上屏」；
  交给系统播放器会先攒一段再开播（实测 12.6 秒，而且每次重新缓冲还会往上叠）。
  所以那条路是核心用 FFmpeg 解成 BGRA、界面自己画（`core/pipeline/` 的
  `MirrorVideoDecoder`）。

最初的设想（核心统一 FFmpeg + Metal/D3D11，Windows 用 libmpv 作播放内核）没有采用，
libmpv 也没用上。

批次 4 的分平台现状（协议层三端共用同一份代码，**三端均已在真机确认画面与声音**）：

| | 广播 / 握手 / FairPlay / 解密 / 时间同步 | 画面 | 声音 |
| --- | --- | --- | --- |
| macOS | 完成 | 完成（含 4K 与 H.265） | 完成（AAC-ELD，核心用 FFmpeg 解） |
| Windows | 完成 | 完成（核心自解码 → 界面自绘） | 完成 |
| Android TV | 完成 | 完成（`SurfaceView` + `MediaCodec`） | 完成（`MediaCodec` + `AudioTrack`） |

除画面与声音外，批次 4 还做完了两件：**延时压缩**（不再让系统播放器先攒十几秒，
只画最新一帧）与**结束投屏 / 继续观看 / 主动断开**（AirPlay 那条能真正让 iPhone
停止镜像；DLNA 是 DMR，协议上无法命令手机停止，只能尽力而为）。

两处已知缺口：**音频模式（ALAC）不支持** —— 控制中心里选这台设备放音乐是没有
声音的，那条路走 ALAC（`ct=2`），而目前只解镜像伴音的 AAC-ELD（`ct=8`）；
**唇音同步未做** —— 伴音按到达顺序播放，与画面各自独立。另外**手机停止投屏后，
待机页的横幅不会消失**（已知，暂不修）。

各批次的完成标志 vs 现状、以及批次 6 的逐项状态，见
[进度与已知问题](docs/进度与已知问题.md)。

实现过程中卡住过的问题、以及「为什么代码写成这样」的来龙去脉，集中在
[技术难点与踩过的坑](docs/技术难点与踩过的坑.md) —— 协议与媒体管线、各平台的坑、
构建与依赖、界面交互、以及可复用的诊断方法。

## Android 签名（fork 之后也一样）

**每次构建都用仓库里那把公开的密钥库** `app-tv/fallback-signing.p12`，
口令就写在 `app-tv/app/build.gradle.kts` 里。不读环境变量、也不读 Secrets ——
包括 fork 出来的仓库，构建结果与本仓库一致。

**为什么必须固定一把**：Android 要求新旧包签名一致才能就地覆盖升级，否则必须先卸载，
而卸载会清掉应用配置与和设备配对用的密钥。所以签名一旦确定就不能再换。

**为什么不用 Secrets 存私有密钥**：这里早先走过「配了私有密钥就优先用」的三级回退，
麻烦大于收益 —— GitHub 的 Secrets 不复制到 fork，可复用工作流也不继承，两处都只是
**静默**失效：gradle 不报错，直接退回 debug 签名，而 debug 密钥库是每个 runner 构建时
现生成的，等于签名每版都变，发出去的包反而让用户每次升级都要先卸载 —— 正好绕回要
解决的问题上。

**它能保证什么**：同一台设备上能覆盖升级。**它保证不了什么**：发布者身份 ——
公开的东西做不到这件事。好在也不需要：所有构建（含 fork）都用同一个包名
`com.adisplay.tv`，一台设备上只装得下一个。

## 安装

各平台安装包见 [Releases](https://github.com/cat-clever/ADisplay/releases)。
详细的安装步骤、系统拦截怎么放行、以及搜不到设备时该查什么，见 **[安装说明](docs/安装说明.md)**。

## 构建

本项目**不使用本机编译发布**，所有安装包由 GitHub Actions 构建并发布到 Release。

### 发版

```bash
git tag -a v0.5.84 -m "v0.5.84"
git push origin v0.5.84
```

CI 会构建四个平台并自动挂到 Release 上。

版本号以 `CMakeLists.txt` 的 `project(... VERSION ...)` 为准，发版时要同时改
`vcpkg.json`、`app-tv/app/build.gradle.kts`（versionCode 与 versionName）、
`app-macos/Info.plist`（短版本与构建号两处）、`app-windows/ADisplay.Windows.csproj`
—— CI 里有个「版本号一致性」任务会逐个核对，漏一处就红。

**改了依赖、或者新增了一个 ABI（等于新增一个 vcpkg 三元组）时，先热身一轮再打 tag**：
先 `gh workflow run ci.yml`，等它跑完再打。原因是 GitHub 的缓存**按 ref 分域** ——
tag 上创建的缓存别的 ref 读不到，只有默认分支上创建的缓存所有 ref 共享。不先养一次，
每个 tag 轮次都会发现要的东西不在缓存里，于是从头重编（Windows 上实测 4 分钟变 18–21 分钟）。

### 本机开发

需要 CMake ≥ 3.21、Ninja、以及 [vcpkg](https://github.com/microsoft/vcpkg)。
另外按平台需要 MSVC / Xcode 命令行工具 / Android SDK。

```bash
export VCPKG_ROOT=/path/to/vcpkg

# macOS（本机架构，构建更快）
cmake --preset macos-arm64
cmake --build --preset macos-arm64
ctest --preset macos-arm64
```

### CI 矩阵

| Job | Runner | 产物 |
| --- | --- | --- |
| Linux 核心库与测试 | `ubuntu-22.04` | 无（只跑 ctest） |
| Windows x64 | `windows-2022` | `adisplay-windows-x64.zip` |
| Windows ARM64 | `windows-2022` | `adisplay-windows-arm64.zip` |
| macOS Universal | `macos-14` | `adisplay-macos-universal.dmg` |
| Android 核心库 | `ubuntu-22.04` | 无（编 arm64-v8a 与 armeabi-v7a 两个 ABI，并校验产物架构） |
| Android TV | `ubuntu-22.04` | `adisplay-android-tv.apk` |

首次构建要源码编译第三方依赖（30–60 分钟），之后命中 vcpkg 二进制缓存会快很多。

## 关于 AirPlay 与许可

AirPlay 镜像依赖 Apple 的 FairPlay 机制，所有第三方实现都是逆向工程产物。
本项目按文档 3.1.3 的路线，复用 [UxPlay](https://github.com/FDH2/UxPlay) 的协议层
（GPLv3），与本仓库的 AGPL-3.0 兼容。仅在自己的设备与网络内使用。

iOS 大版本升级可能导致 AirPlay 失效，这是文档第 11 节列为「高」等级的风险。

## 许可

[AGPL-3.0](LICENSE)
