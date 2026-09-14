#include "base/data/frame/TestFrameGen.h"

#include <cstring>
#include <random>

namespace pv {
namespace {

void writeUint(std::vector<uint8_t>& out, uint64_t v, int n, bool bigEndian) {
    for (int i = 0; i < n; ++i)
        out.push_back(bigEndian ? (uint8_t)(v >> (8 * (n - 1 - i))) : (uint8_t)(v >> (8 * i)));
}

// 字段值 → 字节（bytes 由类型/长度属性决定，与 protocolFramingFromComponent 同口径）
std::vector<uint8_t> randomFieldBytes(const TagField& f, std::mt19937& rng) {
    std::vector<uint8_t> b;
    switch (f.type) {
    case packet::FieldType::Bool: {
        std::uniform_int_distribution<int> d(0, 1);
        b.push_back((uint8_t)d(rng));
        break;
    }
    case packet::FieldType::U8: {
        std::uniform_int_distribution<int> d(0, 255);
        writeUint(b, (uint64_t)d(rng), 1, f.bigEndian);
        break;
    }
    case packet::FieldType::U16: case packet::FieldType::U32: {
        std::uniform_int_distribution<uint32_t> d(
            0, f.type == packet::FieldType::U16 ? 0xFFFFu : 0xFFFFFFFFu);
        writeUint(b, d(rng), f.type == packet::FieldType::U16 ? 2 : 4, f.bigEndian);
        break;
    }
    case packet::FieldType::I8: case packet::FieldType::I16:
    case packet::FieldType::I32: {
        // 常规量级（±30000）足够验证且有符号性可见，避免每次都是极端值
        std::uniform_int_distribution<int32_t> d(-30000, 30000);
        uint64_t u = (uint64_t)(int64_t)d(rng);
        writeUint(b, u, f.type == packet::FieldType::I8   ? 1
                          : f.type == packet::FieldType::I16 ? 2
                                                             : 4,
                  f.bigEndian);
        break;
    }
    case packet::FieldType::F32: {
        std::uniform_int_distribution<int> d(0, 10000); // [0,100] 两位小数
        float v = (float)(d(rng) / 100.0);
        uint32_t u;
        std::memcpy(&u, &v, 4);
        writeUint(b, u, 4, f.bigEndian);
        break;
    }
    case packet::FieldType::F64: {
        std::uniform_int_distribution<int> d(0, 10000);
        double v = d(rng) / 100.0;
        uint64_t u;
        std::memcpy(&u, &v, 8);
        writeUint(b, u, 8, f.bigEndian);
        break;
    }
    case packet::FieldType::String: { // 可打印大写字母（UTF-8 安全、肉眼可辨）
        std::uniform_int_distribution<int> d('A', 'Z');
        for (int i = 0; i < f.bytes; ++i) b.push_back((uint8_t)d(rng));
        break;
    }
    case packet::FieldType::Enum: {
        if (!f.enums.empty()) {
            std::uniform_int_distribution<size_t> d(0, f.enums.size() - 1);
            writeUint(b, (uint64_t)f.enums[d(rng)].first, f.bytes, f.bigEndian);
        } else { // 无映射表：按宽度随机
            std::uniform_int_distribution<uint32_t> d(0, (uint32_t)((1ull << (8 * f.bytes)) - 1));
            writeUint(b, d(rng), f.bytes, f.bigEndian);
        }
        break;
    }
    }
    return b;
}

// TLV：单字段成帧 T|L|V（V = offset 个 0 填充 + 值）
std::vector<uint8_t> encodeTlvFrame(const packet::FramingConfig& fr, const TagField& f,
                                    std::mt19937& rng) {
    std::vector<uint8_t> v((size_t)f.offset, 0);
    std::vector<uint8_t> val = randomFieldBytes(f, rng);
    v.insert(v.end(), val.begin(), val.end());
    std::vector<uint8_t> frame;
    writeUint(frame, (uint64_t)f.tagId, fr.tagBytes, fr.bigEndian);
    uint64_t len = v.size();
    if (fr.lenIncludesHeader) len += (uint64_t)(fr.tagBytes + fr.lenBytes);
    writeUint(frame, len, fr.lenBytes, fr.bigEndian);
    frame.insert(frame.end(), v.begin(), v.end());
    return frame;
}

// 帧头+Length：一帧含全部字段（负载按 offset 布局，空隙 0 填充）
std::vector<uint8_t> encodeHeaderLenFrame(const packet::FramingConfig& fr,
                                          const std::vector<TagField>& fields,
                                          std::mt19937& rng) {
    size_t payloadLen = 0;
    for (const auto& f : fields)
        payloadLen = std::max(payloadLen, (size_t)f.offset + (size_t)f.bytes);
    std::vector<uint8_t> payload(payloadLen, 0);
    for (const auto& f : fields) {
        std::vector<uint8_t> val = randomFieldBytes(f, rng);
        std::memcpy(payload.data() + f.offset, val.data(), val.size());
    }
    const int lenFieldEnd = fr.lenOffset + fr.lenBytesHeader;
    std::vector<uint8_t> frame = fr.header;
    frame.resize((size_t)fr.lenOffset, 0); // 帧头与 length 字段间的填充
    uint64_t len = fr.lenIncludesAll
                       ? (uint64_t)lenFieldEnd + payloadLen
                       : (uint64_t)payloadLen;
    writeUint(frame, len, fr.lenBytesHeader, fr.bigEndianHeader);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

} // namespace

std::vector<std::vector<uint8_t>> generateTestFrames(const packet::FramingConfig& fr,
                                                     const std::vector<TagField>& fields,
                                                     int count, uint32_t seed) {
    std::vector<std::vector<uint8_t>> out;
    if (fields.empty() || count <= 0) return out;
    std::mt19937 rng(seed ? seed : std::random_device{}());
    for (int i = 0; i < count; ++i) {
        if (fr.mode == packet::FrameMode::Tlv) {
            for (const auto& f : fields) out.push_back(encodeTlvFrame(fr, f, rng));
        } else {
            out.push_back(encodeHeaderLenFrame(fr, fields, rng));
        }
    }
    return out;
}

// 多帧头重载：每组配置对各自归属的字段生成（TLV 组走单配置语义，不影响）
std::vector<std::vector<uint8_t>> generateTestFrames(
    const std::vector<packet::FramingConfig>& framings, const std::vector<TagField>& fields,
    int count, uint32_t seed) {
    std::vector<std::vector<uint8_t>> out;
    if (framings.empty() || fields.empty() || count <= 0) return out;
    std::mt19937 rng(seed ? seed : std::random_device{}());
    for (int i = 0; i < count; ++i) {
        for (size_t k = 0; k < framings.size(); ++k) {
            std::vector<TagField> group;
            for (const auto& f : fields)
                if ((size_t)f.framingIndex == k) group.push_back(f);
            if (group.empty()) continue;
            if (framings[k].mode == packet::FrameMode::Tlv) {
                for (const auto& f : group) out.push_back(encodeTlvFrame(framings[k], f, rng));
            } else {
                out.push_back(encodeHeaderLenFrame(framings[k], group, rng));
            }
        }
    }
    return out;
}

} // namespace pv
