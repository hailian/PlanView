#include "base/llm/AiProto.h"

#include <nlohmann/json.hpp>

namespace pv::llm {

std::string aiProtoSystemPrompt() {
    return
        "你是工业通信协议配置助手。根据用户给出的协议文档或自然语言描述，输出一个 JSON 对象"
        "（除 JSON 外不要输出任何文字，不要使用 Markdown 围栏）。schema：\n"
        "{\n"
        "  \"framingMode\": \"TLV\" 或 \"帧头+Length\",\n"
        "  \"tagBytes\": 1-4, \"lenBytes\": 1-4, \"bigEndian\": bool, \"lenIncludesHeader\": bool,\n"
        "  \"headerHex\": \"AA 55\", \"lenOffset\": 2, \"lenBytesHeader\": 1-4, "
        "\"bigEndianHeader\": bool, \"lenIncludesAll\": bool,\n"
        "  \"fields\": [ {\"name\": \"字段中文名\", \"tagId\": 1, \"offset\": 0, "
        "\"type\": \"u16\", \"bigEndian\": true} ]\n"
        "}\n"
        "规则：TLV 模式填 tagBytes/lenBytes/bigEndian/lenIncludesHeader，字段 tagId 为该帧 T 值"
        "（十进制数），offset 相对该帧负载 V 的起点；帧头+Length 模式填 headerHex/lenOffset/"
        "lenBytesHeader/bigEndianHeader/lenIncludesAll，offset 相对帧头+length 字段之后的负载数据区。"
        "字段 type ∈ u8|i8|u16|i16|u32|i32|f32|f64|bool|string|enum。"
        "string 类型（定长字节串）额外附 \"len\": 字节数；enum 类型（整数编码对应显示名）额外附 "
        "\"enum\": [{\"v\": 1, \"n\": \"启动\"}] 映射表。字段名用简短中文。"
        "length 语义按文档判断：指整帧长则 lenIncludesAll/lenIncludesHeader 为 true，"
        "仅负载长则为 false。未说明的字节序默认大端。";
}

namespace {

bool validFieldType(const std::string& t) {
    static const char* kTypes[] = {"u8",  "i8",  "u16", "i16", "u32", "i32",
                                   "f32", "f64", "bool", "string", "enum"};
    for (const char* k : kTypes)
        if (t == k) return true;
    return false;
}

} // namespace

bool parseAiProtoResponse(const std::string& raw, AiProtoResult& out, std::string& err) {
    err.clear();
    // 容错：取第一个 '{' 到最后一个 '}'（剥离围栏与前后说明文字）
    size_t b = raw.find('{');
    size_t e = raw.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e <= b) {
        err = "响应中未找到 JSON 对象";
        return false;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(raw.substr(b, e - b + 1));
    } catch (const std::exception& ex) {
        err = std::string("JSON 解析失败: ") + ex.what();
        return false;
    }
    if (!j.is_object()) {
        err = "JSON 不是对象";
        return false;
    }

    std::string mode = j.value("framingMode", std::string("TLV"));
    out = AiProtoResult{}; // 重置默认
    out.tlv = mode != "帧头+Length" && mode != "headerLength" && mode != "HeaderLength";
    if (out.tlv) {
        out.tagBytes = j.value("tagBytes", out.tagBytes);
        out.lenBytes = j.value("lenBytes", out.lenBytes);
        out.bigEndian = j.value("bigEndian", out.bigEndian);
        out.lenIncludesHeader = j.value("lenIncludesHeader", out.lenIncludesHeader);
    } else {
        out.headerHex = j.value("headerHex", out.headerHex);
        out.lenOffset = j.value("lenOffset", out.lenOffset);
        out.lenBytesHeader = j.value("lenBytesHeader", out.lenBytesHeader);
        out.bigEndianHeader = j.value("bigEndianHeader", out.bigEndianHeader);
        out.lenIncludesAll = j.value("lenIncludesAll", out.lenIncludesAll);
    }

    if (j.contains("fields") && j["fields"].is_array()) {
        for (const auto& jf : j["fields"]) {
            if (!jf.is_object()) continue;
            AiProtoField f;
            f.name = jf.value("name", std::string("字段"));
            f.tagId = jf.value("tagId", 0);
            f.offset = jf.value("offset", 0);
            std::string t = jf.value("type", std::string("u16"));
            f.type = validFieldType(t) ? t : "u16"; // 非法类型回退
            f.bigEndian = jf.value("bigEndian", true);
            if (f.type == "string")
                f.strLen = jf.value("len", 16);
            if (f.type == "enum" && jf.contains("enum") && jf["enum"].is_array())
                for (const auto& je : jf["enum"]) {
                    AiEnumItem it;
                    it.value = je.value("v", (int64_t)0);
                    it.name = je.value("n", std::string());
                    if (!it.name.empty()) f.enumItems.push_back(std::move(it));
                }
            out.fields.push_back(std::move(f));
        }
    }
    if (out.fields.empty())
        err = "未解析到字段（framing 已应用，字段为空）";
    return true;
}

void applyAiProto(Component& c, const AiProtoResult& r) {
    c.setProp("framingMode", std::string(r.tlv ? "TLV" : "帧头+Length"));
    if (r.tlv) {
        c.setProp("tagBytes", int64_t(r.tagBytes));
        c.setProp("lenBytes", int64_t(r.lenBytes));
        c.setProp("bigEndian", r.bigEndian);
        c.setProp("lenIncludesHeader", r.lenIncludesHeader);
    } else {
        c.setProp("headerHex", r.headerHex);
        c.setProp("lenOffset", int64_t(r.lenOffset));
        c.setProp("lenBytesHeader", int64_t(r.lenBytesHeader));
        c.setProp("bigEndianHeader", r.bigEndianHeader);
        c.setProp("lenIncludesAll", r.lenIncludesAll);
    }
    c.setProp("fieldCount", int64_t(r.fields.size()));
    for (size_t i = 0; i < r.fields.size(); ++i) {
        std::string p = "f" + std::to_string(i) + ".";
        const AiProtoField& f = r.fields[i];
        c.setProp(p + "name", f.name);
        c.setProp(p + "tagId", int64_t(f.tagId));
        c.setProp(p + "offset", int64_t(f.offset));
        c.setProp(p + "type", f.type);
        c.setProp(p + "bigEndian", f.bigEndian);
        if (f.type == "string")
            c.setProp(p + "len", int64_t(f.strLen));
        if (f.type == "enum") {
            c.setProp(p + "len", int64_t(1)); // 枚举默认 1 字节（可在枚举编辑器中改）
            c.setProp(p + "enumCount", int64_t(f.enumItems.size()));
            for (size_t k = 0; k < f.enumItems.size(); ++k) {
                std::string ep = p + "e" + std::to_string(k) + ".";
                c.setProp(ep + "v", f.enumItems[k].value);
                c.setProp(ep + "n", f.enumItems[k].name);
            }
        }
    }
}

} // namespace pv::llm
