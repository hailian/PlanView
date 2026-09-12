#include "planner/panels/Inspector.h"

#include "base/model/ComponentRegistry.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "planner/PlannerContext.h"

namespace softg::planner::panels {

namespace {

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
        int64_t v = props::asInt(value);
        int i = (int)std::clamp<int64_t>(v, INT_MIN, INT_MAX);
        if (ImGui::DragInt(spec.label.c_str(), &i, 1.0f,
                           spec.minValue ? (int)*spec.minValue : 0,
                           spec.maxValue ? (int)*spec.maxValue : 0)) {
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
            PropertyValue v = c->propOr(spec.key, spec.defaultValue);
            bool changed = editProperty(spec, v);
            commitOnEdit(spec, changed, ctx);
            if (changed) c->setProp(spec.key, v);
        }
    } else {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.6f, 1), "未注册类型: %s", c->typeId.c_str());
    }

    ImGui::End();
}

} // namespace softg::planner::panels
