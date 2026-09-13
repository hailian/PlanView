// FrameCodec — TCP 字节流拆帧：TLV（T|L|V）或 固定帧头+Length 两种模式。
// UDP 为数据报天然成帧，不经本模块。
#pragma once

#include <cstdint>
#include <vector>

namespace softg::packet {

enum class FrameMode : uint8_t { Tlv, HeaderLength };

struct FramingConfig {
    FrameMode mode = FrameMode::Tlv;

    // ---- TLV 模式：帧 = T(tagBytes) | L(lenBytes) | V(L) ----
    int tagBytes = 1;                // T 字段字节数 1..4
    int lenBytes = 2;                // L 字段字节数 1..4
    bool bigEndian = true;           // 多字节字段字节序
    bool lenIncludesHeader = false;  // L 是否包含 T+L 自身

    // ---- 帧头+Length 模式：帧 = 帧头 | ... | len 字段 | 负载 ----
    std::vector<uint8_t> header = {0xAA, 0x55}; // 固定帧头
    int lenOffset = 2;               // length 字段在帧内的偏移（自帧首）
    int lenBytesHeader = 2;          // length 字段字节数 1..4
    bool bigEndianHeader = true;     // length 字段字节序
    bool lenIncludesAll = false;     // length 是否为整帧长度（含帧头）

    int maxFrameLen = 4096;          // 帧长上限保护（超出按失步丢弃）
};

// 增量拆帧器：feed 任意分段字节流，输出完整帧。内部维护接收缓冲，支持失步重同步。
class FrameSplitter {
public:
    explicit FrameSplitter(const FramingConfig& config) : cfg_(config) {}

    void setConfig(const FramingConfig& config);
    const FramingConfig& config() const { return cfg_; }

    // 追加收到的字节并尝试拆出完整帧（按到达顺序追加到 out）
    void feed(const uint8_t* data, size_t len, std::vector<std::vector<uint8_t>>& out);

    // 清空接收缓冲（连接重建时调用）
    void reset() { buf_.clear(); }

private:
    // 尝试从 buf_ 头部拆出一帧；返回 true 表示已拆出（帧存入 out）
    bool tryExtract(std::vector<std::vector<uint8_t>>& out);
    // 丢弃 buf_ 首部重同步；返回丢弃字节数（0 = 缓冲整体是帧头前缀，无进展）
    size_t resync();

    FramingConfig cfg_;
    std::vector<uint8_t> buf_;
};

// 单帧结构解析（UDP 数据报 / 已拆出的完整帧共用）：
//   TLV 模式 → tagId = 帧 T 值，payload 指向负载 V，payloadLen = L；
//   帧头+Length 模式 → tagId = -1，payload 指向整帧，payloadLen = 整帧长。
// 返回 false 表示帧不完整/非法。
bool decodeFrameOnce(const FramingConfig& cfg, const std::vector<uint8_t>& frame,
                     int64_t& tagId, const uint8_t*& payload, int& payloadLen);

} // namespace softg::packet
