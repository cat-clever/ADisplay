#include "MirrorNalu.h"

namespace adisplay {
namespace airplay {
namespace {

// 切分时记两列：起始码自己的下标、以及内容起点。这样 4 字节起始码前面多出来的
// 那个 0 不会被算进上一个单元。
struct StartCodeMark {
    std::size_t prefix;
    std::size_t content;
};

bool start_code_at(const uint8_t* data, std::size_t size, std::size_t index, std::size_t* length) {
    if (index + 3 > size) {
        return false;
    }
    if (data[index] != 0 || data[index + 1] != 0) {
        return false;
    }
    if (data[index + 2] == 1) {
        *length = 3;
        return true;
    }
    if (index + 4 <= size && data[index + 2] == 0 && data[index + 3] == 1) {
        *length = 4;
        return true;
    }
    return false;
}

int nal_type_of(const NaluUnit& unit, bool is_h265) {
    if (unit.empty()) {
        return -1;
    }
    if (is_h265) {
        return (static_cast<int>(unit[0]) >> 1) & 0x3F;
    }
    return static_cast<int>(unit[0]) & 0x1F;
}

}  // namespace

std::vector<NaluUnit> split_annex_b(const uint8_t* data, std::size_t size) {
    std::vector<NaluUnit> units;
    if (data == nullptr || size == 0) {
        return units;
    }

    std::vector<StartCodeMark> marks;
    std::size_t index = 0;
    while (index + 3 <= size) {
        std::size_t length = 0;
        if (start_code_at(data, size, index, &length)) {
            marks.push_back(StartCodeMark{index, index + length});
            index += length;
            continue;
        }
        ++index;
    }
    if (marks.empty()) {
        return units;
    }

    units.reserve(marks.size());
    for (std::size_t position = 0; position < marks.size(); ++position) {
        std::size_t end = (position + 1 < marks.size()) ? marks[position + 1].prefix : size;
        // 单元末尾挂着的零是 Annex B 的填充（trailing_zero_8bits），不属于 NAL。
        while (end > marks[position].content && data[end - 1] == 0) {
            --end;
        }
        if (end <= marks[position].content) {
            continue;
        }
        units.emplace_back(data + marks[position].content, data + end);
    }
    return units;
}

std::vector<NaluUnit> select_parameter_sets(const std::vector<NaluUnit>& units, bool is_h265) {
    std::vector<NaluUnit> found;
    bool has_sps = false;
    bool has_pps = false;
    bool has_vps = false;

    for (const NaluUnit& unit : units) {
        const int type = nal_type_of(unit, is_h265);
        if (is_h265) {
            if (type == 32) {
                has_vps = true;
                found.push_back(unit);
            } else if (type == 33) {
                has_sps = true;
                found.push_back(unit);
            } else if (type == 34) {
                has_pps = true;
                found.push_back(unit);
            }
        } else {
            if (type == 7) {
                has_sps = true;
                found.push_back(unit);
            } else if (type == 8) {
                has_pps = true;
                found.push_back(unit);
            }
        }
    }

    const bool complete = is_h265 ? (has_vps && has_sps && has_pps) : (has_sps && has_pps);
    if (!complete) {
        return {};
    }
    return found;
}

bool contains_keyframe(const std::vector<NaluUnit>& units, bool is_h265) {
    for (const NaluUnit& unit : units) {
        const int type = nal_type_of(unit, is_h265);
        if (is_h265) {
            // IDR_W_RADL / IDR_N_LP。CRA(21) 也算随机访问点，但镜像流里不出现。
            if (type == 19 || type == 20) {
                return true;
            }
        } else if (type == 5) {
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> join_annex_b(const std::vector<NaluUnit>& units) {
    std::size_t total = 0;
    for (const NaluUnit& unit : units) {
        total += unit.size() + 4;
    }

    std::vector<uint8_t> out;
    out.reserve(total);
    for (const NaluUnit& unit : units) {
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        out.push_back(1);
        out.insert(out.end(), unit.begin(), unit.end());
    }
    return out;
}

void MirrorNaluNormalizer::reset() {
    parameter_sets_.clear();
    codec_known_ = false;
    last_prepended_ = false;
    last_keyframe_ = false;
}

std::vector<uint8_t> MirrorNaluNormalizer::normalize(const uint8_t* data, std::size_t size, bool is_h265) {
    last_prepended_ = false;
    last_keyframe_ = false;
    if (data == nullptr || size == 0) {
        return {};
    }

    if (!codec_known_ || is_h265 != is_h265_) {
        // 编码器换了，旧的参数集整套作废。
        parameter_sets_.clear();
        is_h265_ = is_h265;
        codec_known_ = true;
    }

    const std::vector<NaluUnit> units = split_annex_b(data, size);
    if (units.empty()) {
        return {};
    }

    last_keyframe_ = contains_keyframe(units, is_h265);

    const std::vector<NaluUnit> in_frame = select_parameter_sets(units, is_h265);
    if (!in_frame.empty()) {
        parameter_sets_ = in_frame;
    }

    // 帧自带参数集时以帧为准（上面已经存下了）。只有帧里没有、手上有缓存、
    // 而且这是个关键帧时才补 —— 补在非关键帧上解不出来，白占带宽。
    const bool prepend = in_frame.empty() && !parameter_sets_.empty() && last_keyframe_;
    if (!prepend) {
        return join_annex_b(units);
    }

    std::vector<NaluUnit> merged;
    merged.reserve(parameter_sets_.size() + units.size());
    merged.insert(merged.end(), parameter_sets_.begin(), parameter_sets_.end());
    merged.insert(merged.end(), units.begin(), units.end());
    last_prepended_ = true;
    return join_annex_b(merged);
}

}  // namespace airplay
}  // namespace adisplay
