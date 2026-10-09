// ADisplay —— AirPlay 镜像帧规范化的测试
//
// 守的是这次修的那个故障：macOS 上「帧一直在计数、屏幕始终全黑」。
//
// 根因不在解密、也不在解码，而在**参数集只挂在第一帧上**。协议层
// （UxPlay raop_rtp_mirror.c）把明文 codec 包里的 SPS/PPS 拼到紧随其后的那一帧
// 前面，拼完就把标志清掉；此后只有切分辨率或暂停恢复才会再带一次。而界面层的
// 帧回调与会话回调不在同一个线程上 —— 会话回调要排队回主线程才能把渲染面建出来
// 并挂上，帧回调却是立刻送达的。第一帧就这样被丢在「还没有落点」的时候，
// 参数集从此再也不来，解码配置永远建不出来。
//
// 这个故障最难的地方是它**完全静默**：帧的计数在核心里涨，日志里一行异常都没有。
// 所以这里既钉住切分逻辑，也钉住「晚挂上来也能拿到参数集」这条行为。
//
// 文件里的 SPS/PPS 字节取自 2026-10-08 一份 iPhone 17 Pro（iPhone18,1）
// 镜像会话的真实日志，不是编的。

#include "MirrorNalu.h"

#include <cstdint>
#include <vector>

#include "AdTest.h"

namespace {

namespace airplay = adisplay::airplay;

using Bytes = std::vector<uint8_t>;
using Unit = airplay::NaluUnit;

// 真实日志里的参数集：
//   raop_rtp_mirror SPS NAL size = 18
//   27 64 00 1f ac 13 14 50 20 02 27 88 96 6e 02 1a 02 04
//   raop_rtp_mirror PPS NAL size = 4
//   28 ee 3c b0
// 注意它们**不带起始码** —— 协议层是在拼给下一帧时才加上的。
const Bytes kSps = {0x27, 0x64, 0x00, 0x1f, 0xac, 0x13, 0x14, 0x50, 0x20,
                    0x02, 0x27, 0x88, 0x96, 0x6e, 0x02, 0x1a, 0x02, 0x04};
const Bytes kPps = {0x28, 0xee, 0x3c, 0xb0};

const Bytes kStartCode = {0x00, 0x00, 0x00, 0x01};

// H.264 的 NAL 头：低 5 位是类型，高位随便给一个合法值。
Unit h264_unit(int type, std::size_t payload_len) {
    Unit unit;
    unit.push_back(static_cast<uint8_t>(0x60 | (type & 0x1F)));
    for (std::size_t i = 0; i < payload_len; ++i) {
        unit.push_back(static_cast<uint8_t>(i + 1));
    }
    return unit;
}

// H.265 的 NAL 头：类型在 bit 1..6。
Unit h265_unit(int type, std::size_t payload_len) {
    Unit unit;
    unit.push_back(static_cast<uint8_t>((type & 0x3F) << 1));
    for (std::size_t i = 0; i < payload_len; ++i) {
        unit.push_back(static_cast<uint8_t>(i + 1));
    }
    return unit;
}

Bytes append(Bytes out, const Bytes& part) {
    out.insert(out.end(), part.begin(), part.end());
    return out;
}

// 按 Annex B 手工拼一帧。刻意不复用被测的 join_annex_b ——
// 用它来造输入会让用例变成自证。
Bytes frame_of(const std::vector<Bytes>& units) {
    Bytes out;
    for (const Bytes& unit : units) {
        out = append(out, kStartCode);
        out = append(out, unit);
    }
    return out;
}

// 一帧里某个字节序列出现了几次。用来确认参数集没有被补成两份。
std::size_t count_occurrences(const Bytes& haystack, const Bytes& needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return 0;
    }
    std::size_t count = 0;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool same = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (haystack[i + j] != needle[j]) {
                same = false;
                break;
            }
        }
        if (same) {
            ++count;
        }
    }
    return count;
}

// 首帧的真实形态：起始码 + SPS + 起始码 + PPS + 起始码 + IDR。
Bytes first_frame() {
    return frame_of({kSps, kPps, h264_unit(5, 32)});
}

// 之后的关键帧：只有 IDR，不带参数集。
Bytes later_keyframe() {
    return frame_of({h264_unit(5, 48)});
}

// 非关键帧：只有 type 1 的片子。
Bytes non_keyframe() {
    return frame_of({h264_unit(1, 24)});
}

AD_TEST(airplay_mirror_split_real_first_frame, "镜像首帧按起始码切成三段，类型 7/8/5") {
    const Bytes frame = first_frame();
    const std::vector<Unit> units = airplay::split_annex_b(frame.data(), frame.size());

    AD_CHECK_EQ(units.size(), static_cast<std::size_t>(3));
    if (units.size() != 3) {
        return;
    }
    // 单元不含起始码，第一个字节就是 NAL 头。
    AD_CHECK_EQ(static_cast<int>(units[0][0] & 0x1F), 7);
    AD_CHECK_EQ(static_cast<int>(units[1][0] & 0x1F), 8);
    AD_CHECK_EQ(static_cast<int>(units[2][0] & 0x1F), 5);
    // 与日志里的字节逐字节相同。
    AD_CHECK_EQ(units[0], kSps);
    AD_CHECK_EQ(units[1], kPps);
}

AD_TEST(airplay_mirror_split_handles_three_byte_start_code, "三字节起始码同样能切，尾部填充零不算内容") {
    Bytes frame = {0x00, 0x00, 0x01};
    const Unit unit = h264_unit(1, 8);
    frame = append(frame, unit);
    // 下一个起始码前面的填充零，不能算进上一个单元。
    frame = append(frame, Bytes{0x00, 0x00, 0x00});
    frame = append(frame, Bytes{0x00, 0x00, 0x01});
    frame = append(frame, h264_unit(1, 4));

    const std::vector<Unit> units = airplay::split_annex_b(frame.data(), frame.size());
    AD_CHECK_EQ(units.size(), static_cast<std::size_t>(2));
    if (units.size() != 2) {
        return;
    }
    AD_CHECK_EQ(units[0], unit);
}

AD_TEST(airplay_mirror_keyframe_detection, "关键帧判定：IDR 是，普通片子不是") {
    const std::vector<Unit> key = airplay::split_annex_b(first_frame().data(), first_frame().size());
    AD_CHECK(airplay::contains_keyframe(key, false));

    const Bytes plain = non_keyframe();
    const std::vector<Unit> plain_units = airplay::split_annex_b(plain.data(), plain.size());
    AD_CHECK(!airplay::contains_keyframe(plain_units, false));
}

AD_TEST(airplay_mirror_first_frame_passes_through_once, "首帧自带参数集，原样通过且不补成两份") {
    airplay::MirrorNaluNormalizer normalizer;
    const Bytes frame = first_frame();
    const Bytes out = normalizer.normalize(frame.data(), frame.size(), false);

    AD_CHECK_EQ(out, frame);
    // 关键：SPS 只出现一次，没有在前面又补一份。
    AD_CHECK_EQ(count_occurrences(out, kSps), static_cast<std::size_t>(1));
    AD_CHECK_EQ(normalizer.cached_parameter_set_count(), static_cast<std::size_t>(2));
    AD_CHECK(!normalizer.last_frame_had_prepended_sets());
}

// 这次故障的正面复现：第一帧在渲染面挂上来之前就到了、被丢掉，
// 于是从第二帧开始才有落点。参数集必须还能拿到。
AD_TEST(airplay_mirror_late_sink_still_gets_parameter_sets,
         "第一帧被丢弃后，后续关键帧仍会补上参数集") {
    airplay::MirrorNaluNormalizer normalizer;

    // 关键在分工：核心照常规范化**每一帧**，「这一帧没人接」发生在界面层，
    // 不在核心。所以第一帧虽然被渲染面丢了，它的参数集已经进了缓存。
    const Bytes frame = first_frame();
    normalizer.normalize(frame.data(), frame.size(), false);

    const Bytes key = later_keyframe();
    const Bytes out = normalizer.normalize(key.data(), key.size(), false);

    AD_CHECK(normalizer.last_frame_had_prepended_sets());
    // 输出 = 起始码+SPS+起始码+PPS + 原本那一帧。
    const Bytes expected_head = frame_of({kSps, kPps});
    AD_CHECK_EQ(out.size(), expected_head.size() + key.size());
    if (out.size() == expected_head.size() + key.size()) {
        const Bytes head(out.begin(), out.begin() + static_cast<long>(expected_head.size()));
        AD_CHECK_EQ(head, expected_head);
        const Bytes tail(out.begin() + static_cast<long>(expected_head.size()), out.end());
        AD_CHECK_EQ(tail, key);
    }
}

AD_TEST(airplay_mirror_non_keyframe_not_padded, "非关键帧不补参数集（补了也解不出来，白占带宽）") {
    airplay::MirrorNaluNormalizer normalizer;
    const Bytes frame = first_frame();
    normalizer.normalize(frame.data(), frame.size(), false);

    const Bytes plain = non_keyframe();
    const Bytes out = normalizer.normalize(plain.data(), plain.size(), false);

    AD_CHECK_EQ(out, plain);
    AD_CHECK(!normalizer.last_frame_had_prepended_sets());
}

AD_TEST(airplay_mirror_incomplete_parameter_sets_not_cached, "残缺的参数集不入缓存") {
    airplay::MirrorNaluNormalizer normalizer;

    // 只有 SPS、没有 PPS：这一套建不出解码配置，当成没有。
    const Bytes half = frame_of({kSps, h264_unit(5, 16)});
    const std::vector<Unit> half_units = airplay::split_annex_b(half.data(), half.size());
    AD_CHECK(airplay::select_parameter_sets(half_units, false).empty());

    normalizer.normalize(half.data(), half.size(), false);
    AD_CHECK_EQ(normalizer.cached_parameter_set_count(), static_cast<std::size_t>(0));

    const Bytes key = later_keyframe();
    const Bytes out = normalizer.normalize(key.data(), key.size(), false);
    AD_CHECK_EQ(out, key);   // 手上没有完整的一套，不补
}

AD_TEST(airplay_mirror_h265_parameter_sets, "H.265 要 VPS/SPS/PPS 三个，IDR 认 19/20") {
    const Bytes vps = h265_unit(32, 12);
    const Bytes sps = h265_unit(33, 14);
    const Bytes pps = h265_unit(34, 6);
    const Bytes idr = h265_unit(19, 20);

    const Bytes frame = frame_of({vps, sps, pps, idr});
    const std::vector<Unit> units = airplay::split_annex_b(frame.data(), frame.size());
    AD_CHECK_EQ(units.size(), static_cast<std::size_t>(4));

    const std::vector<Unit> sets = airplay::select_parameter_sets(units, true);
    AD_CHECK_EQ(sets.size(), static_cast<std::size_t>(3));
    AD_CHECK(airplay::contains_keyframe(units, true));
    // 同一个字节按 H.264 解释就不是关键帧了 —— 编解码器必须分清。
    AD_CHECK(!airplay::contains_keyframe(units, false));
}

AD_TEST(airplay_mirror_codec_switch_clears_cache, "编码器切换时旧参数集作废") {
    airplay::MirrorNaluNormalizer normalizer;
    const Bytes frame = first_frame();
    normalizer.normalize(frame.data(), frame.size(), false);
    AD_CHECK_EQ(normalizer.cached_parameter_set_count(), static_cast<std::size_t>(2));

    // 换成 H.265 的一帧：H.264 那套参数集对新编码器毫无意义。
    const Bytes h265_frame = frame_of({h265_unit(19, 10)});
    normalizer.normalize(h265_frame.data(), h265_frame.size(), true);
    AD_CHECK_EQ(normalizer.cached_parameter_set_count(), static_cast<std::size_t>(0));
}

AD_TEST(airplay_mirror_reset_clears_cache, "复位后缓存清空") {
    airplay::MirrorNaluNormalizer normalizer;
    const Bytes frame = first_frame();
    normalizer.normalize(frame.data(), frame.size(), false);
    normalizer.reset();
    AD_CHECK_EQ(normalizer.cached_parameter_set_count(), static_cast<std::size_t>(0));
}

AD_TEST(airplay_mirror_degenerate_input, "空输入、无起始码、空指针都返回空") {
    airplay::MirrorNaluNormalizer normalizer;

    AD_CHECK(airplay::split_annex_b(nullptr, 0).empty());
    AD_CHECK(normalizer.normalize(nullptr, 0, false).empty());

    const Bytes junk = {0x11, 0x22, 0x33, 0x44, 0x55};
    AD_CHECK(airplay::split_annex_b(junk.data(), junk.size()).empty());
    AD_CHECK(normalizer.normalize(junk.data(), junk.size(), false).empty());

    // 只有一个起始码、后面什么都没有。
    const Bytes lonely = {0x00, 0x00, 0x00, 0x01};
    AD_CHECK(airplay::split_annex_b(lonely.data(), lonely.size()).empty());
}

}  // namespace

int main() {
    return adtest::run_all("AirPlay 镜像帧规范化测试");
}
