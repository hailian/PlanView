#include "planner/panels/Inspector.h"

#include "base/model/ComponentRegistry.h"
#include "base/packet/PacketSpec.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "planner/PlannerContext.h"

namespace softg::planner::panels {

namespace {

// 规约字段类型候选（与 packet::FieldType 的可映射子集）
const char* kFieldTypes[] = {"u8", "i8", "u16", "i16", "u32", "i32", "f32", "f64"};
const int kFieldTypeCount = 8;

// DataSource 组件的规约字段编辑区：字段以 f<i>.* 索引属性存储（随组件快照进 undo/序列化）
void drawDataSourceFields(Component& c, PlannerContext& ctx) {
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
        c.setProp(p + "address", int64_t(count));
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
                                  : "帧头+Length：偏移相对整帧首");
    if (count == 0) {
        ImGui::TextDisabled("  (无字段：运行器仍可收帧并监视，但不驱动任何标签)");
        return;
    }

    float w = ImGui::GetFontSize();
    for (int i = 0; i < count; ++i) {
        std::string p = "f" + std::to_string(i) + ".";
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(w * 4.0f);
        std::string name = props::asString(c.propOr(p + "name", std::string("?")));
        if (ImGui::InputText("##n", &name)) {
            if (ImGui::IsItemActivated()) ctx.doc.commit("字段名");
            c.setProp(p + "name", name);
        }
        ImGui::SameLine();
        if (tlv) {
            ImGui::SetNextItemWidth(w * 1.8f);
            int64_t tagId = props::asInt(c.propOr(p + "tagId", int64_t(0)));
            int v = (int)tagId;
            if (ImGui::InputInt("槽位##t", &v, 0, 0)) {
                if (ImGui::IsItemActivated()) ctx.doc.commit("字段槽位");
                c.setProp(p + "tagId", int64_t(std::clamp(v, 0, 255)));
            }
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(w * 1.8f);
        int64_t offset = props::asInt(c.propOr(p + "offset", int64_t(0)));
        int v = (int)offset;
        if (ImGui::InputInt("偏移##o", &v, 0, 0)) {
            if (ImGui::IsItemActivated()) ctx.doc.commit("字段偏移");
            c.setProp(p + "offset", int64_t(std::clamp(v, 0, 4096)));
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w * 2.2f);
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
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w * 1.8f);
        int64_t addr = props::asInt(c.propOr(p + "address", int64_t(0)));
        int a = (int)addr;
        if (ImGui::InputInt("地址##a", &a, 0, 0)) {
            if (ImGui::IsItemActivated()) ctx.doc.commit("字段标签地址");
            c.setProp(p + "address", int64_t(std::clamp(a, 0, 65535)));
        }
        ImGui::PopID();
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
        for (const auto& spec : info->properties) {
            // 数据源组件的 TCP 拆帧项仅在对应模式下有意义，切换显示避免误配
            if (c->typeId == "DataSource") {
                bool tlv = props::asString(c->propOr("framingMode", std::string("TLV"))) == "TLV";
                bool isTlvItem = spec.key == "tagBytes" || spec.key == "lenBytes" ||
                                 spec.key == "bigEndian" || spec.key == "lenIncludesHeader";
                bool isHeaderItem = spec.key == "headerHex" || spec.key == "lenOffset" ||
                                    spec.key == "lenBytesHeader" || spec.key == "bigEndianHeader" ||
                                    spec.key == "lenIncludesAll";
                if (isTlvItem && !tlv) continue;
                if (isHeaderItem && tlv) continue;
                if (spec.key == "localPort" &&
                    props::asString(c->propOr("transport", std::string("UDP"))) == "TCP")
                    continue; // 本地端口仅 UDP 使用
            }
            PropertyValue v = c->propOr(spec.key, spec.defaultValue);
            bool changed = editProperty(spec, v);
            commitOnEdit(spec, changed, ctx);
            if (changed) c->setProp(spec.key, v);
        }
        if (c->typeId == "DataSource")
            drawDataSourceFields(*c, ctx); // 规约字段列表（索引属性，自定义编辑）
    } else {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.6f, 1), "未注册类型: %s", c->typeId.c_str());
    }

    ImGui::End();
}

} // namespace softg::planner::panels
