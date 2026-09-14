#include "base/data/frame/FrameSourceSettings.h"

#include "base/model/ComponentRegistry.h"
#include "base/model/Project.h"
#include "base/packet/HexUtil.h"

#include <algorithm>
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
        f.tagId = (int)props::asInt(c.propOr(prefix + "tagId", int64_t(0))); // 缺省 0，不自增
        f.offset = (int)props::asInt(c.propOr(prefix + "offset", int64_t(0)));
        std::string typeName = props::asString(c.propOr(prefix + "type", std::string("u16")));
        if (!packet::fieldTypeFromString(typeName, f.type))
            f.type = packet::FieldType::U16;
        if (int n = packet::fieldTypeBytes(f.type)) {
            f.bytes = n; // 固定长度类型由类型决定
        } else if (f.type == packet::FieldType::String) {
            f.bytes = (int)std::clamp<int64_t>(props::asInt(c.propOr(prefix + "len", int64_t(16))),
                                               1, 256);
        } else if (f.type == packet::FieldType::Enum) {
            int64_t w = props::asInt(c.propOr(prefix + "len", int64_t(1)));
            f.bytes = (w == 2 || w == 4) ? (int)w : 1; // 枚举宽度限 1/2/4 字节
        }
        f.bigEndian = props::asBool(c.propOr(prefix + "bigEndian", true));
        f.scale = props::asDouble(c.propOr(prefix + "scale", 1.0)); // 数值字段换算，缺省不缩放
        if (f.type == packet::FieldType::Enum) { // 枚举映射表 e<j>.v/.n
            int64_t n = props::asInt(c.propOr(prefix + "enumCount", int64_t(0)));
            for (int64_t j = 0; j < n; ++j) {
                std::string ep = prefix + "e" + std::to_string(j) + ".";
                f.enums.emplace_back(props::asInt(c.propOr(ep + "v", int64_t(0))),
                                     props::asString(c.propOr(ep + "n", std::string())));
            }
        }
        f.address = (int)i; // 标签槽位 = 字段序号（自动分配；不再人工配置）
        fields.push_back(std::move(f));
    }
}

FrameSourceSettings frameSettingsFromProject(const Project& p) {
    FrameSourceSettings s = p.settings.frame; // 旧工程回退（无数据源组件时保留）

    // 数据源组件存在即生效（取首个；autoStart 仅决定 PageViewer 打开时是否主动连接，
    // 默认关 = 打开后在运行器顶栏手动启动，配置本身始终可用）
    const Component* ds = p.findComponentByType("DataSource");
    if (!ds)
        return s; // enabled 维持工程设置（默认 false → SoftG 行协议）

    // 传输来自数据源组件（TCP / UDP / 串口；串口独占一组属性，角色/端口项不适用）
    s.enabled = true; // 数据源组件存在即启用
    s.autoStart = props::asBool(ds->propOr("autoStart", false)); // 默认关：运行器手动启动
    std::string transport = props::asString(ds->propOr("transport", std::string("UDP")));
    s.udp = transport == "UDP";
    s.serial = transport == "串口";
    s.udpClient = s.udp &&
                  props::asString(ds->propOr("udpRole", std::string("服务端"))) == "客户端";
    s.tcpClient = props::asString(ds->propOr("tcpRole", std::string("客户端"))) != "服务端";
    s.host = props::asString(ds->propOr("host", s.host));
    s.remotePort = (int)props::asInt(ds->propOr("remotePort", int64_t(s.remotePort)));
    s.localPort = (int)props::asInt(ds->propOr("localPort", int64_t(s.localPort)));
    s.serialPort = props::asString(ds->propOr("serialPort", s.serialPort));
    s.baud = (int)props::asInt(ds->propOr("baud", int64_t(s.baud)));
    s.dataBits = (int)props::asInt(ds->propOr("dataBits", int64_t(s.dataBits)));
    s.parity = props::asString(ds->propOr("parity", s.parity));
    s.stopBits = (int)props::asInt(ds->propOr("stopBits", int64_t(s.stopBits)));

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

std::vector<ComponentId> generateFieldComponents(Page& page, Project& proj,
                                                 const ComponentId& protoId, int& skipped) {
    skipped = 0;
    std::vector<ComponentId> created;

    // ---- 先快照协议组件全部数据：后续 push_back 扩容会使组件引用失效 ----
    const Component* proto = page.find(protoId);
    if (!proto || proto->typeId != "ProtocolConfig") return created;
    const std::string protoName = proto->name;
    const float x0 = proto->frame.x;
    const float y0 = proto->frame.y + proto->frame.h + 40.0f;
    struct Spec {
        std::string name, type;
    };
    std::vector<Spec> specs;
    int64_t count = props::asInt(proto->propOr("fieldCount", int64_t(0)));
    for (int64_t i = 0; i < count; ++i) {
        std::string p = "f" + std::to_string(i) + ".";
        specs.push_back({props::asString(proto->propOr(p + "name", std::string("?"))),
                         props::asString(proto->propOr(p + "type", std::string("u16")))});
    }

    // 字段类型 → 组件类型：bool→指示灯；其余（整数/浮点/string/enum）一律文本——
    // 数值精度与名称类都更适合文本展示，仪表按需手工添加并绑定
    auto mapType = [](const std::string& t) -> const char* {
        return t == "bool" ? "Lamp" : "Label";
    };

    // 纵向排列：每行 = 字段名文本(96) + 间距(8) + 组件，行距 56
    const float rowH = 56.0f;
    const float nameW = 96.0f;
    const float gap = 8.0f;
    int row = 0;
    for (const auto& sp : specs) {
        const char* compType = mapType(sp.type);
        std::string bind = protoName + "/" + sp.name;
        bool exists = false;
        for (const auto& ec : page.components)
            if (ec.typeId == compType &&
                props::asString(ec.propOr("bindField", std::string())) == bind)
                exists = true;
        if (exists) {
            ++skipped;
            continue;
        }
        const float rowY = y0 + row * rowH;
        Component nc = ComponentRegistry::createComponent(compType, proj.allocId("comp"));
        nc.name = sp.name;
        nc.frame.x = x0 + nameW + gap;
        nc.frame.y = rowY;
        nc.z = page.components.empty() ? 0 : page.components.back().z + 1;
        nc.setProp("bindField", bind);
        page.components.push_back(std::move(nc));
        created.push_back(page.components.back().id);

        // 组件左侧的字段名静态文本（无绑定；组件已存在跳过时不再补名字，
        // 避免重复生成时叠字）
        Component nl = ComponentRegistry::createComponent("Label", proj.allocId("comp"));
        nl.name = sp.name + "·名";
        nl.frame.x = x0;
        nl.frame.y = rowY;
        nl.frame.w = nameW;
        nl.z = page.components.back().z + 1;
        nl.setProp("text", sp.name);
        page.components.push_back(std::move(nl));
        created.push_back(page.components.back().id);
        ++row;
    }
    return created;
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
                case packet::FieldType::Bool: nt.type = TagDataType::Bool; break;
                case packet::FieldType::String: case packet::FieldType::Enum:
                    nt.type = TagDataType::String; break; // 文本/枚举名标签
                default: nt.type = TagDataType::UInt16; break; // u8/u16/i8 升宽
                }
                // 带 scale 的数值字段：工程值可能为小数，标签升为 Float32 承接
                if (f->scale != 1.0 && f->type != packet::FieldType::Bool &&
                    f->type != packet::FieldType::String && f->type != packet::FieldType::Enum)
                    nt.type = TagDataType::Float32;
                nt.scale = 1.0; // 换算在协议侧（字段 scale），标签不再二次缩放
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
