// ADisplay 图标渲染（构建期工具，不进产品代码）
//
// 图形取自 Android TV 的矢量图标与横幅：屏幕轮廓 + 底座 + 一点蓝色信号；
// 底色用横幅那块的 #101418。桌面图标必须自带底色 —— 原来那份是白描边、透明底，
// 放到浅色桌面上就是一个看不见的白图标。
//
// 每个尺寸都**原生重画**（而不是从大图缩），小尺寸才不会糊；
// 线宽与底座高度按像素下限兜住，16×16 时也不会细到看不见。
//
// 比例（正方形画布）与 Android 的矢量图标是同一套，两边各有一行
// PROPORTIONS 由 tools/icons/check_icon_consistency.py 对表 —— 改一处就要改两处。
//
// PROPORTIONS: field_inset=0.051 field_radius=0.195 screen_x=0.190 screen_y=0.264 screen_w=0.620 screen_h=0.420 screen_radius=0.042 screen_stroke=0.048 stand_x=0.420 stand_y=0.684 stand_w=0.160 stand_h=0.052 dot_x=0.500 dot_y=0.474 dot_radius=0.075
//
// 用法：swift tools/icons/make_icon.swift <输出目录>（再用 iconutil / build_ico.py 打包）

import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

func color(_ value: UInt32) -> CGColor {
    CGColor(red: CGFloat((value >> 16) & 0xFF) / 255.0,
            green: CGFloat((value >> 8) & 0xFF) / 255.0,
            blue: CGFloat(value & 0xFF) / 255.0,
            alpha: 1.0)
}

let fieldColor = color(0x101418)    // 与 TV 横幅同底
let glyphColor = color(0xFFFFFF)
let signalColor = color(0x4FC3F7)   // 与 TV 横幅同蓝

func render(size: Int) -> CGImage? {
    let s = CGFloat(size)
    guard let ctx = CGContext(data: nil, width: size, height: size,
                              bitsPerComponent: 8, bytesPerRow: 0,
                              space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
        return nil
    }
    ctx.setAllowsAntialiasing(true)
    // 换成左上角原点，下面的数字就可以照着设计稿直接写。
    ctx.translateBy(x: 0, y: s)
    ctx.scaleBy(x: 1, y: -1)

    // 底板：圆角方块，四周留一点边（图标在 Dock / 资源管理器里都不会顶到边）
    ctx.addPath(CGPath(roundedRect: CGRect(x: 0.051 * s, y: 0.051 * s,
                                           width: 0.898 * s, height: 0.898 * s),
                       cornerWidth: 0.195 * s, cornerHeight: 0.195 * s, transform: nil))
    ctx.setFillColor(fieldColor)
    ctx.fillPath()

    // 屏幕：描边圆角矩形。线宽按像素兜底，否则 16×16 时那圈框会细成一条灰线。
    let lineWidth = max(1.5, 0.048 * s)
    let screen = CGRect(x: 0.190 * s, y: 0.264 * s, width: 0.620 * s, height: 0.420 * s)
    ctx.setStrokeColor(glyphColor)
    ctx.setLineWidth(lineWidth)
    // 圆角要按「外沿半径减去半个线宽」给到描边中线，弧才对得上 Android 那边
    // 同一份比例（矢量 drawable 的描边同样是骑在中线上）。
    let cornerRadius = max(0.5, (0.042 - 0.048 / 2) * s)
    ctx.addPath(CGPath(roundedRect: screen.insetBy(dx: lineWidth / 2, dy: lineWidth / 2),
                       cornerWidth: cornerRadius, cornerHeight: cornerRadius, transform: nil))
    ctx.strokePath()

    // 底座
    let standHeight = max(2.0, 0.052 * s)
    ctx.setFillColor(glyphColor)
    ctx.fill(CGRect(x: 0.420 * s, y: 0.684 * s, width: 0.160 * s, height: standHeight))

    // 投屏信号
    let dot = max(1.0, 0.075 * s)
    ctx.setFillColor(signalColor)
    ctx.fillEllipse(in: CGRect(x: 0.5 * s - dot, y: 0.474 * s - dot,
                               width: dot * 2, height: dot * 2))

    return ctx.makeImage()
}

let outDir = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : ".")
for size in [16, 24, 32, 48, 64, 128, 256, 512, 1024] {
    guard let image = render(size: size) else {
        FileHandle.standardError.write("渲染 \(size) 失败\n".data(using: .utf8)!)
        exit(1)
    }
    let url = outDir.appendingPathComponent("icon-\(size).png")
    guard let dest = CGImageDestinationCreateWithURL(url as CFURL,
                                                     UTType.png.identifier as CFString, 1, nil) else {
        FileHandle.standardError.write("无法写 \(url.path)\n".data(using: .utf8)!)
        exit(1)
    }
    CGImageDestinationAddImage(dest, image, nil)
    CGImageDestinationFinalize(dest)
    print("icon-\(size).png")
}
