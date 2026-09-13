#include "base/data/frame/FrameSourceSettings.h"

#include "base/model/Project.h"
#include "base/packet/HexUtil.h"

#include <map>

namespace softg {

namespace {

// 分割 "协议名/字段名" 并去除首尾空格（容忍 UI 早期版本存储的 " / " 分隔）
bool splitBindField(const std::string& bf, std::string& protoName, std::string& fieldName) {
    size_t slash = bf.find('/');
    if (slash == std::string::npos) return false;
    auto trim = [](const std::string& s) {
        size_t b = s.find_first_not_of(" \t");
        size_t e = s.find_last_not_of(" \t");
        return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    };
    protoName = trim(bf.substr(0, slash));
    fieldName = trim(bf.substr(slash + 1));
    return !protoName.empty() && !fieldName.empty();
}

} // namespace

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
        f.address = (int)i; // 标签槽位 = 字段序号（自动分配；不再人工配置）
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
    s.udpClient = s.udp &&
                  props::asString(ds->propOr("udpRole", std::string("服务端"))) == "客户端";
    s.tcpClient = props::asString(ds->propOr("tcpRole", std::string("客户端"))) != "服务端";
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

const char* defaultBindableProperty(const Component& c) {
    if (c.typeId == "Label") return "text";
    if (c.typeId == "Lamp" || c.typeId == "Switch") return "isOn";
    return "value";
}

void synthesizeImplicitBindings(Project& p) {
    // 协议名 -> 字段表（拷贝，避免悬垂）
    std::map<std::string, std::vector<TagField>> protoFields;
    for (const auto& pg : p.pages)
        for (const auto& c : pg.components)
            if (c.typeId == "ProtocolConfig") {
                packet::FramingConfig fr;
                std::vector<TagField> fs;
                protocolFramingFromComponent(c, fr, fs);
                protoFields[c.name] = std::move(fs);
            }

    for (auto& pg : p.pages)
        for (auto& c : pg.components) {
            if (c.typeId == "DataSource" || c.typeId == "ProtocolConfig") continue;
            std::string bf = props::asString(c.propOr("bindField", std::string()));
            if (bf.empty()) continue;
            std::string protoName, fieldName;
            if (!splitBindField(bf, protoName, fieldName)) continue;
            auto it = protoFields.find(protoName);
            if (it == protoFields.end()) continue;
            const TagField* f = nullptr;
            for (const auto& tf : it->second)
                if (tf.name == fieldName) {
                    f = &tf;
                    break;
                }
            if (!f) continue;

            // 隐式标签：同槽位复用；否则按字段创建（名字唯一化）
            Tag* t = nullptr;
            for (auto& tag : p.tags.all())
                if (tag.address == f->address) {
                    t = &tag;
                    break;
                }
            if (!t) {
                Tag nt;
                nt.name = f->name;
                while (p.tags.find(nt.name))
                    nt.name = f->name + "@" + std::to_string(f->address); // 重名加槽位后缀
                nt.address = f->address;
                switch (f->type) {
                case packet::FieldType::I8: case packet::FieldType::I16:
                    nt.type = TagDataType::Int16; break;
                case packet::FieldType::I32: nt.type = TagDataType::Int32; break;
                case packet::FieldType::U32: nt.type = TagDataType::UInt32; break;
                case packet::FieldType::F32: case packet::FieldType::F64:
                    nt.type = TagDataType::Float32; break;
                default: nt.type = TagDataType::UInt16; break; // u8/u16/i8 升宽
                }
                nt.scale = 1.0; // 组件直接绑字段：换算在字段类型/协议侧，不做二次缩放
                std::string err;
                p.tags.add(std::move(nt), err);
                for (auto& tag : p.tags.all())
                    if (tag.address == f->address) t = &tag;
            }
            if (!t) continue;

            // 数据绑定：同组件同属性已有（手工或已合成）则跳过
            const char* prop = defaultBindableProperty(c);
            bool exists = false;
            for (const auto& a : p.associations)
                if (auto* b = std::get_if<DataBinding>(&a);
                    b && b->component == c.id && b->property == prop)
                    exists = true;
            if (!exists) {
                DataBinding b;
                b.id = p.allocId("bind");
                b.component = c.id;
                b.property = prop;
                b.tag = t->name;
                p.associations.push_back(std::move(b));
            }
        }
}

} // namespace softg
