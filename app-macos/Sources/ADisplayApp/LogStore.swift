// ADisplay —— 日志的存放处
//
// 刻意不放进 EngineModel。
//
// EngineModel 是投屏页与设置页共同观察的对象（@Published）。日志写在它上面时，
// 每来一行都会让所有观察它的视图重算一遍 —— 而协议层一秒能冒出十几行。日志一多，
// 投屏页的重排就成了常态，全屏切换这类需要连贯动画的操作会被拖住，CPU 也跟着上去。
// 一行数上限救不了这个：代价在「每次都通知整个界面」，不在行数。
//
// 日志实际上只有日志窗口在看，所以让它自己成为一个可观察对象。写日志不再牵动投屏页。

import Foundation
import SwiftUI

final class LogStore: ObservableObject {

    static let shared = LogStore()

    /// 只留最近这些行。长时间挂着接收服务时，日志窗口的重排代价跟着行数走。
    static let maxLines = 500

    /// 合并发布的时间窗。协议层一秒能冒十几行，逐行发布就是十几次窗口重排。
    private static let flushDelay: TimeInterval = 0.15

    // 每行都新建一个 DateFormatter 是这条路上最贵的一步，复用同一个。
    private static let formatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        return formatter
    }()

    @Published private(set) var lines: [String] = []

    private var pending: [String] = []
    private var flushScheduled = false

    private init() {}

    /// 记一行。可以在任意线程调用 —— 内部会切回主线程再改 @Published。
    func append(level: LogLevel, text: String) {
        if Thread.isMainThread {
            enqueue(level: level, text: text)
            return
        }
        DispatchQueue.main.async { [weak self] in
            if let self = self {
                self.enqueue(level: level, text: text)
            }
        }
    }

    func clear() {
        if !Thread.isMainThread {
            DispatchQueue.main.async { [weak self] in
                if let self = self {
                    self.clear()
                }
            }
            return
        }
        pending.removeAll()
        lines.removeAll()
    }

    private func enqueue(level: LogLevel, text: String) {
        let stamp = LogStore.formatter.string(from: Date())
        pending.append("[" + stamp + "] [" + level.label + "] " + text)
        scheduleFlush()
    }

    private func scheduleFlush() {
        if flushScheduled {
            return
        }
        flushScheduled = true
        // 攒到下一拍一次性发布。代价是日志最多晚 flushDelay 出现 ——
        // 排查时看的仍然是最后几行，这个延迟感觉不到。
        DispatchQueue.main.asyncAfter(deadline: .now() + LogStore.flushDelay) { [weak self] in
            if let self = self {
                self.flush()
            }
        }
    }

    private func flush() {
        flushScheduled = false
        if pending.isEmpty {
            return
        }
        lines.append(contentsOf: pending)
        pending.removeAll(keepingCapacity: true)
        if lines.count > LogStore.maxLines {
            lines.removeFirst(lines.count - LogStore.maxLines)
        }
    }
}
