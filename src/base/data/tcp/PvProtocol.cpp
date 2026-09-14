#include "base/data/tcp/PvProtocol.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace pv::tcp {

namespace {

std::optional<int> toInt(std::string_view s) {
    int v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
    return v;
}
std::optional<double> toDouble(std::string_view s) {
    // from_chars(double) MSVC 支持；但这里文本可能带小数点/负号——用 strtod 兜底
    std::string buf(s);
    char* end = nullptr;
    double v = std::strtod(buf.c_str(), &end);
    if (end == buf.c_str() || *end != '\0') return std::nullopt;
    return v;
}

} // namespace

std::string encodePing() { return "PING\n"; }

std::string encodeReadReq(const std::vector<int>& indices) {
    std::string s = "READ " + std::to_string(indices.size());
    for (int idx : indices) s += " " + std::to_string(idx);
    s += "\n";
    return s;
}

std::string encodeWriteReq(int index, TagDataType type, const std::string& valueText) {
    return "WRITE " + std::to_string(index) + " " + tagDataTypeName(type) + " " + valueText + "\n";
}

std::string tagValueToText(const TagValue& v) {
    if (auto* b = std::get_if<bool>(&v)) return *b ? "1" : "0";
    if (auto* i = std::get_if<int64_t>(&v)) return std::to_string(*i);
    if (auto* d = std::get_if<double>(&v)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.9g", *d);
        return buf;
    }
    return std::get<std::string>(v);
}

std::optional<TagValue> parseTypedValue(std::string_view type, std::string_view text) {
    if (type == "bool") {
        if (text == "1" || text == "true") return TagValue(true);
        if (text == "0" || text == "false") return TagValue(false);
        return std::nullopt;
    }
    if (type == "int16" || type == "uint16" || type == "int32" || type == "uint32") {
        auto v = toInt(text);
        if (!v) return std::nullopt;
        return TagValue((int64_t)*v);
    }
    if (type == "float32") {
        auto v = toDouble(text);
        if (!v) return std::nullopt;
        return TagValue(*v);
    }
    return std::nullopt;
}

bool parseValuesLine(std::string_view line, std::vector<ReadItem>& out) {
    out.clear();
    if (!line.starts_with("VALUES")) return false;
    std::vector<std::string_view> tok;
    size_t pos = 0;
    while (pos <= line.size()) {
        size_t sp = line.find(' ', pos);
        if (sp == std::string_view::npos) {
            tok.push_back(line.substr(pos));
            break;
        }
        tok.push_back(line.substr(pos, sp - pos));
        pos = sp + 1;
    }
    if (tok.size() < 2) return false;
    auto n = toInt(tok[1]);
    if (!n) return false;
    if ((int)tok.size() < 2 + *n) return false;

    for (int i = 0; i < *n; ++i) {
        std::string_view item = tok[2 + (size_t)i];
        // idx:type:value
        size_t c1 = item.find(':');
        size_t c2 = item.find(':', c1 + 1);
        if (c1 == std::string_view::npos || c2 == std::string_view::npos) return false;
        ReadItem ri;
        auto idx = toInt(item.substr(0, c1));
        if (!idx) return false;
        ri.index = *idx;
        std::string_view type = item.substr(c1 + 1, c2 - c1 - 1);
        std::string_view value = item.substr(c2 + 1);
        auto t = tagDataTypeFromName(type);
        if (type == "?" || value == "-") {
            ri.ok = false;  // 服务器无此槽位
        } else if (t) {
            ri.type = *t;
            if (auto v = parseTypedValue(type, value)) {
                ri.value = *v;
                ri.ok = true;
            } else {
                ri.ok = false;
            }
        } else {
            return false;  // 未知类型名 -> 整行非法
        }
        out.push_back(ri);
    }
    return true;
}

WriteAck parseWriteAck(std::string_view line) {
    WriteAck ack{false, -1, ""};
    if (line.starts_with("OK ")) {
        auto idx = toInt(line.substr(3));
        if (idx) {
            ack.ok = true;
            ack.index = *idx;
        }
    } else if (line.starts_with("ERR ")) {
        ack.error = std::string(line.substr(4));
    }
    return ack;
}

} // namespace pv::tcp
