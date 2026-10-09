// ADisplay —— macOS 独立的日志窗口
//
// 用 AppKit 的 NSWindow 而不是 SwiftUI 的 openWindow(id:)：后者要 macOS 13，
// 而文档 5.1 定的是最低 macOS 12。
//
// 为什么单独开一个窗口而不是画在页面里：投屏时画面要占满窗口，而日志是排查时
// 才看的东西 —— 两者同时需要盯着的场合很少，挤在一起只会让画面变小、日志也
// 只有巴掌大。设置页与投屏页各留一个「查看日志」按钮，都打开这一个窗口。

import AppKit
import SwiftUI

/// 日志窗口的生命周期。同一个窗口复用：再点「查看日志」只是把它提到前面，
/// 不会越点越多。
@MainActor
final class LogWindowController {
    static let shared = LogWindowController()

    private var window: NSWindow?

    func show() {
        if let existing = window {
            existing.makeKeyAndOrderFront(nil)
            return
        }

        let hosting = NSHostingView(rootView: LogView())
        let created = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 760, height: 460),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        created.title = "ADisplay 日志"
        created.contentView = hosting
        // 关掉之后这个对象还要能再打开一次，所以不随窗口释放 —— 否则第二次点
        // 「查看日志」时那个引用已经是一块野内存。
        created.isReleasedWhenClosed = false
        created.center()
        created.makeKeyAndOrderFront(nil)

        window = created
    }
}

/// 日志窗口的内容。
///
/// 只观察 LogStore，不观察 EngineModel —— 后者是投屏页也在观察的对象，
/// 日志挂在它上面会让投屏页每来一行日志就重排一次（全屏切换时最明显）。
struct LogView: View {

    @ObservedObject private var store = LogStore.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("日志")
                    .font(.headline)

                Spacer()

                // 日志每行是独立的 Text（为了能懒加载、不一次渲染几百行），
                // 代价是只能一行一行选。贴给别人排查时很不方便，所以给一个
                // 一键复制的入口。
                Button("复制全部") {
                    copyAll()
                }
                .buttonStyle(.borderless)
                .font(.caption)
                .disabled(store.lines.isEmpty)
            }

            ScrollViewReader { proxy in
                ScrollView {
                    // 整份日志放在一个 Text 里，而不是每行一个。
                    //
                    // 每行一个便于懒加载，但鼠标拖不出跨行的选区 —— 想复制中间
                    // 一段就只能一行一行来，而「把出错前后几行一起贴出去」恰好
                    // 是看日志的人最常做的事。行数由 LogStore 限制在 500 行，
                    // 而且同一瞬间的多行会合并成一次发布。
                    Text(store.lines.joined(separator: "\n"))
                        .font(.system(size: 11, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                        .id("日志末尾")
                }
                .background(Color(nsColor: .textBackgroundColor).opacity(0.5))
                .cornerRadius(6)
                // 新日志进来时自动滚到底 —— 排查时盯的就是最后几行。
                .onChange(of: store.lines.count) { _ in
                    proxy.scrollTo("日志末尾", anchor: .bottom)
                }
            }
        }
        .padding(16)
        .frame(minWidth: 520, minHeight: 320)
    }

    private func copyAll() {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(store.lines.joined(separator: "\n"), forType: .string)
    }
}
