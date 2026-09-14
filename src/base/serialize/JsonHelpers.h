// JSON 序列化辅助：PropertyValue / 颜色 / 枚举 <-> nlohmann::json。
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "base/model/Association.h"
#include "base/model/Property.h"

namespace pv::jsonx {

using Json = nlohmann::json;

// PropertyValue -> json（按持有类型：Color->"#RRGGBBAA", Vec2->[x,y]）
Json toJson(const PropertyValue& v);
// json -> PropertyValue；spec 提供类型上下文（Enum/String 区分），
// spec 为空（未知属性保留透传）时按 json 形态推断。
PropertyValue fromJson(const Json& j, const PropertySpec* spec);

// 颜色：IM_COL32 布局 <-> "#RRGGBBAA"
std::string colorToJson(uint32_t col);
uint32_t jsonToColor(const std::string& s, uint32_t fallback = 0xFFFFFFFF);

// 枚举 <-> 字符串
std::string comparatorToString(Comparator c);
Comparator comparatorFromString(const std::string& s, Comparator fallback);
std::string severityToString(AlarmSeverity s);
AlarmSeverity severityFromString(const std::string& s, AlarmSeverity fallback);
std::string alarmStyleToString(AlarmStyle s);
AlarmStyle alarmStyleFromString(const std::string& s, AlarmStyle fallback);
std::string linkageEventToString(LinkageEvent e);
LinkageEvent linkageEventFromString(const std::string& s, LinkageEvent fallback);
std::string linkageActionToString(LinkageAction a);
LinkageAction linkageActionFromString(const std::string& s, LinkageAction fallback);

} // namespace pv::jsonx
