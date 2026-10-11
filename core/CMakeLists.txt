# castcore —— 跨平台核心库
#
# 界面层只跟这里的 C ABI 打交道（include/adisplay/adisplay.h）。
# 分层对应文档 4.1：
#   common     基础设施：日志、配置、字节序、线程、网络工具
#   discovery  mDNS / SSDP（批次 1）
#   session    会话管理、权限（批次 1）
#   pipeline   解码 / 同步 / 渲染 / 音频 抽象（批次 2）
#   api        C ABI 门面，产出 castcore 共享库

add_subdirectory(common)
add_subdirectory(discovery)
add_subdirectory(api)

add_subdirectory(pipeline)