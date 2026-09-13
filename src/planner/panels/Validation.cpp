#include "planner/panels/Validation.h"

#include "imgui.h"
#include "planner/PlannerContext.h"

namespace softg::planner::panels {

void drawValidation(PlannerContext& ctx) {
    if (!ImGui::Begin("校验")) {
        ImGui::End();
        return;
    }
    Project& p = ctx.project();
    struct Issue {
        std::string text;
        ComponentId jumpTo;  // 非空时双击跳转
        bool jumpable = false;
    };
    std::vector<Issue> issues;

    auto checkComponent = [&](const ComponentId& id, const std::string& what) {
        if (!p.findComponent(id)) issues.push_back({what + " 引用了不存在的组件: " + id, id, true});
    };

    for (const auto& a : p.associations) {
        if (auto* b = std::get_if<DataBinding>(&a)) {
            checkComponent(b->component, "数据绑定");
            if (!p.tags.find(b->tag))
                issues.push_back({"数据绑定引用了不存在的标签: " + b->tag, "", false});
        } else if (auto* l = std::get_if<LinkageRule>(&a)) {
            checkComponent(l->source, "联动源");
            if (l->action == LinkageAction::Navigate) {
                if (!p.findPage(l->param))
                    issues.push_back({"联动跳转目标页面不存在: " + l->param, "", false});
            } else if (l->action == LinkageAction::SetTagValue) {
                if (!p.tags.find(l->param))
                    issues.push_back({"联动写值目标标签不存在: " + l->param, "", false});
            } else {
                if (l->target.empty())
                    issues.push_back({"联动缺少目标组件", l->source, true});
                else
                    checkComponent(l->target, "联动目标");
            }
        } else if (auto* r = std::get_if<AlarmRule>(&a)) {
            if (!p.tags.find(r->tag))
                issues.push_back({"告警规则引用了不存在的标签: " + r->tag, "", false});
            for (const auto& cid : r->components)
                checkComponent(cid, "告警受影响");
        }
    }
    // 无任何绑定的标签（提示级）
    for (const auto& t : p.tags.all()) {
        bool used = false;
        for (const auto& a : p.associations) {
            if (auto* b = std::get_if<DataBinding>(&a); b && b->tag == t.name) used = true;
            if (auto* r = std::get_if<AlarmRule>(&a); r && r->tag == t.name) used = true;
            if (auto* l = std::get_if<LinkageRule>(&a); l && l->param == t.name) used = true;
        }
        if (!used)
            issues.push_back({"标签未被任何关联引用: " + t.name + " @" + std::to_string(t.address), "",
                              false});
    }

    // 数据源组件全局唯一：多个时仅第一个生效（findComponentByType 语义）
    {
        int dsCount = 0;
        const Component* firstDs = nullptr;
        for (const auto& pg : p.pages)
            for (const auto& c : pg.components)
                if (c.typeId == "DataSource") {
                    if (!firstDs) firstDs = &c;
                    ++dsCount;
                }
        if (dsCount > 1)
            issues.push_back({"数据源组件超过一个（仅第一个生效）: " +
                                  std::to_string(dsCount) + " 个",
                              firstDs ? firstDs->id : "", true});
    }

    if (issues.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "未发现问题 (%d 项关联, %d 个标签)",
                           (int)p.associations.size(), (int)p.tags.all().size());
    } else {
        ImGui::Text("发现 %d 项问题:", (int)issues.size());
        int n = 0;
        for (const auto& iss : issues) {
            ImGui::PushID(n++);
            bool jump = iss.jumpable && !iss.jumpTo.empty();
            if (ImGui::Selectable(iss.text.c_str(), false,
                                  jump ? ImGuiSelectableFlags_AllowDoubleClick : 0)) {
                if (jump && ImGui::IsMouseDoubleClicked(0)) {
                    ctx.focusComponent = iss.jumpTo;
                    ctx.focusRequested = true;
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::TextDisabled("双击可定位的问题行跳转到相关组件");
    ImGui::End();
}

} // namespace softg::planner::panels
