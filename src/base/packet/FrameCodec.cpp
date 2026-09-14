#include "base/packet/FrameCodec.h"

#include <algorithm>

namespace pv::packet {

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
    if (config.mode != cfg_.mode || config.header != cfg_.header) {
        buf_.clear(); // 帧结构变化后旧缓冲不再可信
        head_ = 0;
    }
    cfg_ = config;
    cfgs_.clear(); // 回到单配置模式
}

void FrameSplitter::setConfigs(const std::vector<FramingConfig>& configs) {
    buf_.clear();
    head_ = 0;
    cfgs_ = configs;
    if (!cfgs_.empty()) cfg_ = cfgs_.front(); // 主配置 = 首条（报文监视等兼容读）
}

void FrameSplitter::feed(const uint8_t* data, size_t len,
                         std::vector<std::vector<uint8_t>>& out) {
    buf_.insert(buf_.end(), data, data + len);
    while (tryExtract(out)) {
    }
    // 收尾一次性前移已消费前缀：取帧期间只推进 head_，不做逐帧 memmove
    if (head_ > 0) {
        buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)head_);
        head_ = 0;
    }
    // 缓冲上限保护：防止失步且无法同步时无限增长
    int maxLen = cfg_.maxFrameLen;
    for (const auto& c : cfgs_) maxLen = std::max(maxLen, c.maxFrameLen);
    size_t cap = (size_t)maxLen * 2 + 1024;
    if (buf_.size() > cap)
        buf_.erase(buf_.begin(), buf_.end() - (std::ptrdiff_t)cap);
}

// 丢弃已消费头部之后的数据以重新对齐；返回丢弃字节数
//（0 = 缓冲整体是帧头前缀，需等更多数据）
size_t FrameSplitter::resync() {
    if (cfg_.mode == FrameMode::HeaderLength && !cfg_.header.empty()) {
        // 先找下一个完整帧头位置
        for (size_t i = head_ + 1; i < buf_.size(); ++i) {
            if (startsWith(buf_, i, cfg_.header)) {
                size_t discarded = i - head_;
                head_ = i;
                return discarded;
            }
        }
        // 没有完整帧头：保留末尾可能构成帧头前缀的最长部分
        size_t avail = buf_.size() - head_;
        size_t maxKeep = std::min(avail, cfg_.header.size() - 1);
        for (size_t keep = maxKeep; keep >= 1; --keep) {
            if (std::equal(buf_.end() - keep, buf_.end(), cfg_.header.begin())) {
                size_t discard = avail - keep;
                head_ = buf_.size() - keep;
                return discard;
            }
        }
        head_ = buf_.size(); // 连帧头前缀都不是：全部丢弃
        return avail == 0 ? 1 : avail; // 视为有进展（已消费空）
    }
    ++head_; // TLV 无同步特征，逐字节滑动
    return 1;
}

bool FrameSplitter::tryExtract(std::vector<std::vector<uint8_t>>& out) {
    size_t avail = buf_.size() - head_; // head_ <= buf_.size() 恒成立

    // ---- 多帧头（协议组）：从头部逐帧头匹配，命中者按其 length 语义取帧 ----
    if (!cfgs_.empty()) {
        if (avail == 0) return false;
        for (const auto& cfg : cfgs_) {
            if (!startsWith(buf_, head_, cfg.header)) continue;
            const int lenFieldEnd = cfg.lenOffset + cfg.lenBytesHeader;
            if (cfg.lenOffset < 0 || cfg.lenBytesHeader < 1 || cfg.lenBytesHeader > 4 ||
                cfg.lenOffset > (int)cfg.header.size())
                continue; // 该配置非法，试下一个
            if (avail < (size_t)lenFieldEnd) return false; // 帧头命中，等长度字段
            uint64_t len = readUint(buf_.data() + head_ + cfg.lenOffset, cfg.lenBytesHeader,
                                    cfg.bigEndianHeader);
            uint64_t total = cfg.lenIncludesAll ? len : (uint64_t)lenFieldEnd + len;
            if (total < (uint64_t)lenFieldEnd || total > (uint64_t)cfg.maxFrameLen) {
                resyncMulti(); // 长度非法：按失步处理
                return true;
            }
            if (avail < total) return false; // 不完整，等更多数据
            out.emplace_back(buf_.begin() + (std::ptrdiff_t)head_,
                             buf_.begin() + (std::ptrdiff_t)(head_ + total));
            head_ += (size_t)total;
            return true;
        }
        size_t discarded = resyncMulti(); // 无帧头命中
        return discarded > 0;
    }

    if (cfg_.mode == FrameMode::Tlv) {
        int hl = cfg_.tagBytes + cfg_.lenBytes;
        if (cfg_.tagBytes < 1 || cfg_.tagBytes > 4 || cfg_.lenBytes < 1 || cfg_.lenBytes > 4)
            return false; // 参数非法，不消费
        if (avail < (size_t)hl)
            return false;
        uint64_t len = readUint(buf_.data() + head_ + cfg_.tagBytes, cfg_.lenBytes,
                                cfg_.bigEndian);
        if (cfg_.lenIncludesHeader)
            len = len > (uint64_t)hl ? len - hl : 0;
        if (len > (uint64_t)cfg_.maxFrameLen) {
            resync();
            return true; // 已消费 1 字节，继续尝试
        }
        if (avail < (size_t)hl + len)
            return false; // 不完整，等更多数据
        out.emplace_back(buf_.begin() + (std::ptrdiff_t)head_,
                         buf_.begin() + (std::ptrdiff_t)(head_ + hl + len));
        head_ += (size_t)hl + (size_t)len;
        return true;
    }

    // ---- 帧头+Length ----
    if (avail == 0)
        return false;
    const int lenFieldEnd = cfg_.lenOffset + cfg_.lenBytesHeader;
    if (cfg_.lenOffset < 0 || cfg_.lenBytesHeader < 1 || cfg_.lenBytesHeader > 4 ||
        cfg_.lenOffset > (int)cfg_.header.size())
        return false; // 参数非法
    if (!startsWith(buf_, head_, cfg_.header)) {
        size_t discarded = resync();
        return discarded > 0; // 0 = 缓冲是帧头前缀，等更多数据
    }
    if (avail < (size_t)lenFieldEnd)
        return false; // 帧头在但 length 字段未收全
    uint64_t len = readUint(buf_.data() + head_ + cfg_.lenOffset, cfg_.lenBytesHeader,
                            cfg_.bigEndianHeader);
    uint64_t total = cfg_.lenIncludesAll
                         ? len
                         : (uint64_t)lenFieldEnd + len; // length 只计负载
    if (total < (uint64_t)lenFieldEnd || total > (uint64_t)cfg_.maxFrameLen) {
        resync();
        return true;
    }
    if (avail < total)
        return false;
    out.emplace_back(buf_.begin() + (std::ptrdiff_t)head_,
                     buf_.begin() + (std::ptrdiff_t)(head_ + total));
    head_ += (size_t)total;
    return true;
}

// 多帧头重同步：向后找最近一个任意帧头的完整出现位置；
// 找不到则保留可能构成某帧头前缀的最长尾部
size_t FrameSplitter::resyncMulti() {
    for (size_t i = head_ + 1; i < buf_.size(); ++i) {
        for (const auto& cfg : cfgs_) {
            if (cfg.header.empty()) continue;
            if (startsWith(buf_, i, cfg.header)) {
                size_t discarded = i - head_;
                head_ = i;
                return discarded;
            }
        }
    }
    size_t avail = buf_.size() - head_;
    size_t maxKeep = 0;
    for (const auto& cfg : cfgs_) {
        if (cfg.header.empty()) continue;
        size_t keep = std::min(avail, cfg.header.size() - 1);
        for (; keep >= 1 && keep > maxKeep; --keep) {
            if (std::equal(buf_.end() - keep, buf_.end(), cfg.header.begin()))
                maxKeep = std::max(maxKeep, keep);
        }
    }
    if (maxKeep == 0) {
        head_ = buf_.size();
        return avail == 0 ? 1 : avail; // 保证有进展（不会死循环）
    }
    size_t discard = avail - maxKeep;
    head_ = buf_.size() - maxKeep;
    return discard == 0 ? 1 : discard; // 保证有进展（不会死循环）
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

    // 帧头+Length：帧头须匹配，字段偏移相对负载（帧头 + length 字段之后的数据区，
    // 与 length 字段语义一致；例如 AA 55|00 02|03 04 → 负载 = 03 04，偏移 0 读到 03）
    if (frame.size() < cfg.header.size()) return false;
    for (size_t i = 0; i < cfg.header.size(); ++i)
        if (frame[i] != cfg.header[i]) return false;
    const int lenFieldEnd = cfg.lenOffset + cfg.lenBytesHeader;
    if (cfg.lenOffset < 0 || cfg.lenBytesHeader < 1 || cfg.lenBytesHeader > 4)
        return false; // 参数非法
    if ((int)frame.size() < lenFieldEnd) return false;
    uint64_t len = readUint(frame.data() + cfg.lenOffset, cfg.lenBytesHeader,
                            cfg.bigEndianHeader);
    uint64_t total = cfg.lenIncludesAll
                         ? len
                         : (uint64_t)lenFieldEnd + len; // length 只计负载
    if (total < (uint64_t)lenFieldEnd || total > (uint64_t)cfg.maxFrameLen) return false;
    if ((int)frame.size() < (int)total) return false; // 帧不完整
    tagId = -1;
    payload = frame.data() + lenFieldEnd;
    payloadLen = (int)(total - (uint64_t)lenFieldEnd);
    return true;
}

} // namespace pv::packet
