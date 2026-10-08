// ADisplay —— macOS 主界面
//
// 与 Windows 端保持同样的信息结构：设备名称、服务开关、本机信息、日志。
// 界面本身不做任何协议相关的判断，全部状态来自 castcore。

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
        // 布局策略：前三块按内容取高，日志区吃掉剩余空间。
        //
        // 这样窗口缩小时先压日志区，而不是把某一块挤出可视区域 ——
        // 后者会让用户看不到控件却也不知道为什么。日志区的 minHeight
        // 与窗口的 minHeight 配合，保证压到极限时仍能显示几行。
        VStack(alignment: .leading, spacing: 20) {
            deviceNameSection
            serviceSection
            infoSection
            logSection
                .frame(minHeight: 120, maxHeight: .infinity)
        }
        .padding(24)
        .frame(minWidth: 520, minHeight: 520)
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

    private func copyAllLogs() {
        let text = model.logs.joined(separator: "\n")
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(text, forType: .string)
    }

    private var logSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("日志")
                    .font(.headline)

                Spacer()

                // 日志区每行是独立的 Text（为了能懒加载、不一次渲染几百行），
                // 代价是只能一行一行选。想贴给别人时很不方便，所以给个
                // 一键复制全部的入口。
                Button("复制全部") {
                    copyAllLogs()
                }
                .buttonStyle(.borderless)
                .font(.caption)
                .disabled(model.logs.isEmpty)
            }

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 2) {
                        ForEach(Array(model.logs.enumerated()), id: \.offset) { index, line in
                            Text(line)
                                .font(.system(size: 11, design: .monospaced))
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .id(index)
                        }
                    }
                    .padding(8)
                }
                .background(Color(nsColor: .textBackgroundColor).opacity(0.5))
                .cornerRadius(6)
                .onChange(of: model.logs.count) { _ in
                    guard let last = model.logs.indices.last else { return }
                    proxy.scrollTo(last, anchor: .bottom)
                }
            }
        }
    }
}
