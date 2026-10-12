// ADisplay —— macOS 主界面
//
// 与 Windows 端保持同样的信息结构：设备名称、服务开关、本机信息，
// 外加一个打开日志窗口的入口。界面本身不做任何协议相关的判断，
// 全部状态来自 castcore。

import AppKit
import SwiftUI

struct ContentView: View {

    @EnvironmentObject private var model: EngineModel

    @State private var nameError: String?
    @State private var isEditingName = false

    var body: some View {
        // 投屏页**一直留在视图树里**（结束投屏之后缩到看不见），而不是被换成设置页 ——
        // 与 Android 端同一个做法（那边是把渲染面缩到 1dp，见 MirrorScreen.kt）。
        //
        // 为什么不能摘掉：镜像的渲染层是 AVSampleBufferDisplayLayer，它一旦离开窗口
        // 就会 flush，之后必须**等一个关键帧**才能恢复；而 iOS 在镜像里整场只插一个
        // 关键帧（实测 1657 帧 1 个）。2026-10-12 用户实测的正是这个：结束投屏 →
        // 继续观看之后，新渲染面一路「丢弃 N 帧（还没有解码参数（SPS/PPS））」，
        // 画面永远黑着（日志见 docs/进度与已知问题.md 的已知问题 3）。
        ZStack {
            if model.activeMedia != nil || model.mirrorSessionId != nil {
                // 有投屏就把整个窗口让给画面：这是用户此刻唯一关心的事，
                // 设置项等停止投屏后再回来（那时它只是被缩到看不见，没有被摘掉）。
                //
                // 镜像会话没有「媒体地址」，所以这里传的是可选的 —— 播放页
                // 自己按 mirrorSessionId 决定走渲染面还是播放器。
                PlayerPage(media: model.activeMedia)
                    .environmentObject(model)
                    // 结束投屏之后缩到千分之一、并让开交互：解码一直在走，
                    // 「继续观看」因此是即时的。用**缩小**而不是 opacity(0) 或
                    // hidden —— 后两者会让系统认为这块不必显示，解码就停了，
                    // 那正好又回到「等关键帧」的死路。
                    .scaleEffect(model.castingDismissed ? 0.001 : 1)
                    .allowsHitTesting(!model.castingDismissed)
            }
            if model.castingDismissed || (model.activeMedia == nil && model.mirrorSessionId == nil) {
                settingsPage
                    // 盖住下面那层缩小的画面。窗口背景色本身不透明，补上它就不必
                    // 依赖「缩到多小才看不见」这件事。
                    .background(Color(nsColor: .windowBackgroundColor))
            }
        }
    }

    /// 结束投屏之后，发送端往往还在推流 —— 给两个入口：回去看，或者真正断开。
    private var resumeBanner: some View {
        HStack(spacing: 12) {
            Text(model.strings.castingEndedBanner)
                .foregroundStyle(.secondary)
            Spacer(minLength: 0)
            Button(model.strings.resumeCasting) {
                model.resumeCasting()
            }
            Button(model.strings.disconnectCasting) {
                model.disconnectCasting()
            }
        }
        .padding(12)
        .background(Color.gray.opacity(0.15), in: RoundedRectangle(cornerRadius: 10))
    }

    /// 没有投屏时的设置页。
    private var settingsPage: some View {
        // 每块都按内容取高，剩下的空间留白。窗口因此可以缩到很小 ——
        // 日志移到独立窗口之后，这里不再有「吃掉剩余空间」的那一块。
        VStack(alignment: .leading, spacing: 20) {
            if model.castingDismissed {
                resumeBanner
            }
            deviceNameSection
            qualitySection
            languageSection
            broadcastSection
            serviceSection
            infoSection
            logEntry
            Spacer(minLength: 0)
        }
        .padding(24)
        .frame(minWidth: 520, minHeight: 460)
    }

    // MARK: - 设备名称（文档 2.4）

    private var deviceNameSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(model.strings.deviceNameLabel)
                .font(.headline)

            HStack(spacing: 8) {
                TextField(model.strings.deviceNamePlaceholder, text: $model.deviceNameDraft)
                    .textFieldStyle(.roundedBorder)
                    .onChange(of: model.deviceNameDraft) { _ in
                        // 输入时就给出提示，而不是等点了保存才报错。
                        nameError = model.validateDeviceNameDraft()
                    }
                    .onSubmit { saveDeviceName() }

                Button(model.strings.save, action: saveDeviceName)
                    .disabled(nameError != nil || model.deviceNameDraft.isEmpty)
            }

            if let message = nameError {
                Text(message)
                    .font(.caption)
                    .foregroundColor(.red)
            } else {
                Text(model.strings.deviceNameHint)
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
    }

    private func saveDeviceName() {
        do {
            try model.applyDeviceName()
            nameError = nil
        } catch {
            nameError = error.localizedDescription
        }
    }

    // MARK: - 画质档位（文档 2.3）

    private var qualitySection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(model.strings.mirrorQuality)
                .font(.headline)

            Picker(model.strings.mirrorQuality, selection: Binding(
                get: { model.qualityPreset },
                set: { model.setQualityPreset($0) }
            )) {
                Text(model.strings.qualityLabel(.smooth)).tag(QualityPreset.smooth)
                Text(model.strings.qualityLabel(.balanced)).tag(QualityPreset.balanced)
                Text(model.strings.qualityLabel(.sharp)).tag(QualityPreset.sharp)
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            Text(model.strings.qualityHint(smooth: QualityPreset.smooth.resolutionText,
                                           balanced: QualityPreset.balanced.resolutionText,
                                           sharp: QualityPreset.sharp.resolutionText))
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }

    // MARK: - 界面语言

    /// 界面语言。排法与电视端一致（画质那一块之后）。
    ///
    /// 「中文」「English」两个词**不翻译** —— 任何语言下都这么写，用户找的是自己认得的
    /// 那两个字，这是通行做法。
    private var languageSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(model.strings.uiLanguage)
                .font(.headline)

            Picker(model.strings.uiLanguage, selection: Binding(
                get: { model.language },
                set: { model.setLanguage($0) }
            )) {
                Text("中文").tag(Language.zh)
                Text("English").tag(Language.en)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
        }
    }

    // MARK: - 广播网卡

    /// 设备发现（SSDP）在哪几块网卡上广播。默认只在实际的局域网上 ——
    /// 打开后才包含 VPN / ZeroTier 这类虚拟网卡。
    ///
    /// 提醒一句预期：tun 模式的 OpenVPN 不转发组播，在它上面即便打开也发现不了
    /// 设备；这一项真正有用的是二层覆盖网（ZeroTier 等）与「整机只有 VPN」的情况。
    private var broadcastSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(model.strings.broadcastLabel)
                .font(.headline)

            Picker(model.strings.broadcastLabel, selection: Binding(
                get: { model.includeVirtualInterfaces },
                set: { model.setIncludeVirtualInterfaces($0) }
            )) {
                Text(model.strings.broadcastAuto).tag(false)
                Text(model.strings.broadcastIncludeVirtual).tag(true)
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            Text(model.strings.broadcastHint)
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }

    // MARK: - 服务开关

    private var serviceSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.strings.receiverService)
                .font(.headline)

            HStack(spacing: 12) {
                Toggle(model.strings.startReceiving, isOn: Binding(
                    get: { model.isRunning },
                    set: { _ in model.toggleService() }
                ))
                .toggleStyle(.switch)

                Text(model.statusText)
                    .foregroundColor(model.state == .error ? .red : .secondary)
            }
        }
    }

    // MARK: - 本机信息
    //
    // 电视端待机页也要显示这些，方便用户核对手机和电脑是否在同一网段（文档 2.3）。

    private var infoSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(model.strings.localInfo)
                .font(.headline)

            infoRow(label: model.strings.infoLocalAddress, value: formattedAddresses)
            infoRow(label: model.strings.infoDeviceId, value: model.deviceId.isEmpty ? "—" : model.deviceId)
            infoRow(label: model.strings.infoCoreVersion, value: model.version.isEmpty ? "—" : model.version)
        }
    }

    private var formattedAddresses: String {
        if model.localAddresses.isEmpty {
            return model.strings.noLocalAddress
        }
        let lines = model.localAddresses
            .split(separator: "\n", omittingEmptySubsequences: true)
            .map(String.init)
        return lines.isEmpty ? model.strings.noLocalAddress
                             : lines.joined(separator: model.strings.addressSeparator)
    }

    private func infoRow(label: String, value: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Text(label + model.strings.labelColon)
                .font(.caption)
                .foregroundColor(.secondary)
            Text(value)
                .font(.caption)
                .textSelection(.enabled)
        }
    }

    // MARK: - 日志

    /// 日志入口。日志本身在独立的窗口里（见 LogWindow.swift）——
    /// 投屏时画面要占满窗口，日志挤在同一页只会两边都变小。
    private var logEntry: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(model.strings.logLabel)
                .font(.headline)

            HStack(spacing: 12) {
                Button(model.strings.viewLog) {
                    LogWindowController.shared.show()
                }
                Text(model.strings.logHint)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}
