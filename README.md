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
| Android TV / 电视盒子（全 ABI） | 电视 App（APK 侧载） | Kotlin + Compose for TV |
| Linux | 桌面软件 | GTK 4（P2，暂缓） |

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
| 1 | 设备发现：mDNS 广播与 SSDP 应答，设备名称即时生效 | 计划中 |
| 2 | DLNA 接收：SSDP + SOAP + GENA 事件 | 计划中 |
| 3 | 媒体管线：FFmpeg 解码、硬解优先、音画同步、渲染 | 计划中 |
| 4 | AirPlay 镜像：配对、FairPlay、镜像流解密、RAOP 音频 | 计划中 |
| 5 | Android TV：遥控器界面、屏幕 PIN、开机自启、全 ABI | 计划中 |
| 6 | 体验与安全：托盘 / 菜单栏常驻、白名单、全屏截图、中英双语 | 计划中 |

批次 0 的界面可以启动、显示设备信息与日志，但**还收不到投屏** —— 协议服务从批次 1 开始接入。

## 构建

本项目**不使用本机编译发布**，所有安装包由 GitHub Actions 构建并发布到 Release。

### 发版

```bash
git tag v0.1.0
git push origin v0.1.0
```

CI 会构建四个平台并自动挂到 Release 上。

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
| Android TV | `ubuntu-22.04` | `adisplay-android-tv.apk` |

首次构建要源码编译第三方依赖（30–60 分钟），之后命中 vcpkg 二进制缓存会快很多。

## 关于 AirPlay 与许可

AirPlay 镜像依赖 Apple 的 FairPlay 机制，所有第三方实现都是逆向工程产物。
本项目按文档 3.1.3 的路线，复用 [UxPlay](https://github.com/FDH2/UxPlay) 的协议层
（GPLv3），与本仓库的 AGPL-3.0 兼容。仅在自己的设备与网络内使用。

iOS 大版本升级可能导致 AirPlay 失效，这是文档第 11 节列为「高」等级的风险。

## 许可

[AGPL-3.0](LICENSE)
