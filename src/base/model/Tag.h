// 数据标签：组件与外部数据（TCP 数据服务器）之间的命名地址。
// 地址 = 数据服务器的槽位索引（0..65535），类型决定传输编码。
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "base/model/Types.h"

namespace softg {

// 类型名（协议与 JSON 共用）：bool/int16/uint16/int32/uint32/float32/string
// string 仅用于帧数据源（枚举名/文本字段）；SoftG 行协议不支持字符串标签。
enum class TagDataType : uint8_t { Bool, Int16, UInt16, Int32, UInt32, Float32, String };
const char* tagDataTypeName(TagDataType t);
std::optional<TagDataType> tagDataTypeFromName(std::string_view name);

// 运行时值统一升宽；字符串供显示
using TagValue = std::variant<bool, int64_t, double, std::string>;

enum class TagQuality : uint8_t { Good, Bad, CommLost };

struct Tag {
    TagName name;  // 主键，全工程唯一
    TagDataType type = TagDataType::UInt16;
    int address = 0;  // 数据服务器槽位索引
    double scale = 1.0;   // 工程值 = 原始值 * scale + offset
    double offset = 0.0;
    std::string comment;

    // ---- 运行时状态（不序列化） ----
    TagValue currentValue{};
    TagQuality quality = TagQuality::CommLost;
    std::chrono::steady_clock::time_point lastUpdate{};

    double toEngineering(double raw) const { return raw * scale + offset; }
    // 运行时值转 double（供绑定/告警求值）
    double numeric() const;
};

} // namespace softg
