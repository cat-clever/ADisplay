# ADisplay —— 构建开关
#
# 各界面工程（WinUI 3 / SwiftUI / Compose）不走 CMake，由各自的工具链构建，
# 所以这里只管核心库与测试。

option(ADISPLAY_BUILD_TESTS "构建单元测试（ctest）" ON)

# 是否把 UxPlay 的协议层拉进来做 AirPlay 支持（文档 3.1.3）。
# 打开后 CMake 会在配置阶段 FetchContent 拉取 FDH2/UxPlay。
# 默认关闭：批次 0 尚未用到，开着会让 CI 白白多花几分钟。
option(ADISPLAY_BUNDLE_UXPLAY "拉取并链接 UxPlay 的 AirPlay 协议层" OFF)

# 关闭后核心库只依赖系统 API，用于排查第三方依赖引起的问题。
option(ADISPLAY_MINIMAL_DEPS "只构建不依赖第三方库的最小核心" OFF)

if(ADISPLAY_MINIMAL_DEPS)
    message(WARNING
        "ADISPLAY_MINIMAL_DEPS 已开启：当前实现仍然需要 spdlog/fmt/json，"
        "该开关仅为后续批次预留。")
endif()
