// ADisplay —— macOS 应用入口
//
// 文档 2.3 要求菜单栏常驻（NSStatusItem）。批次 0 先把主窗口和核心链路跑通，
// 菜单栏图标随批次 6「体验与安全」一起加。

import SwiftUI

@main
struct ADisplayApp: App {

    @StateObject private var model = EngineModel()

    var body: some Scene {
        WindowGroup("ADisplay 投屏接收端") {
            ContentView()
                .environmentObject(model)
                .onAppear {
                    model.create()
                }
        }
    }
}
