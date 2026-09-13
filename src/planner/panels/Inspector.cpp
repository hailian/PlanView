#include "planner/panels/Inspector.h"

#include "base/data/frame/FrameSourceSettings.h"
#include "base/model/ComponentRegistry.h"
#include "base/packet/PacketSpec.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "planner/PlannerContext.h"

namespace softg::planner::panels {

namespace {

// 规约字段类型候选（与 packet::FieldType 的可映射子集）
const char* kFieldTypes[] = {"u8",  "i8",   "u16",  "i16", "u32", "i32",
                             "f32", "f64",  "bool", "string", "enum"};
const int kFieldTypeCount = 11;

// 枚举字段编辑弹窗：字节宽度(1/2/4) + 值→名称行（存 f<i>.len / enumCount / e<j>.v|n）
void drawEnumEditor(Component& c, PlannerContext& ctx, const std::string& p) {
    int64_t w = props::asInt(c.propOr(p + "len", int64_t(1)));
    int width = (w == 2 || w == 4) ? (int)w : 1;
    ImGui::TextUnformatted("字节宽度");
    const int widths[3] = {1, 2, 4};
    for (int k = 0; k < 3; ++k) {
        if (k) ImGui::SameLine();
        bool sel = width == widths[k];
        std::string lbl = std::to_string(widths[k]) + "B";
        if (ImGui::RadioButton(lbl.c_str(), sel) && !sel) {
            ctx.doc.commit("枚举宽度");
            c.setProp(p + "len", int64_t(widths[k]));
        }
    }
    ImGui::Separator();
    int64_t count64 = props::asInt(c.propOr(p + "enumCount", int64_t(0)));
    int count = (int)std::clamp<int64_t>(count64, 0, 64);
    if (ImGui::Button("添加项")) {
        ctx.doc.commit("添加枚举项");
        std::string ep = p + "e" + std::to_string(count) + ".";
        c.setProp(ep + "v", int64_t(count));
        c.setProp(ep + "n", std::string("项" + std::to_string(count + 1)));
        c.setProp(p + "enumCount", int64_t(count + 1));
        ++count;
    }
    ImGui::SameLine();
    if (count > 0 && ImGui::Button("删除末尾")) {
        ctx.doc.commit("删除枚举项");
        c.setProp(p + "enumCount", int64_t(count - 1));
        --count;
    }
    if (count > 0 &&
        ImGui::BeginTable("enums", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();
        for (int j = 0; j < count; ++j) {
            std::string ep = p + "e" + std::to_string(j) + ".";
            ImGui::PushID(j);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            int64_t v = props::asInt(c.propOr(ep + "v", int64_t(0)));
            int iv = (int)v;
            if (ImGui::InputInt("##v", &iv, 0, 0)) {
                if (ImGui::IsItemActivated()) ctx.doc.commit("枚举值");
                c.setProp(ep + "v", int64_t(iv));
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            std::string nm = props::asString(c.propOr(ep + "n", std::string()));
            if (ImGui::InputText("##n", &nm)) {
                if (ImGui::IsItemActivated()) ctx.doc.commit("枚举名称");
                c.setProp(ep + "n", nm);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// 协议配置组件的规约字段编辑区：字段以 f<i>.* 索引属性存储（随组件快照进 undo/序列化）
void drawProtocolFields(Component& c, PlannerContext& ctx) {
    ImGui::Separator();
    ImGui::TextUnformatted("规约字段（帧 -> 标签槽位）");
    int64_t count64 = props::asInt(c.propOr("fieldCount", int64_t(0)));
    int count = (int)std::clamp<int64_t>(count64, 0, 64);

    if (ImGui::Button("添加字段")) {
        ctx.doc.commit("添加规约字段");
        std::string p = "f" + std::to_string(count) + ".";
        c.setProp(p + "name", std::string("字段" + std::to_string(count + 1)));
        c.setProp(p + "tagId", int64_t(count + 1));
        c.setProp(p + "offset", int64_t(0));
        c.setProp(p + "type", std::string("u16"));
        c.setProp(p + "bigEndian", true);
        c.setProp("fieldCount", int64_t(count + 1));
        ++count;
    }
    ImGui::SameLine();
    if (count > 0 && ImGui::Button("删除末尾")) {
        ctx.doc.commit("删除规约字段");
        c.setProp("fieldCount", int64_t(count - 1));
        --count;
    }
    bool tlv = props::asString(c.propOr("framingMode", std::string("TLV"))) == "TLV";
    ImGui::TextDisabled("%s", tlv ? "TLV：字段按槽位(T)匹配帧，偏移相对该帧负载 V"
                                  : "帧头+Length：偏移相对负载（帧头 + length 字段之后）");
    if (count == 0) {
        ImGui::TextDisabled("  (无字段：运行器仍可收帧并监视，但不驱动任何标签)");
        return;
    }

    // 表格式行编辑：列头作标签、控件填满列宽，避免行内控件互相挤压截断
    //（标签槽位由字段序号自动分配，不再编辑；隐式标签按槽位合成）
    int cols = tlv ? 5 : 4;
    if (!ImGui::BeginTable("pfields", cols, ImGuiTableFlags_SizingStretchProp |
                                             ImGuiTableFlags_RowBg))
        return;
    ImGui::TableSetupColumn("字段名", ImGuiTableColumnFlags_WidthStretch, 2.2f);
    if (tlv)
        ImGui::TableSetupColumn("槽位(hex)", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("偏移", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("长度/枚举", ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableHeadersRow();

    for (int i = 0; i < count; ++i) {
        std::string p = "f" + std::to_string(i) + ".";
        ImGui::PushID(i);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        std::string name = props::asString(c.propOr(p + "name", std::string("?")));
        if (ImGui::InputText("##n", &name)) {
            if (ImGui::IsItemActivated()) ctx.doc.commit("字段名");
            c.setProp(p + "name", name);
        }

        if (tlv) {
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            int64_t tagId = props::asInt(c.propOr(p + "tagId", int64_t(0)));
            int v = (int)tagId;
            // 槽位 = TLV 帧 T 值（报文字节），十六进制输入/显示；
            // 取值上限随 T 字节数：1B→0xFF、2B→0xFFFF、3B→0xFFFFFF、4B→0xFFFFFFFF
            int64_t maxTagId = props::asInt(
                c.propOr("tagBytes", int64_t(1)));
            maxTagId = (int64_t)1 << (int64_t)(8 * std::clamp<int64_t>(maxTagId, 1, 4));
            if (ImGui::InputInt("##t", &v, 0, 0, ImGuiInputTextFlags_CharsHexadecimal)) {
                if (ImGui::IsItemActivated()) ctx.doc.commit("字段槽位");
                uint32_t uv = (uint32_t)std::clamp<int64_t>(
                    (int64_t)v, 0, std::min<int64_t>(maxTagId - 1, 0xFFFFFFFF));
                c.setProp(p + "tagId", int64_t(uv));
            }
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        int64_t offset = props::asInt(c.propOr(p + "offset", int64_t(0)));
        int v = (int)offset;
        if (ImGui::InputInt("##o", &v, 0, 0)) {
            if (ImGui::IsItemActivated()) ctx.doc.commit("字段偏移");
            c.setProp(p + "offset", int64_t(std::clamp(v, 0, 4096)));
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        std::string typeName = props::asString(c.propOr(p + "type", std::string("u16")));
        if (ImGui::BeginCombo("##ty", typeName.c_str())) {
            for (int k = 0; k < kFieldTypeCount; ++k) {
                bool sel = typeName == kFieldTypes[k];
                if (ImGui::Selectable(kFieldTypes[k], sel) && !sel) {
                    ctx.doc.commit("字段类型");
                    c.setProp(p + "type", std::string(kFieldTypes[k]));
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        // 类型可能刚被修改，重新读取以决定本列
        typeName = props::asString(c.propOr(p + "type", std::string("u16")));

        // 长度/枚举：字符串=长度（字节）；枚举=编辑映射；其它=空
        ImGui::TableNextColumn();
        if (typeName == "string") {
            ImGui::SetNextItemWidth(-1);
            int len = (int)props::asInt(c.propOr(p + "len", int64_t(16)));
            if (ImGui::InputInt("##len", &len, 0, 0)) {
                if (ImGui::IsItemActivated()) ctx.doc.commit("字段长度");
                c.setProp(p + "len", int64_t(std::clamp(len, 1, 256)));
            }
        } else if (typeName == "enum") {
            if (ImGui::Button("枚举…")) ImGui::OpenPopup("枚举映射");
            ImGui::SetItemTooltip("编辑枚举映射（值 → 名称）与字节宽度");
            if (ImGui::BeginPopup("枚举映射")) {
                drawEnumEditor(c, ctx, p);
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// 数据源组件的「关联协议」动态下拉：候选 = 工程内全部协议配置组件名（跨页）
void drawProtocolSelector(Component& c, PlannerContext& ctx) {
    std::string cur = props::asString(c.propOr("protocol", std::string()));
    const char* preview = cur.empty() ? "(未关联)" : cur.c_str();
    if (ImGui::BeginCombo("关联协议", preview)) {
        if (ImGui::Selectable("(未关联)", cur.empty())) {
            ctx.doc.commit("取消关联协议");
            c.setProp("protocol", std::string());
        }
        for (const auto& pg : ctx.project().pages)
            for (const auto& pc : pg.components) {
                if (pc.typeId != "ProtocolConfig") continue;
                bool sel = pc.name == cur;
                if (ImGui::Selectable(pc.name.c_str(), sel) && !sel) {
                    ctx.doc.commit("关联协议");
                    c.setProp("protocol", pc.name);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
        ImGui::EndCombo();
    }
}

// 按 PropertySpec 生成单个类型化编辑器；返回是否有变更
bool editProperty(const PropertySpec& spec, PropertyValue& value) {
    bool changed = false;
    ImGui::PushID(spec.key.c_str());
    switch (spec.type) {
    case PropertyType::Bool: {
        bool b = props::asBool(value);
        if (ImGui::Checkbox(spec.label.c_str(), &b)) {
            value = b;
            changed = true;
        }
        break;
    }
    case PropertyType::Int: {
        // 直接键入的编辑框（无步进按钮）；越界输入按 spec 范围拉回
        int64_t v = props::asInt(value);
        int i = (int)std::clamp<int64_t>(v, INT_MIN, INT_MAX);
        if (ImGui::InputInt(spec.label.c_str(), &i, 0, 0)) {
            if (spec.minValue) i = std::max(i, (int)*spec.minValue);
            if (spec.maxValue) i = std::min(i, (int)*spec.maxValue);
            value = (int64_t)i;
            changed = true;
        }
        break;
    }
    case PropertyType::Double: {
        double d = props::asDouble(value);
        float f = (float)d;
        float speed = 0.05f * std::max(1.0f, std::abs(f) * 0.01f);
        if (ImGui::DragFloat(spec.label.c_str(), &f, speed,
                             spec.minValue ? (float)*spec.minValue : -FLT_MAX,
                             spec.maxValue ? (float)*spec.maxValue : FLT_MAX, "%.3f")) {
            value = (double)f;
            changed = true;
        }
        break;
    }
    case PropertyType::String: {
        std::string s = props::asString(value);
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", s.c_str());
        if (ImGui::InputText(spec.label.c_str(), buf, sizeof(buf))) {
            value = std::string(buf);
            changed = true;
        }
        break;
    }
    case PropertyType::Color: {
        uint32_t col = props::asColor(value);
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
        if (ImGui::ColorEdit4(spec.label.c_str(), &c.x,
                              ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs)) {
            value = ImGui::ColorConvertFloat4ToU32(c);
            changed = true;
        }
        break;
    }
    case PropertyType::Enum: {
        std::string cur = props::asString(value);
        if (ImGui::BeginCombo(spec.label.c_str(), cur.c_str())) {
            for (const auto& opt : spec.enumValues) {
                bool sel = opt == cur;
                if (ImGui::Selectable(opt.c_str(), sel) && !sel) {
                    value = opt;
                    changed = true;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        break;
    }
    case PropertyType::Vec2: {
        ImVec2 v = props::asVec2(value);
        if (ImGui::DragFloat2(spec.label.c_str(), &v.x)) {
            value = v;
            changed = true;
        }
        break;
    }
    }
    ImGui::PopID();
    return changed;
}

// undo 粒度：连续拖动类控件在"激活首帧"压栈（此刻值尚未修改），离散控件在有变更时压栈
void commitOnEdit(const PropertySpec& spec, bool changed, PlannerContext& ctx) {
    bool discrete = spec.type == PropertyType::Bool || spec.type == PropertyType::Enum ||
                    spec.type == PropertyType::String;
    if (ImGui::IsItemActivated() || (changed && discrete))
        ctx.doc.commit("属性 " + spec.label);
}

// 显示组件的「绑定协议字段」下拉：候选 = 工程内全部协议配置的字段。
// 选择后存组件 bindField（"协议名/字段名"），运行器合成隐式标签+绑定直接驱动
void drawFieldBinding(Component& c, PlannerContext& ctx) {
    ImGui::Separator();
    std::string cur = props::asString(c.propOr("bindField", std::string()));
    const char* preview = cur.empty() ? "(不绑定)" : cur.c_str();
    if (ImGui::BeginCombo("绑定协议字段", preview)) {
        if (ImGui::Selectable("(不绑定)", cur.empty()) && !cur.empty()) {
            ctx.doc.commit("取消绑定协议字段");
            c.setProp("bindField", std::string());
        }
        packet::FramingConfig fr;
        std::vector<TagField> fields;
        for (const auto& pg : ctx.project().pages)
            for (const auto& pc : pg.components) {
                if (pc.typeId != "ProtocolConfig") continue;
                protocolFramingFromComponent(pc, fr, fields);
                for (const auto& f : fields) {
                    // 存储格式 "协议名/字段名"（无空格；解析侧同时容忍带空格的历史数据）
                    std::string item = pc.name + "/" + f.name;
                    bool sel = item == cur;
                    if (ImGui::Selectable(item.c_str(), sel) && !sel) {
                        ctx.doc.commit("绑定协议字段");
                        c.setProp("bindField", item);
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(-> %s)", defaultBindableProperty(c));
}

} // namespace

void drawInspector(PlannerContext& ctx) {
    if (!ImGui::Begin("属性")) {
        ImGui::End();
        return;
    }
    Page* page = ctx.currentPagePtr();
    if (!page) {
        ImGui::TextUnformatted("没有页面");
        ImGui::End();
        return;
    }
    if (ctx.selection.empty()) {
        // ---- 页面属性 ----
        ImGui::TextUnformatted("页面属性（未选中组件）");
        ImGui::Separator();
        if (ImGui::InputText("页面名", &page->name)) ctx.doc.commit("页面改名");
        float size[2] = {page->size.x, page->size.y};
        if (ImGui::DragFloat2("尺寸", size, 1.0f, 100.0f, 8192.0f, "%.0f")) {
            if (ImGui::IsItemActivated()) ctx.doc.commit("页面尺寸");
            page->size = ImVec2(std::max(100.0f, size[0]), std::max(100.0f, size[1]));
        }
        ImGui::End();
        return;
    }

    Component* c = page->find(*ctx.selection.begin());
    if (!c) {
        ImGui::TextUnformatted("选中组件不在当前页");
        ImGui::End();
        return;
    }
    const auto* info = ComponentRegistry::instance().find(c->typeId);
    ImGui::Text("%s (%s)", c->name.c_str(), info ? info->displayName.c_str() : "?");
    ImGui::TextDisabled("id: %s", c->id.c_str());
    ImGui::Separator();

    // ---- 名称 ----
    if (ImGui::InputText("名称", &c->name))
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.doc.commit("改名");

    // ---- 几何与状态 ----
    float pos[2] = {c->frame.x, c->frame.y};
    float size[2] = {c->frame.w, c->frame.h};
    if (ImGui::DragFloat2("位置", pos, 1.0f, -8192.0f, 8192.0f, "%.0f")) {
        if (ImGui::IsItemActivated()) ctx.doc.commit("移动组件");
        c->frame.x = ctx.view.snap(pos[0]);
        c->frame.y = ctx.view.snap(pos[1]);
    }
    if (ImGui::DragFloat2("尺寸", size, 1.0f, 8.0f, 8192.0f, "%.0f")) {
        if (ImGui::IsItemActivated()) ctx.doc.commit("调整尺寸");
        c->frame.w = std::max(8.0f, size[0]);
        c->frame.h = std::max(8.0f, size[1]);
    }
    if (ImGui::Checkbox("可见", &c->visible)) ctx.doc.commit("可见性");
    if (ImGui::Checkbox("锁定", &c->locked)) ctx.doc.commit("锁定");

    // ---- 层级 ----
    ImGui::Separator();
    if (ImGui::Button("置顶")) ctx.nudgeZOrder(10000);
    ImGui::SameLine();
    if (ImGui::Button("上移")) ctx.nudgeZOrder(1);
    ImGui::SameLine();
    if (ImGui::Button("下移")) ctx.nudgeZOrder(-1);
    ImGui::SameLine();
    if (ImGui::Button("置底")) ctx.nudgeZOrder(-10000);

    // ---- 类型化属性 ----
    ImGui::Separator();
    ImGui::TextUnformatted("组件属性");
    if (info) {
        bool isDs = c->typeId == "DataSource";
        bool isProto = c->typeId == "ProtocolConfig";
        bool dsTcp = props::asString(c->propOr("transport", std::string("UDP"))) == "TCP";
        bool dsUdpClient =
            !dsTcp && props::asString(c->propOr("udpRole", std::string("服务端"))) == "客户端";
        bool dsTcpServer =
            dsTcp && props::asString(c->propOr("tcpRole", std::string("客户端"))) == "服务端";
        // 目标(host/remotePort) 仅客户端使用；本地端口(localPort) 仅服务端使用
        bool dsUseTarget = dsTcp ? !dsTcpServer : dsUdpClient;
        bool dsUseLocalPort = dsTcp ? dsTcpServer : !dsUdpClient;
        bool protoTlv =
            props::asString(c->propOr("framingMode", std::string("TLV"))) == "TLV";
        for (const auto& spec : info->properties) {
            if (isDs) {
                if (spec.key == "protocol") continue; // 动态下拉（候选为协议组件名）
                // 按传输方式与客户端/服务端只显示相关项，避免误配：
                // 角色项各自仅对应传输显示；客户端用 host:remotePort；服务端用 localPort
                if (spec.key == "udpRole" && dsTcp) continue;  // 仅 UDP
                if (spec.key == "tcpRole" && !dsTcp) continue; // 仅 TCP
                if ((spec.key == "host" || spec.key == "remotePort") && !dsUseTarget) continue;
                if (spec.key == "localPort" && !dsUseLocalPort) continue;
            }
            if (isProto) { // 拆帧项按模式互斥显示，避免误配
                bool tlvItem = spec.key == "tagBytes" || spec.key == "lenBytes" ||
                               spec.key == "bigEndian" || spec.key == "lenIncludesHeader";
                bool headerItem = spec.key == "headerHex" || spec.key == "lenOffset" ||
                                  spec.key == "lenBytesHeader" ||
                                  spec.key == "bigEndianHeader" || spec.key == "lenIncludesAll";
                if (tlvItem && !protoTlv) continue;
                if (headerItem && protoTlv) continue;
            }
            PropertyValue v = c->propOr(spec.key, spec.defaultValue);
            bool changed = editProperty(spec, v);
            commitOnEdit(spec, changed, ctx);
            if (changed) c->setProp(spec.key, v);
        }
        if (isDs)
            drawProtocolSelector(*c, ctx); // 关联协议（动态候选）
        if (isProto)
            drawProtocolFields(*c, ctx); // 规约字段列表（索引属性，自定义编辑）
        if (!isDs && !isProto)
            drawFieldBinding(*c, ctx); // 显示组件直接绑定协议字段
    } else {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.6f, 1), "未注册类型: %s", c->typeId.c_str());
    }

    ImGui::End();
}

} // namespace softg::planner::panels
