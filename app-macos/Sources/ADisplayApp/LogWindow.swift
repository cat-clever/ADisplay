// ADisplay —— macOS 独立的日志窗口
//
// 用 AppKit 的 NSTextView，不用 SwiftUI。这不只是「顺手」的选择：
//
// SwiftUI 的 NSHostingView 在**每个显示周期**都会向内容要一次最小尺寸
// （updateWindowContentSizeExtremaIfNecessary → sizeThatFits），那是对整棵视图树
// 的一次布局。用 sample 采过：主线程近一半的时间花在这条路上，而其中最贵的一点
// 就是「整份日志放在一个 Text 里」—— 那是一块不断变长、动辄几万字符的文本。
// 投屏时显示周期连续跑（画面在动），这笔开销就变成常态，CPU 直接顶到 90%。
//
// NSTextView 正是为「大量文本 + 可选中」设计的：追加的代价只跟新增的行数有关，
// 而且完全不参与窗口的最小尺寸计算。
//
// 为什么单独开一个窗口而不是画在页面里：投屏时画面要占满窗口，而日志是排查时
// 才看的东西 —— 两者同时需要盯着的场合很少，挤在一起只会让画面变小、日志也
// 只有巴掌大。设置页与投屏页各留一个「查看日志」按钮，都打开这一个窗口。

import AppKit

/// 日志窗口的生命周期。同一个窗口复用：再点「查看日志」只是把它提到前面，
/// 不会越点越多。
@MainActor
final class LogWindowController {
    static let shared = LogWindowController()

    private var window: NSWindow?
    private var content: LogTextView?

    func show() {
        if let existing = window {
            existing.makeKeyAndOrderFront(nil)
            return
        }

        let created = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 760, height: 460),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        created.title = "ADisplay 日志"

        let view = LogTextView(frame: NSRect(x: 0, y: 0, width: 760, height: 460))
        created.contentView = view
        // 关掉之后这个对象还要能再打开一次，所以不随窗口释放 —— 否则第二次点
        // 「查看日志」时那个引用已经是一块野内存。
        created.isReleasedWhenClosed = false
        created.center()
        created.makeKeyAndOrderFront(nil)

        view.attach()

        window = created
        content = view
    }
}

/// 日志的显示体：一个滚动区域套一个只读但可选的 NSTextView，上面一条细工具条。
///
/// 布局刻意用 autoresizing 而不是 Auto Layout：这个视图里没有需要协商的东西，
/// 用约束只会把每次窗口变化都变成一轮约束求解，而这一轮的开销正是要避免的。
final class LogTextView: NSView {

    // 文本视图自己留的上限。比 LogStore 的 500 行多一点，这样往上翻还能看到
    // 已从 LogStore 淘汰的内容；但也不能无限长，超过就从头裁掉一半。
    private static let maxLines = 1500

    private let scrollView = NSScrollView()
    private let textView = NSTextView()
    private let copyButton = NSButton()

    private var lineCount = 0

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        // 自己摆子视图，不用 autoresizing 的自动分配。
        autoresizesSubviews = false

        textView.isEditable = false
        textView.isSelectable = true
        textView.drawsBackground = true
        textView.backgroundColor = NSColor.textBackgroundColor
        textView.textColor = NSColor.labelColor
        textView.font = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        // 长行不折行：日志一行往往很长，折了反而更难读，横着滚更好。
        textView.isHorizontallyResizable = true
        textView.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude,
                                  height: CGFloat.greatestFiniteMagnitude)
        textView.textContainer?.widthTracksTextView = false
        textView.textContainer?.containerSize =
            NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)

        scrollView.documentView = textView
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = true
        scrollView.autohidesScrollers = true
        scrollView.borderType = .noBorder
        scrollView.drawsBackground = true
        scrollView.backgroundColor = NSColor.textBackgroundColor
        addSubview(scrollView)

        copyButton.title = "复制全部"
        copyButton.bezelStyle = .rounded
        copyButton.target = self
        copyButton.action = #selector(copyAll)
        copyButton.isEnabled = false
        addSubview(copyButton)
    }

    required init?(coder: NSCoder) {
        nil   // 这个视图只由代码创建
    }

    /// 接上日志源。重复调用是安全的（只会接一次）。
    func attach() {
        let store = LogStore.shared
        // 先把已有的填进来，否则窗口打开时是空的，要等下一行日志才有内容。
        let existing = store.lines
        if !existing.isEmpty {
            append(lines: existing)
        } else {
            refreshCopyButton()
        }

        store.onAppend = { [weak self] lines in
            if let self = self {
                self.append(lines: lines)
            }
        }
    }

    override func layout() {
        super.layout()
        let headerHeight: CGFloat = 36
        copyButton.frame = NSRect(x: 10, y: bounds.height - headerHeight + 7, width: 90, height: 22)
        scrollView.frame = NSRect(x: 0, y: 0, width: bounds.width,
                                  height: max(0, bounds.height - headerHeight))
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        needsLayout = true
    }

    // MARK: - 追加

    private func append(lines: [String]) {
        // 窗口关着就不做无用功。isReleasedWhenClosed = false 让视图活着，
        // 但它已经没有窗口了 —— 这正是判断依据。
        if window == nil {
            return
        }
        guard let storage = textView.textStorage else { return }

        let wasAtBottom = isScrolledToBottom()
        storage.append(NSAttributedString(string: lines.joined(separator: "\n") + "\n"))
        lineCount += lines.count
        trimIfNeeded()
        refreshCopyButton()

        if wasAtBottom {
            scrollToBottom()
        }
    }

    /// 超出上限就从头部裁掉一半。一次扫描，且只在真的超了才发生。
    private func trimIfNeeded() {
        if lineCount <= LogTextView.maxLines {
            return
        }
        guard let storage = textView.textStorage else { return }
        let text = storage.string as NSString
        let dropLines = LogTextView.maxLines / 2
        var cut = 0
        var seen = 0
        while cut < text.length && seen < dropLines {
            let range = text.range(of: "\n", options: [], range: NSRange(location: cut, length: text.length - cut))
            if range.location == NSNotFound {
                break
            }
            cut = range.location + range.length
            seen += 1
        }
        storage.deleteCharacters(in: NSRange(location: 0, length: cut))
        lineCount -= seen
    }

    private func isScrolledToBottom() -> Bool {
        guard let clip = scrollView.contentView as NSClipView? else { return true }
        let visible = clip.bounds
        let total = textView.frame.height
        return visible.maxY >= total - 24
    }

    private func scrollToBottom() {
        let end = NSRange(location: textView.string.count, length: 0)
        textView.scrollRangeToVisible(end)
    }

    private func refreshCopyButton() {
        copyButton.isEnabled = lineCount > 0
    }

    // MARK: - 复制

    @objc private func copyAll() {
        // 复制的是 LogStore 里那份完整的（上限 500 行），而不是文本视图里
        // 已经裁过、可能更旧的内容。
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(LogStore.shared.lines.joined(separator: "\n"), forType: .string)
    }
}
