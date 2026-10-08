// ADisplay —— DLNA 媒体地址的规范化
//
// 单独成文件是为了能被单元测试直接覆盖：这段逻辑只有几十行，但踩错一次
// 就表现为「手机显示投屏中、接收端没画面」，而且日志里看不出所以然。
#pragma once

#include <string>

namespace adisplay::dlna::media_url {

// 手机给的地址里若出现回环地址（127.x / localhost / [::1]），换成发送端的地址。
//
// 为什么需要这个：部分国产投屏 App 会在自己手机上跑一个本地 HTTP 代理
// （转码、解密、鉴权都在里面），然后把 http://127.0.0.1:7000/xxx 这样的地址
// 推给接收端。对接收端来说 127.0.0.1 是它自己，自然拉不到 —— 报出来就是
// 「You do not have permission to access the requested resource」。
//
// 这些 App 是故意这么发的：真机电视固件普遍会把回环地址改写成发送端 IP，
// 它们依赖这个行为。所以这不是它们的 bug，是接收端该做的一步。
//
// peer_address 为空（拿不到发送端地址）时原样返回，不做猜测。
// 端口、查询串、路径都保持不变，只换主机名。
std::string rewrite_loopback(const std::string& url, const std::string& peer_address);

}  // namespace adisplay::dlna::media_url
