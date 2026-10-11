// ADisplay —— 本地中转：把 HEVC-in-TS 的 HLS 边下边换成 fMP4
//
// 要解决的问题：手机投屏 App 推过来的是 HEVC-in-TS 的 HLS —— 它自己在手机上
// 跑了个带签名的本地代理，播放列表里是 output_000000.ts?m8=... 这样的分片。
// Apple 的 HLS 规范要求 HEVC 用 fMP4（CMAF）封装，AVPlayer 放不了 HEVC-in-TS：
// 播放列表能解析、时长能算出来（所以进度条能拖）、有声音，但视频轨被丢掉，
// 没有画面。验证分片内容确实是 hevc Main + aac。
//
// 办法：在本机起一个 HTTP 服务，把源分片边下边换封装成 fMP4（HEVC 直通，
// 不重编码），以本地 HLS 的形式喂给播放器。
//
// 一条硬约束：这个类必须是「按需介入」的。DLNA 的 SetAVTransportURI 要立刻
// 返回 —— 在里面做远端抓取会拖垮 SOAP 应答，手机判超时之后直接断开。所以
// resolve_for_playback() 只拼一个本地地址、不发任何网络请求；真正的远端工作
// 全部发生在播放器来请求的时候。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace adisplay::pipeline {

class MediaRelay {
public:
    MediaRelay();
    ~MediaRelay();

    MediaRelay(const MediaRelay&) = delete;
    MediaRelay& operator=(const MediaRelay&) = delete;

    // 起本地 HTTP 服务，监听 127.0.0.1 上由系统分配的临时端口。
    // 幂等：已经在跑就直接返回 true。
    bool start(std::string* out_error);

    // 停止并释放当前会话。幂等，可以反复调用；在等的分片请求会被唤醒并失败返回。
    void stop();

    bool is_running() const;

    // 给出「该交给播放器的地址」。**立刻返回，不发任何网络请求。**
    //   HLS（http）→ http://127.0.0.1:<端口>/local.m3u8?src=<远端地址>
    //   其余（非 HLS、https、服务没起来）→ 原样返回远端地址
    // 这一刻还判断不出要不要换封装（要拉了播放列表才知道分片是 TS 还是 fMP4），
    // 所以这一步不做判断：判断错一次，要么本来能直接播的片源白绕一层，要么
    // 该换的没换、用户看到黑屏。
    std::string resolve_for_playback(const std::string& remote_url) const;

    uint16_t port() const;           // 未启动返回 0
    bool is_relaying() const;        // 当前是否有活跃的中转会话
    std::string last_error() const;  // 最近一次失败原因，供日志与界面显示

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace adisplay::pipeline
