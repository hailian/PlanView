#include "base/serialize/ProjectJson.h"

#include <fstream>

#include "base/model/ComponentRegistry.h"
#include "base/packet/HexUtil.h"
#include "base/serialize/JsonHelpers.h"

namespace softg::projio {

using jsonx::Json;
using jsonx::toJson;
using jsonx::fromJson;

// ---- 子对象 -> json ----

static Json componentToJson(const Component& c) {
    Json j;
    j["id"] = c.id;
    j["type"] = c.typeId;
    j["name"] = c.name;
    j["x"] = c.frame.x;
    j["y"] = c.frame.y;
    j["w"] = c.frame.w;
    j["h"] = c.frame.h;
    j["z"] = c.z;
    j["visible"] = c.visible;
    j["locked"] = c.locked;
    Json props = Json::object();
    for (const auto& [key, value] : c.props)  // std::map 按 key 排序，确定性输出
        props[key] = toJson(value);
    j["properties"] = std::move(props);
    return j;
}

static Json pageToJson(const Page& p) {
    Json j;
    j["id"] = p.id;
    j["name"] = p.name;
    j["size"] = Json::array({p.size.x, p.size.y});
    j["background"] = jsonx::colorToJson(p.background);
    Json comps = Json::array();
    for (const auto& c : p.components)
        comps.push_back(componentToJson(c));
    j["components"] = std::move(comps);
    return j;
}

static Json tagToJson(const Tag& t) {
    Json j;
    j["name"] = t.name;
    j["type"] = tagDataTypeName(t.type);
    j["address"] = t.address;
    j["scale"] = t.scale;
    j["offset"] = t.offset;
    j["comment"] = t.comment;
    return j;
}

static Json associationToJson(const Association& a) {
    Json j;
    if (auto* b = std::get_if<DataBinding>(&a)) {
        j["kind"] = "dataBinding";
        j["id"] = b->id;
        j["component"] = b->component;
        j["property"] = b->property;
        j["tag"] = b->tag;
    } else if (auto* l = std::get_if<LinkageRule>(&a)) {
        j["kind"] = "linkage";
        j["id"] = l->id;
        j["source"] = l->source;
        j["event"] = jsonx::linkageEventToString(l->event);
        j["target"] = l->target;
        j["action"] = jsonx::linkageActionToString(l->action);
        j["param"] = l->param;
        j["value"] = l->value.valueless_by_exception() ? Json() : toJson(l->value);
    } else if (auto* r = std::get_if<AlarmRule>(&a)) {
        j["kind"] = "alarmRule";
        j["id"] = r->id;
        j["tag"] = r->tag;
        j["comparator"] = jsonx::comparatorToString(r->cmp);
        j["threshold"] = r->threshold;
        j["severity"] = jsonx::severityToString(r->severity);
        j["style"] = jsonx::alarmStyleToString(r->style);
        j["latching"] = r->latching;
        Json comps = Json::array();
        for (const auto& id : r->components) comps.push_back(id);
        j["components"] = std::move(comps);
    }
    return j;
}

static Json projectToJson(const Project& p) {
    Json j;
    j["schemaVersion"] = p.schemaVersion;
    j["name"] = p.name;
    j["nextId"] = p.nextId;
    Json settings;
    Json tcp;
    tcp["host"] = p.settings.tcp.host;
    tcp["port"] = p.settings.tcp.port;
    tcp["pollMs"] = p.settings.tcp.pollMs;
    settings["tcp"] = std::move(tcp);
    if (p.settings.frame.enabled) { // 帧数据源：仅启用时写入（缺省字段向后兼容旧工程）
        const FrameSourceSettings& fr = p.settings.frame;
        Json jf;
        jf["udp"] = fr.udp;
        jf["udpClient"] = fr.udpClient;
        jf["tcpClient"] = fr.tcpClient;
        jf["host"] = fr.host;
        jf["remotePort"] = fr.remotePort;
        jf["localPort"] = fr.localPort;
        Json jm;
        jm["mode"] = fr.framing.mode == packet::FrameMode::Tlv ? "tlv" : "headerLength";
        jm["tagBytes"] = fr.framing.tagBytes;
        jm["lenBytes"] = fr.framing.lenBytes;
        jm["bigEndian"] = fr.framing.bigEndian;
        jm["lenIncludesHeader"] = fr.framing.lenIncludesHeader;
        jm["headerHex"] = packet::bytesToHex(fr.framing.header);
        jm["lenOffset"] = fr.framing.lenOffset;
        jm["lenBytesHeader"] = fr.framing.lenBytesHeader;
        jm["bigEndianHeader"] = fr.framing.bigEndianHeader;
        jm["lenIncludesAll"] = fr.framing.lenIncludesAll;
        jm["maxFrameLen"] = fr.framing.maxFrameLen;
        jf["framing"] = std::move(jm);
        Json fields = Json::array();
        for (const auto& f : fr.fields) {
            fields.push_back(Json{{"name", f.name},
                                  {"tagId", f.tagId},
                                  {"offset", f.offset},
                                  {"type", packet::fieldTypeToString(f.type)},
                                  {"bigEndian", f.bigEndian}});
        }
        jf["fields"] = std::move(fields);
        settings["frame"] = std::move(jf);
    }
    j["settings"] = std::move(settings);

    Json pages = Json::array();
    for (const auto& pg : p.pages) pages.push_back(pageToJson(pg));
    j["pages"] = std::move(pages);

    Json tags = Json::array();
    for (const auto& t : p.tags.all()) tags.push_back(tagToJson(t));
    j["tags"] = std::move(tags);

    Json assocs = Json::array();
    for (const auto& a : p.associations) assocs.push_back(associationToJson(a));
    j["associations"] = std::move(assocs);
    return j;
}

// ---- json -> 子对象 ----

static bool parseComponent(const Json& j, Component& c, std::string& err) {
    if (!j.is_object() || !j.contains("id") || !j.contains("type")) {
        err = "组件缺少 id/type 字段";
        return false;
    }
    c.id = j.at("id").get<std::string>();
    c.typeId = j.at("type").get<std::string>();
    c.name = j.value("name", c.typeId);
    c.frame = Rect{j.value("x", 0.0f), j.value("y", 0.0f),
                   j.value("w", 100.0f), j.value("h", 40.0f)};
    c.z = j.value("z", 0);
    c.visible = j.value("visible", true);
    c.locked = j.value("locked", false);
    if (j.contains("properties") && j["properties"].is_object()) {
        const ComponentTypeInfo* info = ComponentRegistry::instance().find(c.typeId);
        for (auto it = j["properties"].begin(); it != j["properties"].end(); ++it) {
            // 已知属性按规格默认值兜底，未知属性原样保留
            PropertyValue v = it->is_null() ? PropertyValue{} : fromJson(*it, nullptr);
            if (v.valueless_by_exception()) v = PropertyValue{};
            const PropertySpec* spec = nullptr;
            if (info)
                for (const auto& s : info->properties)
                    if (s.key == it.key()) { spec = &s; break; }
            if (v.index() == 5 /*Vec2*/ && info && !spec) v = PropertyValue{}; // 无法识别
            c.props[it.key()] = std::move(v);
        }
        // 补齐规格中缺失的默认属性（老版本文件向前兼容）
        if (info)
            for (const auto& s : info->properties)
                c.props.try_emplace(s.key, s.defaultValue);
    }
    return true;
}

static bool parsePage(const Json& j, Page& p, std::string& err) {
    if (!j.is_object() || !j.contains("id")) {
        err = "页面缺少 id 字段";
        return false;
    }
    p.id = j.at("id").get<std::string>();
    p.name = j.value("name", p.id);
    if (j.contains("size") && j["size"].is_array() && j["size"].size() == 2) {
        p.size = ImVec2(j["size"][0].get<float>(), j["size"][1].get<float>());
    }
    if (j.contains("background"))
        p.background = jsonx::jsonToColor(j["background"].get<std::string>(), p.background);
    if (j.contains("components") && j["components"].is_array()) {
        for (const auto& jc : j["components"]) {
            Component c;
            if (!parseComponent(jc, c, err)) {
                err = "页面 " + p.name + ": " + err;
                return false;
            }
            p.components.push_back(std::move(c));
        }
        p.sortComponents();
    }
    return true;
}

static bool parseTag(const Json& j, Tag& t, std::string& err) {
    if (!j.is_object() || !j.contains("name")) {
        err = "标签缺少 name 字段";
        return false;
    }
    t.name = j.at("name").get<std::string>();
    auto type = tagDataTypeFromName(j.value("type", "uint16"));
    if (!type) { err = "未知标签类型: " + j.value("type", ""); return false; }
    t.type = *type;

    int addr = j.value("address", 0);
    if (addr < 0 || addr > 65535) { err = "标签 " + t.name + " 槽位越界"; return false; }
    t.address = addr;
    t.scale = j.value("scale", 1.0);
    t.offset = j.value("offset", 0.0);
    t.comment = j.value("comment", "");
    return true;
}

static bool parseAssociation(const Json& j, Association& out, std::string& err) {
    if (!j.is_object() || !j.contains("kind")) {
        err = "关联缺少 kind 字段";
        return false;
    }
    std::string kind = j.at("kind").get<std::string>();
    if (kind == "dataBinding") {
        DataBinding b;
        b.id = j.at("id").get<std::string>();
        b.component = j.value("component", "");
        b.property = j.value("property", "");
        b.tag = j.value("tag", "");
        out = std::move(b);
        return true;
    }
    if (kind == "linkage") {
        LinkageRule l;
        l.id = j.at("id").get<std::string>();
        l.source = j.value("source", "");
        l.event = jsonx::linkageEventFromString(j.value("event", "click"), LinkageEvent::Click);
        l.target = j.value("target", "");
        l.action = jsonx::linkageActionFromString(j.value("action", "setProperty"),
                                                  LinkageAction::SetProperty);
        l.param = j.value("param", "");
        if (j.contains("value") && !j["value"].is_null())
            l.value = fromJson(j["value"], nullptr);
        out = std::move(l);
        return true;
    }
    if (kind == "alarmRule") {
        AlarmRule r;
        r.id = j.at("id").get<std::string>();
        r.tag = j.value("tag", "");
        r.cmp = jsonx::comparatorFromString(j.value("comparator", ">"), Comparator::GT);
        r.threshold = j.value("threshold", 0.0);
        r.severity = jsonx::severityFromString(j.value("severity", "high"), AlarmSeverity::High);
        r.style = jsonx::alarmStyleFromString(j.value("style", "flash"), AlarmStyle::Flash);
        r.latching = j.value("latching", true);
        if (j.contains("components") && j["components"].is_array())
            for (const auto& jc : j["components"])
                if (jc.is_string()) r.components.push_back(jc.get<std::string>());
        out = std::move(r);
        return true;
    }
    err = "未知关联类型: " + kind;
    return false;
}

static bool jsonToProject(const Json& j, Project& p, std::string& err) {
    if (!j.is_object()) { err = "根节点不是对象"; return false; }
    if (!j.contains("schemaVersion")) { err = "缺少 schemaVersion"; return false; }
    int v = j.at("schemaVersion").get<int>();
    if (v > kCurrentSchemaVersion) {
        err = "配置版本过高 (v" + std::to_string(v) +
              ")，请升级本软件后再打开（当前支持 v" + std::to_string(kCurrentSchemaVersion) + "）";
        return false;
    }
    p.schemaVersion = v;
    p.name = j.value("name", "未命名工程");
    p.nextId = j.value("nextId", (uint64_t)1);

    if (j.contains("settings")) {
        const Json& s = j["settings"];
        if (s.contains("tcp")) {
            const Json& t = s["tcp"];
            p.settings.tcp.host = t.value("host", p.settings.tcp.host);
            p.settings.tcp.port = t.value("port", p.settings.tcp.port);
            p.settings.tcp.pollMs = t.value("pollMs", p.settings.tcp.pollMs);
        }
        if (s.contains("frame")) { // 帧数据源（可选；旧工程无此字段）
            const Json& f = s["frame"];
            FrameSourceSettings& fr = p.settings.frame;
            fr.enabled = f.value("enabled", true); // 有 frame 段即视为启用
            fr.udp = f.value("udp", fr.udp);
            fr.udpClient = f.value("udpClient", fr.udpClient);
            fr.tcpClient = f.value("tcpClient", fr.tcpClient);
            fr.host = f.value("host", fr.host);
            fr.remotePort = f.value("remotePort", fr.remotePort);
            fr.localPort = f.value("localPort", fr.localPort);
            if (f.contains("framing")) {
                const Json& m = f["framing"];
                packet::FrameMode mode = m.value("mode", "tlv") == "tlv"
                                             ? packet::FrameMode::Tlv
                                             : packet::FrameMode::HeaderLength;
                fr.framing.mode = mode;
                fr.framing.tagBytes = m.value("tagBytes", fr.framing.tagBytes);
                fr.framing.lenBytes = m.value("lenBytes", fr.framing.lenBytes);
                fr.framing.bigEndian = m.value("bigEndian", fr.framing.bigEndian);
                fr.framing.lenIncludesHeader =
                    m.value("lenIncludesHeader", fr.framing.lenIncludesHeader);
                std::vector<uint8_t> header;
                std::string herr;
                if (packet::hexToBytes(m.value("headerHex", "AA 55"), header, herr) &&
                    !header.empty())
                    fr.framing.header = header;
                fr.framing.lenOffset = m.value("lenOffset", fr.framing.lenOffset);
                fr.framing.lenBytesHeader = m.value("lenBytesHeader", fr.framing.lenBytesHeader);
                fr.framing.bigEndianHeader = m.value("bigEndianHeader", fr.framing.bigEndianHeader);
                fr.framing.lenIncludesAll = m.value("lenIncludesAll", fr.framing.lenIncludesAll);
                fr.framing.maxFrameLen = m.value("maxFrameLen", fr.framing.maxFrameLen);
            }
            if (f.contains("fields")) {
                fr.fields.clear();
                for (const auto& jfi : f["fields"]) {
                    TagField tf;
                    tf.name = jfi.value("name", std::string("字段"));
                    tf.tagId = jfi.value("tagId", 0);
                    tf.offset = jfi.value("offset", 0);
                    std::string typeName = jfi.value("type", "u16");
                    if (!packet::fieldTypeFromString(typeName, tf.type))
                        tf.type = packet::FieldType::U16;
                    if (int n = packet::fieldTypeBytes(tf.type)) tf.bytes = n;
                    tf.bigEndian = jfi.value("bigEndian", true);
                    tf.address = 0; // 槽位由字段序号自动分配（address 已废弃，读时忽略）
                    fr.fields.push_back(std::move(tf));
                }
            }
        }
    }

    if (j.contains("pages")) {
        for (const auto& jp : j["pages"]) {
            Page pg;
            if (!parsePage(jp, pg, err)) return false;
            p.pages.push_back(std::move(pg));
        }
    }
    if (j.contains("tags")) {
        for (const auto& jt : j["tags"]) {
            Tag t;
            if (!parseTag(jt, t, err)) return false;
            std::string addErr;
            if (!p.tags.add(std::move(t), addErr)) { err = addErr; return false; }
        }
    }
    if (j.contains("associations")) {
        for (const auto& ja : j["associations"]) {
            Association a;
            if (!parseAssociation(ja, a, err)) return false;
            p.associations.push_back(std::move(a));
        }
    }
    return true;
}

// ---- 对外接口 ----

std::string dump(const Project& project) { return projectToJson(project).dump(2); }

bool parse(const std::string& text, Project& project, std::string& err) {
    Json j;
    try {
        j = Json::parse(text);
    } catch (const Json::parse_error& e) {
        err = std::string("JSON 解析失败: ") + e.what();
        return false;
    }
    try {
        Project fresh;  // 解析中途失败不污染原工程
        if (!jsonToProject(j, fresh, err)) return false;
        project = std::move(fresh);
        return true;
    } catch (const std::exception& e) {  // 缺字段/类型不符（.at 抛出）
        err = std::string("配置结构错误: ") + e.what();
        return false;
    }
}

bool save(const std::string& path, const Project& project, std::string& err) {
    try {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) { err = "无法打开文件写入: " + path; return false; }
        f << dump(project);
        if (!f.good()) { err = "写入失败: " + path; return false; }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool load(const std::string& path, Project& project, std::string& err) {
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) { err = "无法打开文件: " + path; return false; }
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return parse(text, project, err);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

} // namespace softg::projio
