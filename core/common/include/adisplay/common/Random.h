// ADisplay —— 随机数
//
// deviceid / UUID 的生成，以及 AirPlay 配对阶段的临时密钥（文档 3.1.2），
// 都必须用操作系统提供的密码学安全随机源，不能用 rand()。
//
// 各平台后端：
//   Windows  BCryptGenRandom
//   macOS    arc4random_buf（内核 CSPRNG）
//   Linux    getrandom(2)，不可用时退回 /dev/urandom
#pragma once

#include <cstddef>
#include <cstdint>

namespace adisplay::common {

// 填充 count 个随机字节。out 不得为 nullptr。
// 随机源不可用时（极罕见）返回 false 并把缓冲区填零 ——
// 调用方必须检查返回值，不要拿零字节当密钥用。
bool random_bytes(uint8_t* out, std::size_t count);

// 生成 [0, bound) 上的均匀随机整数。bound 为 0 时返回 0。
// 内部用拒绝采样，避免取模引入的偏斜。
uint32_t random_below(uint32_t bound);

}  // namespace adisplay::common
