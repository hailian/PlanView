#include "base/packet/FrameCodec.h"

#include <algorithm>

namespace softg::packet {

namespace {

uint64_t readUint(const uint8_t* p, int n, bool bigEndian) {
    uint64_t v = 0;
    if (bigEndian)
        for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    else
        for (int i = n - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

bool startsWith(const std::vector<uint8_t>& buf, size_t pos,
                const std::vector<uint8_t>& head) {
    if (head.empty() || pos + head.size() > buf.size()) return false;
    for (size_t i = 0; i < head.size(); ++i)
        if (buf[pos + i] != head[i]) return false;
    return true;
}

} // namespace

void FrameSplitter::setConfig(const FramingConfig& config) {
    if (config.mode != cfg_.mode || config.header != cfg_.header)
        buf_.clear(); // 帧结构变化后旧缓冲不再可信
    cfg_ = config;
}

void FrameSplitter::feed(const uint8_t* data, size_t len,
                         std::vector<std::vector<uint8_t>>& out) {
    buf_.insert(buf_.end(), data, data + len);
    while (tryExtract(out)) {
    }
    // 缓冲上限保护：防止失步且无法同步时无限增长
    size_t cap = (size_t)cfg_.maxFrameLen * 2 + 1024;
    if (buf_.size() > cap)
        buf_.erase(buf_.begin(), buf_.end() - cap);
}

// 丢弃缓冲头部以重新对齐；返回丢弃字节数（0 = 缓冲整体是帧头前缀，需等更多数据）
size_t FrameSplitter::resync() {
    if (cfg_.mode == FrameMode::HeaderLength && !cfg_.header.empty()) {
        // 先找下一个完整帧头位置
        for (size_t i = 1; i < buf_.size(); ++i) {
            if (startsWith(buf_, i, cfg_.header)) {
                buf_.erase(buf_.begin(), buf_.begin() + i);
                return i;
            }
        }
        // 没有完整帧头：保留末尾可能构成帧头前缀的最长部分
        size_t maxKeep = std::min(buf_.size(), cfg_.header.size() - 1);
        for (size_t keep = maxKeep; keep >= 1; --keep) {
            if (std::equal(buf_.end() - keep, buf_.end(), cfg_.header.begin())) {
                size_t discard = buf_.size() - keep;
                buf_.erase(buf_.begin(), buf_.begin() + discard);
                return discard;
            }
        }
        buf_.clear(); // 连帧头前缀都不是
        return 1;     // 视为有进展（缓冲已清空）
    }
    buf_.erase(buf_.begin()); // TLV 无同步特征，逐字节滑动
    return 1;
}

bool FrameSplitter::tryExtract(std::vector<std::vector<uint8_t>>& out) {
    if (cfg_.mode == FrameMode::Tlv) {
        int head = cfg_.tagBytes + cfg_.lenBytes;
        if (cfg_.tagBytes < 1 || cfg_.tagBytes > 4 || cfg_.lenBytes < 1 || cfg_.lenBytes > 4)
            return false; // 参数非法，不消费
        if ((int)buf_.size() < head)
            return false;
        uint64_t len = readUint(buf_.data() + cfg_.tagBytes, cfg_.lenBytes, cfg_.bigEndian);
        if (cfg_.lenIncludesHeader)
            len = len > (uint64_t)head ? len - head : 0;
        if (len > (uint64_t)cfg_.maxFrameLen) {
            resync();
            return true; // 已消费 1 字节，继续尝试
        }
        if ((int)buf_.size() < head + (int)len)
            return false; // 不完整，等更多数据
        out.emplace_back(buf_.begin(), buf_.begin() + head + (int)len);
        buf_.erase(buf_.begin(), buf_.begin() + head + (int)len);
        return true;
    }

    // ---- 帧头+Length ----
    if (buf_.empty())
        return false;
    const int lenFieldEnd = cfg_.lenOffset + cfg_.lenBytesHeader;
    if (cfg_.lenOffset < 0 || cfg_.lenBytesHeader < 1 || cfg_.lenBytesHeader > 4 ||
        cfg_.lenOffset > (int)cfg_.header.size())
        return false; // 参数非法
    if (!startsWith(buf_, 0, cfg_.header)) {
        size_t discarded = resync();
        return discarded > 0; // 0 = 缓冲是帧头前缀，等更多数据
    }
    if (buf_.size() < (size_t)lenFieldEnd)
        return false; // 帧头在但 length 字段未收全
    uint64_t len = readUint(buf_.data() + cfg_.lenOffset, cfg_.lenBytesHeader,
                            cfg_.bigEndianHeader);
    uint64_t total = cfg_.lenIncludesAll
                         ? len
                         : (uint64_t)lenFieldEnd + len; // length 只计负载
    if (total < (uint64_t)lenFieldEnd || total > (uint64_t)cfg_.maxFrameLen) {
        resync();
        return true;
    }
    if (buf_.size() < total)
        return false;
    out.emplace_back(buf_.begin(), buf_.begin() + (int)total);
    buf_.erase(buf_.begin(), buf_.begin() + (int)total);
    return true;
}

bool decodeFrameOnce(const FramingConfig& cfg, const std::vector<uint8_t>& frame,
                     int64_t& tagId, const uint8_t*& payload, int& payloadLen) {
    if (cfg.mode == FrameMode::Tlv) {
        int head = cfg.tagBytes + cfg.lenBytes;
        if (cfg.tagBytes < 1 || cfg.tagBytes > 4 || cfg.lenBytes < 1 || cfg.lenBytes > 4)
            return false;
        if ((int)frame.size() < head) return false;
        uint64_t len = readUint(frame.data() + cfg.tagBytes, cfg.lenBytes, cfg.bigEndian);
        if (cfg.lenIncludesHeader)
            len = len > (uint64_t)head ? len - head : 0;
        if (len > (uint64_t)cfg.maxFrameLen) return false;
        if ((int)frame.size() < head + (int)len) return false;
        tagId = (int64_t)readUint(frame.data(), cfg.tagBytes, cfg.bigEndian);
        payload = frame.data() + head;
        payloadLen = (int)len;
        return true;
    }

    // 帧头+Length：帧头须匹配，字段偏移相对整帧
    if (frame.size() < cfg.header.size()) return false;
    for (size_t i = 0; i < cfg.header.size(); ++i)
        if (frame[i] != cfg.header[i]) return false;
    tagId = -1;
    payload = frame.data();
    payloadLen = (int)frame.size();
    return true;
}

} // namespace softg::packet
