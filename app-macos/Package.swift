// swift-tools-version:5.9
//
// ADisplay macOS 界面（Swift + SwiftUI / AppKit）。
// 文档 v2 第 5.2 节选型；5.1 节要求最低 macOS 12、构建 Universal 二进制。
//
// 与核心库的桥接方式：CAdDisplay 是一个 systemLibrary target，
// 它只做一件事 —— 把 include/adisplay/adisplay.h 暴露给 Swift。
// 真正链接的 castcore 由 modulemap 里的 link 指令声明，
// 库文件路径在构建时通过 -L 传给链接器（见 .github/workflows/ci.yml）。

import PackageDescription

let package = Package(
    name: "ADisplay",
    platforms: [
        .macOS(.v12)
    ],
    products: [
        .executable(name: "ADisplay", targets: ["ADisplayApp"])
    ],
    targets: [
        .systemLibrary(
            name: "CAdDisplay",
            path: "Sources/CAdDisplay"
        ),
        .executableTarget(
            name: "ADisplayApp",
            dependencies: ["CAdDisplay"],
            path: "Sources/ADisplayApp"
        )
    ]
)
