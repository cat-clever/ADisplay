// ADisplay —— 远端拉取（HTTP 客户端）
//
// 中转要用到两种拉取：播放列表正文（小，几 KB）与 TS 分片（几百 KB）。
// 两者都在各自的线程里同步等结果，所以超时必须是硬性的 —— 远端卡住时我们
// 宁可明确失败并记日志，也不能让播放器的请求无限期挂着。
#pragma once

#include <string>

namespace adisplay::pipeline {

struct FetchResult {
    bool ok = false;    // HTTP 200 才算成功
    int status = 0;     // HTTP 状态码；没拿到响应时为 0
    std::string body;
    std::string error;  // 失败原因（中文，供日志与界面显示）
};

// 拉一个远端地址。timeout_seconds 对连接与读取都生效。
FetchResult fetch_url(const std::string& url, int timeout_seconds);

}  // namespace adisplay::pipeline
