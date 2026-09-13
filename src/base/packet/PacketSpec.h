// PacketSpec — 自配置报文规约：字段列表 + 按规约解析帧负载。
// 规约描述帧内各字段的偏移/长度/类型/字节序与工程换算，解析结果用于展示。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace softg::packet {

enum class FieldType : uint8_t {
    U8, I8, U16, I16, U32, I32, F32, F64,
    Bool,    // 1 字节布尔（非 0 为真）
    String,  // 定长字符串（长度可配，保留原始字节，UTF-8 友好）
    Enum,    // 枚举：整数值 → 名称（宽度 1/2/4 可配，映射表见 PacketField::enums）
    Hex, Ascii,
};

const char* fieldTypeToString(FieldType t);
bool fieldTypeFromString(const std::string& s, FieldType& out);
// 固定长度类型的字节数；String/Enum/Hex/Ascii 返回 0 表示长度可配置
int fieldTypeBytes(FieldType t);

struct PacketField {
    std::string name;          // 字段名，如 "温度"
    int offset = 0;            // 帧内字节偏移
    int length = 2;            // 字节数（固定类型由类型决定；String/Enum/Hex/Ascii 可配）
    FieldType type = FieldType::U16;
    bool bigEndian = true;     // 多字节整数/浮点的字节序
    double scale = 1.0;        // 工程值 = 原始值 * scale + offsetValue
    double offsetValue = 0.0;
    std::vector<std::pair<int64_t, std::string>> enums; // Enum：值 → 名称
};

// 枚举值查名称；未命中返回 nullptr
inline const std::string* findEnumName(
    const std::vector<std::pair<int64_t, std::string>>& enums, int64_t v) {
    for (const auto& e : enums)
        if (e.first == v) return &e.second;
    return nullptr;
}

// 单字段解析结果（展示用）
struct ParsedField {
    std::string name;
    int offset = 0;
    int length = 0;
    std::string rawHex;        // 原始字节 hex
    std::string rawText;       // 原始值文本（整数/浮点/hex/ascii）
    std::string engText;       // 工程值文本；非数值类型与 rawText 相同
    bool ok = false;           // 帧长不足时为 false，rawText = "(越界)"
};

// 按规约解析一帧负载（payload 可为整帧或去头负载，由用户按规约设定偏移即可）
std::vector<ParsedField> parsePacket(const std::vector<PacketField>& fields,
                                     const std::vector<uint8_t>& payload);

// 规约整体（名称仅用于展示/存档）
struct PacketSpec {
    std::string name = "规约1";
    std::vector<PacketField> fields;
};

} // namespace softg::packet
