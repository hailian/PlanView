#include "base/data/frame/FrameSourceSettings.h"

#include "base/model/ComponentRegistry.h"
#include "base/model/Project.h"
#include "base/packet/HexUtil.h"

#include <algorithm>
#include <map>

namespace pv {

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
        f.protoName = c.name; // 字段归属的协议（隐式标签按运行时槽位合成时追溯）
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
        return s; // enabled 维持工程设置（默认 false → PlanView 行协议）

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
    s.listen = transport == "监听";
    if (s.listen) {
        s.listenIp = props::asString(ds->propOr("listenIp", s.listenIp));
        s.listenPort = (int)props::asInt(ds->propOr("listenPort", int64_t(s.listenPort)));
        s.listenTcp = props::asString(ds->propOr("listenProto", std::string("UDP"))) == "TCP";
    }
    s.serialPort = props::asString(ds->propOr("serialPort", s.serialPort));
    s.baud = (int)props::asInt(ds->propOr("baud", int64_t(s.baud)));
    s.dataBits = (int)props::asInt(ds->propOr("dataBits", int64_t(s.dataBits)));
    s.parity = props::asString(ds->propOr("parity", s.parity));
    s.stopBits = (int)props::asInt(ds->propOr("stopBits", int64_t(s.stopBits)));

    // 数据目的组件：把生效数据源收到的原始帧转发出去（按 source 属性关联数据源名）。
    // 传输参数解析与数据源同构；仅 sourceName 匹配生效数据源的 sink 在运行器生效
    s.sourceName = ds->name;
    for (const auto& pg : p.pages)
        for (const auto& c : pg.components) {
            if (c.typeId != "DataSink") continue;
            FrameSinkSettings k;
            k.sourceName = props::asString(c.propOr("source", std::string()));
            std::string tr = props::asString(
                c.propOr("transport", std::string("TCP"))); // 与注册表默认一致
            k.udp = tr == "UDP";
            k.serial = tr == "串口";
            k.udpClient = k.udp &&
                          props::asString(c.propOr("udpRole", std::string("服务端"))) == "客户端";
            k.tcpClient =
                props::asString(c.propOr("tcpRole", std::string("客户端"))) != "服务端";
            k.host = props::asString(c.propOr("host", k.host));
            k.remotePort = (int)props::asInt(c.propOr("remotePort", int64_t(k.remotePort)));
            k.localPort = (int)props::asInt(c.propOr("localPort", int64_t(k.localPort)));
            k.serialPort = props::asString(c.propOr("serialPort", k.serialPort));
            k.baud = (int)props::asInt(c.propOr("baud", int64_t(k.baud)));
            k.dataBits = (int)props::asInt(c.propOr("dataBits", int64_t(k.dataBits)));
            k.parity = props::asString(c.propOr("parity", k.parity));
            k.stopBits = (int)props::asInt(c.propOr("stopBits", int64_t(k.stopBits)));
            s.sinks.push_back(std::move(k));
        }

    // 拆帧/字段来自关联的协议配置组件（按名称匹配；缺失时用默认 TLV 无字段）
    // 或关联的协议组（组内全部协议配置字段按顺序合并——典型 TLV 多协议按 T 值分段；
    // 拆帧参数取组内第一个协议）
    std::string protoName = props::asString(ds->propOr("protocol", std::string()));
    std::string groupName = props::asString(ds->propOr("group", std::string()));
    const Component* proto = nullptr;
    std::vector<const Component*> groupProtos; // 协议组展开（按成员顺序）
    if (!groupName.empty()) {
        for (const auto& pg : p.pages)
            for (const auto& c : pg.components) {
                if (c.typeId != "ProtocolGroup" || c.name != groupName) continue;
                int64_t n = props::asInt(c.propOr("protoCount", int64_t(0)));
                for (int64_t i = 0; i < n; ++i) {
                    std::string member =
                        props::asString(c.propOr("p" + std::to_string(i) + ".name",
                                                 std::string()));
                    if (member.empty()) continue;
                    for (const auto& pg2 : p.pages)
                        for (const auto& c2 : pg2.components)
                            if (c2.typeId == "ProtocolConfig" && c2.name == member)
                                groupProtos.push_back(&c2);
                }
                break; // 重名组取首个（校验面板提示重名）
            }
        // 组未设置/不存在/为空：回退「关联协议」（二选一语义；组无效由校验面板提示）
    }
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
    if (!groupProtos.empty()) {
        std::vector<packet::FramingConfig> configs; // 去重后的拆帧配置（索引即 framingIndex）
        auto sameConfig = [](const packet::FramingConfig& a, const packet::FramingConfig& b) {
            return a.mode == b.mode && a.header == b.header &&
                   a.lenOffset == b.lenOffset && a.lenBytesHeader == b.lenBytesHeader &&
                   a.bigEndianHeader == b.bigEndianHeader && a.lenIncludesAll == b.lenIncludesAll &&
                   a.tagBytes == b.tagBytes && a.lenBytes == b.lenBytes &&
                   a.bigEndian == b.bigEndian && a.lenIncludesHeader == b.lenIncludesHeader;
        };
        for (size_t i = 0; i < groupProtos.size(); ++i) {
            packet::FramingConfig f;
            std::vector<TagField> fs;
            protocolFramingFromComponent(*groupProtos[i], f, fs);
            if (i == 0) framing = f; // 主拆帧配置取第一个成员
            int idx = 0;
            bool dup = false;
            for (size_t k = 0; k < configs.size(); ++k)
                if (sameConfig(configs[k], f)) {
                    idx = (int)k; // 同拆帧参数（含同帧头）的成员共用一条配置
                    dup = true;
                    break;
                }
            if (!dup) {
                idx = (int)configs.size();
                configs.push_back(f);
            }
            for (auto& tf : fs) tf.framingIndex = idx; // 字段归属其协议的拆帧配置
            fields.insert(fields.end(), std::make_move_iterator(fs.begin()),
                          std::make_move_iterator(fs.end()));
        }
        // 隐式标签槽位按合并后序号重排（协议内序号会跨协议冲突）
        for (size_t i = 0; i < fields.size(); ++i) fields[i].address = (int)i;
        // 多条不同配置才需要多帧头匹配（帧头+Length 不同帧头；TLV 组成员本就同配置）
        if (configs.size() > 1) s.framings = std::move(configs);
    } else if (proto) {
        protocolFramingFromComponent(*proto, framing, fields);
    }
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

    // 纵向排列（行距 72，与卡片高留 8px 呼吸）；字段名由绑定组件的复合卡片自显示
    const float rowH = 72.0f;
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
        Component nc = ComponentRegistry::createComponent(compType, proj.allocId("comp"));
        nc.name = sp.name;
        // 生成默认尺寸大于调色板默认：复合卡片要容纳左上字段名 + 内容区
        //（文本 170x60，值可维持 16-18px 字号；指示灯 56x64，灯体在标题下方留足）
        if (compType == std::string("Label")) {
            nc.frame.w = 176.0f;
            nc.frame.h = 64.0f;
        } else { // Lamp
            nc.frame.w = 56.0f;
            nc.frame.h = 64.0f;
        }
        nc.frame.x = x0;
        nc.frame.y = y0 + row * rowH;
        nc.z = page.components.empty() ? 0 : page.components.back().z + 1;
        nc.setProp("bindField", bind);
        page.components.push_back(std::move(nc));
        created.push_back(page.components.back().id);
        ++row;
    }
    return created;
}

void synthesizeImplicitBindings(Project& p) {
    // 运行时（生效数据源）的合并字段表决定隐式标签槽位——协议组会把组内字段重排，
    // 若按各协议内部序号合成，两个协议的字段0会共享同一槽位标签，AA55 帧便会
    // 同时驱动两个协议的显示组件
    std::map<std::pair<std::string, std::string>, int> effAddr; // (协议名,字段名)->槽位
    {
        FrameSourceSettings eff = frameSettingsFromProject(p);
        for (const auto& f : eff.fields)
            effAddr[{f.protoName, f.name}] = f.address;
    }

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

            // 隐式标签槽位以运行时为准（协议组重排后）；未生效的协议回退协议内序号
            int addr = f->address;
            auto ea = effAddr.find({protoName, fieldName});
            if (ea != effAddr.end()) addr = ea->second;

            // 隐式标签：同槽位复用；否则按字段创建（名字唯一化）
            Tag* t = nullptr;
            for (auto& tag : p.tags.all())
                if (tag.address == addr) {
                    t = &tag;
                    break;
                }
            if (!t) {
                Tag nt;
                nt.name = f->name;
                while (p.tags.find(nt.name))
                    nt.name = f->name + "@" + std::to_string(f->address); // 重名加槽位后缀
                nt.address = addr;
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
                    if (tag.address == addr) t = &tag;
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

} // namespace pv
