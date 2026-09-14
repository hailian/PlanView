#include "base/packet/DebugConfig.h"

#include "base/packet/HexUtil.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

namespace pv::packet::debugcfg {

namespace {

using Json = nlohmann::json;

Json fieldToJson(const PacketField& f) {
    Json j{{"name", f.name},
           {"offset", f.offset},
           {"length", f.length},
           {"type", fieldTypeToString(f.type)},
           {"bigEndian", f.bigEndian},
           {"scale", f.scale},
           {"offsetValue", f.offsetValue}};
    if (!f.enums.empty()) { // 枚举映射（仅 Enum 非空时写入）
        Json es = Json::array();
        for (const auto& e : f.enums) es.push_back(Json{{"v", e.first}, {"n", e.second}});
        j["enums"] = std::move(es);
    }
    return j;
}

PacketField fieldFromJson(const Json& j) {
    PacketField f;
    f.name = j.value("name", "");
    f.offset = j.value("offset", 0);
    f.length = j.value("length", 2);
    fieldTypeFromString(j.value("type", "u16"), f.type);
    f.bigEndian = j.value("bigEndian", true);
    f.scale = j.value("scale", 1.0);
    f.offsetValue = j.value("offsetValue", 0.0);
    if (j.contains("enums"))
        for (const auto& ej : j["enums"])
            f.enums.emplace_back(ej.value("v", int64_t(0)), ej.value("n", std::string()));
    // 固定长度类型以类型为准
    if (int n = fieldTypeBytes(f.type)) f.length = n;
    return f;
}

} // namespace

bool save(const std::string& path, const DebugConfig& cfg, std::string& err) {
    Json j;
    j["schemaVersion"] = 1;
    j["transport"] = {
        {"mode", cfg.transport == Transport::Tcp ? "tcp" : "udp"},
        {"host", cfg.host},
        {"remotePort", cfg.remotePort},
        {"localPort", cfg.localPort},
    };
    const FramingConfig& fr = cfg.framing;
    std::string headerHex;
    for (size_t i = 0; i < fr.header.size(); ++i) {
        char b[4];
        std::snprintf(b, sizeof(b), "%02X", fr.header[i]);
        if (i) headerHex += ' ';
        headerHex += b;
    }
    j["framing"] = {
        {"mode", fr.mode == FrameMode::Tlv ? "tlv" : "headerLength"},
        {"tagBytes", fr.tagBytes},
        {"lenBytes", fr.lenBytes},
        {"bigEndian", fr.bigEndian},
        {"lenIncludesHeader", fr.lenIncludesHeader},
        {"headerHex", headerHex},
        {"lenOffset", fr.lenOffset},
        {"lenBytesHeader", fr.lenBytesHeader},
        {"bigEndianHeader", fr.bigEndianHeader},
        {"lenIncludesAll", fr.lenIncludesAll},
        {"maxFrameLen", fr.maxFrameLen},
    };
    Json fields = Json::array();
    for (const auto& f : cfg.spec.fields) fields.push_back(fieldToJson(f));
    j["spec"] = {{"name", cfg.spec.name}, {"fields", fields}};

    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        err = "无法写入文件: " + path;
        return false;
    }
    f << j.dump(2) << "\n";
    return true;
}

bool load(const std::string& path, DebugConfig& cfg, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "无法打开文件: " + path;
        return false;
    }
    Json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        err = std::string("JSON 解析失败: ") + e.what();
        return false;
    }

    DebugConfig out;
    const Json& tr = j.value("transport", Json::object());
    out.transport = tr.value("mode", "udp") == "tcp" ? Transport::Tcp : Transport::Udp;
    out.host = tr.value("host", "127.0.0.1");
    out.remotePort = tr.value("remotePort", 9000);
    out.localPort = tr.value("localPort", 9001);

    const Json& fr = j.value("framing", Json::object());
    FramingConfig fc;
    fc.mode = fr.value("mode", "tlv") == "tlv" ? FrameMode::Tlv : FrameMode::HeaderLength;
    fc.tagBytes = fr.value("tagBytes", 1);
    fc.lenBytes = fr.value("lenBytes", 2);
    fc.bigEndian = fr.value("bigEndian", true);
    fc.lenIncludesHeader = fr.value("lenIncludesHeader", false);
    std::vector<uint8_t> header;
    std::string herr;
    hexToBytes(fr.value("headerHex", "AA 55"), header, herr);
    fc.header = header;
    fc.lenOffset = fr.value("lenOffset", 2);
    fc.lenBytesHeader = fr.value("lenBytesHeader", 2);
    fc.bigEndianHeader = fr.value("bigEndianHeader", true);
    fc.lenIncludesAll = fr.value("lenIncludesAll", false);
    fc.maxFrameLen = fr.value("maxFrameLen", 4096);
    out.framing = fc;

    const Json& sp = j.value("spec", Json::object());
    out.spec.name = sp.value("name", "规约1");
    if (sp.contains("fields"))
        for (const auto& fj : sp["fields"]) out.spec.fields.push_back(fieldFromJson(fj));

    cfg = std::move(out);
    return true;
}

} // namespace pv::packet::debugcfg
