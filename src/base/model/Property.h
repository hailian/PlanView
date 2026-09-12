// 组件属性的类型系统：类型化的键值对 + 属性规格（驱动 Inspector 自动生成编辑器）。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "imgui.h"

namespace softg {

enum class PropertyType : uint8_t {
    Bool,
    Int,
    Double,
    String,
    Color,   // uint32_t，IM_COL32 布局（R|G<<8|B<<16|A<<24）
    Enum,    // std::string，取值来自 enumValues
    Vec2,
};

// variant 成员顺序即类型索引；Color 用 uint32_t，Vec2 用 ImVec2
using PropertyValue = std::variant<bool, int64_t, double, std::string, uint32_t, ImVec2>;

struct PropertySpec {
    std::string key;         // "min"
    std::string label;       // "量程下限"（Inspector 显示名）
    PropertyType type = PropertyType::Bool;
    PropertyValue defaultValue;
    std::optional<double> minValue, maxValue;  // Int/Double 的编辑范围
    std::vector<std::string> enumValues;       // Enum 的候选值
};

// ---- 取值辅助：类型不符时返回默认值，不抛异常 ----
namespace props {

inline bool asBool(const PropertyValue& v, bool d = false) {
    if (auto p = std::get_if<bool>(&v)) return *p;
    if (auto p = std::get_if<int64_t>(&v)) return *p != 0;
    return d;
}
inline int64_t asInt(const PropertyValue& v, int64_t d = 0) {
    if (auto p = std::get_if<int64_t>(&v)) return *p;
    if (auto p = std::get_if<double>(&v)) return (int64_t)*p;
    if (auto p = std::get_if<bool>(&v)) return *p ? 1 : 0;
    return d;
}
inline double asDouble(const PropertyValue& v, double d = 0.0) {
    if (auto p = std::get_if<double>(&v)) return *p;
    if (auto p = std::get_if<int64_t>(&v)) return (double)*p;
    if (auto p = std::get_if<bool>(&v)) return *p ? 1.0 : 0.0;
    return d;
}
inline const std::string& asString(const PropertyValue& v, const std::string& d = std::string()) {
    if (auto p = std::get_if<std::string>(&v)) return *p;
    return d;
}
inline uint32_t asColor(const PropertyValue& v, uint32_t d = IM_COL32(255, 255, 255, 255)) {
    if (auto p = std::get_if<uint32_t>(&v)) return *p;
    return d;
}
inline ImVec2 asVec2(const PropertyValue& v, ImVec2 d = ImVec2(0, 0)) {
    if (auto p = std::get_if<ImVec2>(&v)) return *p;
    return d;
}

// 数值比较（编辑器校验 / 告警判断用）
inline double toNumber(const PropertyValue& v) { return asDouble(v); }

} // namespace props

// 组件属性容器：key -> value（std::less<> 允许 string_view 查找；序列化天然按 key 排序）
using PropertyMap = std::map<std::string, PropertyValue, std::less<>>;

} // namespace softg
