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
        Group {
            if let media = model.activeMedia {
                // 有投屏就把整个窗口让给画面：这是用户此刻唯一关心的事，
                // 设置项等停止投屏后再回来。
                PlayerPage(media: media)
                    .environmentObject(model)
            } else {
                settingsPage
            }
        }
    }

    /// 没有投屏时的设置页。
    private var settingsPage: some View {
        // 四块都按内容取高，剩下的空间留白。窗口因此可以缩到很小 ——
        // 日志移到独立窗口之后，这里不再有「吃掉剩余空间」的那一块。
        VStack(alignment: .leading, spacing: 20) {
            deviceNameSection
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
            Text("设备名称")
                .font(.headline)

            HStack(spacing: 8) {
                TextField("例如：客厅电脑", text: $model.deviceNameDraft)
                    .textFieldStyle(.roundedBorder)
                    .onChange(of: model.deviceNameDraft) { _ in
                        // 输入时就给出提示，而不是等点了保存才报错。
                        nameError = model.validateDeviceNameDraft()
                    }
                    .onSubmit { saveDeviceName() }

                Button("保存", action: saveDeviceName)
                    .disabled(nameError != nil || model.deviceNameDraft.isEmpty)
            }

            if let message = nameError {
                Text(message)
                    .font(.caption)
                    .foregroundColor(.red)
            } else {
                Text("这是手机投屏列表里显示的名字，修改后立即生效，不需要重新配对。")
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

    // MARK: - 服务开关

    private var serviceSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("接收服务")
                .font(.headline)

            HStack(spacing: 12) {
                Toggle("开启接收服务", isOn: Binding(
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
            Text("本机信息")
                .font(.headline)

            infoRow(label: "局域网地址", value: formattedAddresses)
            infoRow(label: "设备标识", value: model.deviceId.isEmpty ? "—" : model.deviceId)
            infoRow(label: "核心版本", value: model.version.isEmpty ? "—" : model.version)
        }
    }

    private var formattedAddresses: String {
        if model.localAddresses.isEmpty {
            return "（未检测到可用的局域网地址）"
        }
        let lines = model.localAddresses
            .split(separator: "\n", omittingEmptySubsequences: true)
            .map(String.init)
        return lines.isEmpty ? "（未检测到可用的局域网地址）" : lines.joined(separator: "，")
    }

    private func infoRow(label: String, value: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Text("\(label)：")
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
            Text("日志")
                .font(.headline)

            HStack(spacing: 12) {
                Button("查看日志") {
                    LogWindowController.shared.show(model: model)
                }
                Text("日志在单独的窗口里显示，投屏时也能一直开着。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}
