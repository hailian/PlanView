#include "base/data/frame/FrameSourceSettings.h"

#include "base/model/Project.h"
#include "base/packet/HexUtil.h"

namespace softg {

// 协议配置组件属性 -> 拆帧参数 + 规约字段。
// 字段存储为索引属性：fieldCount + f<i>.name / f<i>.tagId / f<i>.offset /
// f<i>.type（类型名字符串）/ f<i>.bigEndian / f<i>.address
void protocolFramingFromComponent(const Component& c, packet::FramingConfig& fr,
                                  std::vector<TagField>& fields) {
    bool headerLenMode = props::asString(c.propOr("framingMode", std::string("TLV"))) != "TLV";
    fr.mode = headerLenMode ? packet::FrameMode::HeaderLength : packet::FrameMode::Tlv;
    fr.tagBytes = (int)props::asInt(c.propOr("tagBytes", int64_t(fr.tagBytes)));
    fr.lenBytes = (int)props::asInt(c.propOr("lenBytes", int64_t(fr.lenBytes)));
    fr.bigEndian = props::asBool(c.propOr("bigEndian", fr.bigEndian));
    fr.lenIncludesHeader = props::asBool(c.propOr("lenIncludesHeader", false));
    std::string headerHex = props::asString(c.propOr("headerHex", std::string("AA 55")));
    std::vector<uint8_t> header;
    std::string err;
    if (packet::hexToBytes(headerHex, header, err) && !header.empty())
        fr.header = header;
    fr.lenOffset = (int)props::asInt(c.propOr("lenOffset", int64_t(fr.lenOffset)));
    fr.lenBytesHeader = (int)props::asInt(c.propOr("lenBytesHeader", int64_t(fr.lenBytesHeader)));
    fr.bigEndianHeader = props::asBool(c.propOr("bigEndianHeader", fr.bigEndianHeader));
    fr.lenIncludesAll = props::asBool(c.propOr("lenIncludesAll", false));

    fields.clear();
    int64_t count = props::asInt(c.propOr("fieldCount", int64_t(0)));
    for (int64_t i = 0; i < count; ++i) {
        std::string prefix = "f" + std::to_string(i) + ".";
        TagField f;
        f.name = props::asString(c.propOr(prefix + "name", std::string("字段")));
        f.tagId = (int)props::asInt(c.propOr(prefix + "tagId", int64_t(i + 1)));
        f.offset = (int)props::asInt(c.propOr(prefix + "offset", int64_t(0)));
        std::string typeName = props::asString(c.propOr(prefix + "type", std::string("u16")));
        if (!packet::fieldTypeFromString(typeName, f.type))
            f.type = packet::FieldType::U16;
        if (int n = packet::fieldTypeBytes(f.type)) f.bytes = n;
        f.bigEndian = props::asBool(c.propOr(prefix + "bigEndian", true));
        f.address = (int)props::asInt(c.propOr(prefix + "address", int64_t(0)));
        fields.push_back(std::move(f));
    }
}

FrameSourceSettings frameSettingsFromProject(const Project& p) {
    FrameSourceSettings s = p.settings.frame; // 旧工程回退（无数据源组件时保留）

    const Component* ds = p.findComponentByType("DataSource");
    if (!ds)
        return s; // enabled 维持工程设置（默认 false → SoftG 行协议）

    // 传输来自数据源组件
    s.enabled = true; // 数据源组件存在即启用
    s.udp = props::asString(ds->propOr("transport", std::string("UDP"))) != "TCP";
    s.host = props::asString(ds->propOr("host", s.host));
    s.remotePort = (int)props::asInt(ds->propOr("remotePort", int64_t(s.remotePort)));
    s.localPort = (int)props::asInt(ds->propOr("localPort", int64_t(s.localPort)));

    // 拆帧/字段来自关联的协议配置组件（按名称匹配；缺失时用默认 TLV 无字段）
    std::string protoName = props::asString(ds->propOr("protocol", std::string()));
    const Component* proto = nullptr;
    if (!protoName.empty()) {
        for (const auto& pg : p.pages)
            for (const auto& c : pg.components)
                if (c.typeId == "ProtocolConfig" && c.name == protoName) {
                    proto = &c;
                    break;
                }
    }
    packet::FramingConfig framing;
    std::vector<TagField> fields;
    if (proto)
        protocolFramingFromComponent(*proto, framing, fields);
    s.framing = framing;
    s.fields = std::move(fields);
    return s;
}

} // namespace softg
