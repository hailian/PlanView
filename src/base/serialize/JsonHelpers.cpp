#include "base/serialize/JsonHelpers.h"

#include <charconv>
#include <cstdio>
#include <system_error>

namespace pv::jsonx {

Json toJson(const PropertyValue& v) {
    switch (v.index()) {
    case 0: return std::get<0>(v);                      // bool
    case 1: return std::get<1>(v);                      // int64
    case 2: return std::get<2>(v);                      // double
    case 3: return std::get<3>(v);                      // string
    case 4: return colorToJson(std::get<4>(v));         // color -> "#RRGGBBAA"
    case 5: {                                           // ImVec2 -> [x, y]
        ImVec2 p = std::get<5>(v);
        return Json::array({p.x, p.y});
    }
    }
    return nullptr;
}

PropertyValue fromJson(const Json& j, const PropertySpec* spec) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number_integer()) return j.get<int64_t>();
    if (j.is_number_float()) return j.get<double>();
    if (j.is_string()) {
        const std::string& s = j.get_ref<const std::string&>();
        if (s.size() == 9 && s[0] == '#')
            return jsonToColor(s);  // 颜色字符串
        if (s == "true" || s == "false") return s == "true";  // 兼容手写
        return s;
    }
    if (j.is_array() && j.size() == 2 && j[0].is_number() && j[1].is_number())
        return ImVec2(j[0].get<float>(), j[1].get<float>());
    return PropertyValue{};  // null / 无法识别
}

std::string colorToJson(uint32_t col) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X",
                  (col & 0xFF), ((col >> 8) & 0xFF), ((col >> 16) & 0xFF), ((col >> 24) & 0xFF));
    return buf;
}

uint32_t jsonToColor(const std::string& s, uint32_t fallback) {
    if (s.size() != 9 || s[0] != '#') return fallback;
    auto hex2 = [](const char* p) -> int {
        int v = 0;
        for (int i = 0; i < 2; ++i) {
            char c = p[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return -1;
        }
        return v;
    };
    int r = hex2(s.c_str() + 1), g = hex2(s.c_str() + 3), b = hex2(s.c_str() + 5),
        a = hex2(s.c_str() + 7);
    if (r < 0 || g < 0 || b < 0 || a < 0) return fallback;
    return IM_COL32(r, g, b, a);
}

std::string comparatorToString(Comparator c) {
    switch (c) {
    case Comparator::GT: return ">";
    case Comparator::GE: return ">=";
    case Comparator::LT: return "<";
    case Comparator::LE: return "<=";
    case Comparator::EQ: return "==";
    case Comparator::NE: return "!=";
    }
    return ">";
}
Comparator comparatorFromString(const std::string& s, Comparator fallback) {
    if (s == ">") return Comparator::GT;
    if (s == ">=") return Comparator::GE;
    if (s == "<") return Comparator::LT;
    if (s == "<=") return Comparator::LE;
    if (s == "==") return Comparator::EQ;
    if (s == "!=") return Comparator::NE;
    return fallback;
}

std::string severityToString(AlarmSeverity s) {
    switch (s) {
    case AlarmSeverity::Low: return "low";
    case AlarmSeverity::High: return "high";
    case AlarmSeverity::Critical: return "critical";
    }
    return "high";
}
AlarmSeverity severityFromString(const std::string& s, AlarmSeverity fallback) {
    if (s == "low") return AlarmSeverity::Low;
    if (s == "high") return AlarmSeverity::High;
    if (s == "critical") return AlarmSeverity::Critical;
    return fallback;
}

std::string alarmStyleToString(AlarmStyle s) {
    switch (s) {
    case AlarmStyle::Flash: return "flash";
    case AlarmStyle::Border: return "border";
    case AlarmStyle::Color: return "color";
    }
    return "flash";
}
AlarmStyle alarmStyleFromString(const std::string& s, AlarmStyle fallback) {
    if (s == "flash") return AlarmStyle::Flash;
    if (s == "border") return AlarmStyle::Border;
    if (s == "color") return AlarmStyle::Color;
    return fallback;
}

std::string linkageEventToString(LinkageEvent e) {
    switch (e) {
    case LinkageEvent::Click: return "click";
    case LinkageEvent::ValueChanged: return "valueChanged";
    case LinkageEvent::AlarmActive: return "alarmActive";
    case LinkageEvent::AlarmCleared: return "alarmCleared";
    }
    return "click";
}
LinkageEvent linkageEventFromString(const std::string& s, LinkageEvent fallback) {
    if (s == "click") return LinkageEvent::Click;
    if (s == "valueChanged") return LinkageEvent::ValueChanged;
    if (s == "alarmActive") return LinkageEvent::AlarmActive;
    if (s == "alarmCleared") return LinkageEvent::AlarmCleared;
    return fallback;
}

std::string linkageActionToString(LinkageAction a) {
    switch (a) {
    case LinkageAction::SetProperty: return "setProperty";
    case LinkageAction::Navigate: return "navigate";
    case LinkageAction::ToggleVisible: return "toggleVisible";
    case LinkageAction::SetTagValue: return "setTagValue";
    case LinkageAction::Pulse: return "pulse";
    }
    return "setProperty";
}
LinkageAction linkageActionFromString(const std::string& s, LinkageAction fallback) {
    if (s == "setProperty") return LinkageAction::SetProperty;
    if (s == "navigate") return LinkageAction::Navigate;
    if (s == "toggleVisible") return LinkageAction::ToggleVisible;
    if (s == "setTagValue") return LinkageAction::SetTagValue;
    if (s == "pulse") return LinkageAction::Pulse;
    return fallback;
}

} // namespace pv::jsonx
