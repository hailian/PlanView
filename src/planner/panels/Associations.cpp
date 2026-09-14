#include "planner/panels/Associations.h"

#include "base/model/ComponentRegistry.h"
#include "base/serialize/JsonHelpers.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "planner/PlannerContext.h"

namespace pv::planner::panels {

namespace {

// "页面名/组件名" 下拉（跨页），返回是否变更
bool componentCombo(const char* label, const Project& p, ComponentId& id, bool allowEmpty) {
    bool changed = false;
    const Component* cur = p.findComponent(id);
    std::string curLabel;
    if (cur) {
        if (const Page* pg = p.findPageOfComponent(cur->id))
            curLabel = pg->name + "/" + cur->name;
    } else if (allowEmpty && id.empty()) {
        curLabel = "(无)";
    } else {
        curLabel = "?(已失效) " + id;
    }
    if (ImGui::BeginCombo(label, curLabel.c_str())) {
        if (allowEmpty && ImGui::Selectable("(无)", id.empty())) {
            id.clear();
            changed = true;
        }
        for (const auto& pg : p.pages) {
            ImGui::SeparatorText(pg.name.c_str());
            for (const auto& c : pg.components) {
                bool sel = c.id == id;
                if (ImGui::Selectable((c.name + " (" + c.typeId + ")").c_str(), sel)) {
                    id = c.id;
                    changed = true;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool tagCombo(const char* label, const TagDatabase& db, TagName& tag) {
    bool changed = false;
    const Tag* cur = db.find(tag);
    std::string curLabel = cur ? (cur->name + " @" + std::to_string(cur->address)) : ("?(不存在) " + tag);
    if (ImGui::BeginCombo(label, curLabel.c_str())) {
        for (const auto& t : db.all()) {
            bool sel = t.name == tag;
            if (ImGui::Selectable((t.name + " @" + std::to_string(t.address) + " " + t.comment).c_str(),
                                  sel)) {
                tag = t.name;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// PropertyValue 简易编辑（类型选择 + 对应控件）
bool valueEditor(const char* label, PropertyValue& v) {
    bool changed = false;
    const char* typeNames[] = {"bool", "int", "double", "string"};
    int cur = v.index() == 0 ? 0 : v.index() == 1 ? 1 : v.index() == 2 ? 2 : v.index() == 3 ? 3 : -1;
    if (cur < 0) {
        // Color/Vec2 不用于关联值，转成 string
        v = std::string();
        cur = 3;
    }
    ImGui::PushID(label);
    if (ImGui::BeginCombo("##vt", typeNames[cur])) {
        for (int i = 0; i < 4; ++i)
            if (ImGui::Selectable(typeNames[i], i == cur)) {
                switch (i) {
                case 0: v = false; break;
                case 1: v = (int64_t)0; break;
                case 2: v = 0.0; break;
                case 3: v = std::string(); break;
                }
                changed = true;
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    switch (cur) {
    case 0: {
        bool b = props::asBool(v);
        if (ImGui::Checkbox(label, &b)) { v = b; changed = true; }
        break;
    }
    case 1: {
        int i = (int)props::asInt(v);
        if (ImGui::InputInt(label, &i)) { v = (int64_t)i; changed = true; }
        break;
    }
    case 2: {
        float f = (float)props::asDouble(v);
        if (ImGui::InputFloat(label, &f, 0, 0, "%.4f")) { v = (double)f; changed = true; }
        break;
    }
    case 3: {
        std::string s = props::asString(v);
        if (ImGui::InputText(label, &s)) { v = s; changed = true; }
        break;
    }
    }
    ImGui::PopID();
    return changed;
}

const char* eventLabel(LinkageEvent e) {
    switch (e) {
    case LinkageEvent::Click: return "点击";
    case LinkageEvent::ValueChanged: return "值变化";
    case LinkageEvent::AlarmActive: return "告警触发";
    case LinkageEvent::AlarmCleared: return "告警恢复";
    }
    return "?";
}
const char* actionLabel(LinkageAction a) {
    switch (a) {
    case LinkageAction::SetProperty: return "设置属性";
    case LinkageAction::Navigate: return "页面跳转";
    case LinkageAction::ToggleVisible: return "显隐切换";
    case LinkageAction::SetTagValue: return "写标签值";
    case LinkageAction::Pulse: return "脉冲高亮";
    }
    return "?";
}

// ---- 编辑对话框（模态；ok 后写回） ----
struct EditSession {
    bool open = false;
    bool creating = false;
    size_t index = (size_t)-1;
    Association draft;
};

EditSession& session() {
    static EditSession s;
    return s;
}

void openEditor(PlannerContext& ctx, size_t index) {
    EditSession& s = session();
    s.open = true;
    s.creating = false;
    s.index = index;
    s.draft = ctx.project().associations[index];  // 拷贝草稿
}

void openCreator(PlannerContext& ctx, int kind) {
    EditSession& s = session();
    s.open = true;
    s.creating = true;
    s.index = (size_t)-1;
    switch (kind) {
    case 0: s.draft = DataBinding{ctx.project().allocId("a"), "", "value", ""}; break;
    case 1: s.draft = LinkageRule{ctx.project().allocId("a"), "", LinkageEvent::Click, "", LinkageAction::SetProperty, "", false}; break;
    case 2: s.draft = AlarmRule{ctx.project().allocId("a"), "", Comparator::GT, 0.0, AlarmSeverity::High, AlarmStyle::Flash, true, {}}; break;
    }
    // 注意: allocId 已推进 nextId；取消创建时会浪费一个 id，无害
}

void drawEditor(PlannerContext& ctx) {
    EditSession& s = session();
    if (!s.open) return;
    Project& p = ctx.project();

    const char* title = s.creating ? "新建关联" : "编辑关联";
    if (!ImGui::Begin(title, &s.open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    bool ok = false;

    if (auto* b = std::get_if<DataBinding>(&s.draft)) {
        ImGui::TextUnformatted("数据绑定: 组件属性 <- 数据标签");
        componentCombo("组件", p, b->component, false);
        // 属性名：取组件类型规格中的属性 + 常用伪属性
        if (const Component* c = p.findComponent(b->component)) {
            if (const auto* info = ComponentRegistry::instance().find(c->typeId)) {
                std::string cur = b->property;
                if (ImGui::BeginCombo("属性", cur.c_str())) {
                    for (const auto& spec : info->properties)
                        if (ImGui::Selectable(spec.key.c_str(), spec.key == cur)) b->property = spec.key;
                    ImGui::Separator();
                    if (ImGui::Selectable("visible", cur == "visible")) b->property = "visible";
                    ImGui::EndCombo();
                }
            }
        } else {
            ImGui::InputText("属性", &b->property);
        }
        tagCombo("标签", p.tags, b->tag);
        ok = !b->component.empty() && !b->property.empty() && p.tags.find(b->tag) != nullptr;
    } else if (auto* l = std::get_if<LinkageRule>(&s.draft)) {
        ImGui::TextUnformatted("组件联动: 源组件事件 -> 目标动作");
        componentCombo("源组件", p, l->source, false);
        // 事件
        if (ImGui::BeginCombo("事件", eventLabel(l->event))) {
            for (uint8_t i = 0; i <= (uint8_t)LinkageEvent::AlarmCleared; ++i) {
                LinkageEvent e = (LinkageEvent)i;
                if (ImGui::Selectable(eventLabel(e), e == l->event)) l->event = e;
            }
            ImGui::EndCombo();
        }
        // 动作
        if (ImGui::BeginCombo("动作", actionLabel(l->action))) {
            for (uint8_t i = 0; i <= (uint8_t)LinkageAction::Pulse; ++i) {
                LinkageAction a = (LinkageAction)i;
                if (ImGui::Selectable(actionLabel(a), a == l->action)) l->action = a;
            }
            ImGui::EndCombo();
        }
        // 动作相关参数
        switch (l->action) {
        case LinkageAction::SetProperty: {
            componentCombo("目标组件", p, l->target, false);
            const Component* c = p.findComponent(l->target);
            if (c) {
                if (const auto* info = ComponentRegistry::instance().find(c->typeId)) {
                    if (ImGui::BeginCombo("属性", (l->param.empty() ? "(选择)" : l->param).c_str())) {
                        for (const auto& spec : info->properties)
                            if (ImGui::Selectable((spec.key + " - " + spec.label).c_str(),
                                                  spec.key == l->param))
                                l->param = spec.key;
                        ImGui::Separator();
                        if (ImGui::Selectable("visible", l->param == "visible")) l->param = "visible";
                        ImGui::EndCombo();
                    }
                }
            }
            valueEditor("值", l->value);
            ok = !l->source.empty() && !l->target.empty() && !l->param.empty();
            break;
        }
        case LinkageAction::Navigate: {
            const Page* curPage = p.findPage(l->param);
            if (ImGui::BeginCombo("目标页面", curPage ? curPage->name.c_str() : "(选择)")) {
                for (const auto& pg : p.pages)
                    if (ImGui::Selectable(pg.name.c_str(), pg.id == l->param)) l->param = pg.id;
                ImGui::EndCombo();
            }
            ok = !l->source.empty() && p.findPage(l->param) != nullptr;
            break;
        }
        case LinkageAction::ToggleVisible:
        case LinkageAction::Pulse:
            componentCombo("目标组件", p, l->target, false);
            ok = !l->source.empty() && !l->target.empty();
            break;
        case LinkageAction::SetTagValue:
            tagCombo("标签", p.tags, l->param);
            valueEditor("值", l->value);
            ok = !l->source.empty() && p.tags.find(l->param) != nullptr;
            break;
        }
    } else if (auto* r = std::get_if<AlarmRule>(&s.draft)) {
        ImGui::TextUnformatted("阈值告警: 标签比较 -> 视觉告警");
        tagCombo("标签", p.tags, r->tag);
        const char* cmps[] = {">", ">=", "<", "<=", "==", "!="};
        if (ImGui::BeginCombo("比较", cmps[(int)r->cmp])) {
            for (int i = 0; i < 6; ++i)
                if (ImGui::Selectable(cmps[i], i == (int)r->cmp)) r->cmp = (Comparator)i;
            ImGui::EndCombo();
        }
        ImGui::InputDouble("阈值", &r->threshold, 0, 0, "%.4f");
        const char* sevs[] = {"低", "高", "严重"};
        if (ImGui::BeginCombo("严重度", sevs[(int)r->severity])) {
            for (int i = 0; i < 3; ++i)
                if (ImGui::Selectable(sevs[i], i == (int)r->severity)) r->severity = (AlarmSeverity)i;
            ImGui::EndCombo();
        }
        const char* styles[] = {"闪烁", "描边", "变色"};
        if (ImGui::BeginCombo("样式", styles[(int)r->style])) {
            for (int i = 0; i < 3; ++i)
                if (ImGui::Selectable(styles[i], i == (int)r->style)) r->style = (AlarmStyle)i;
            ImGui::EndCombo();
        }
        ImGui::Checkbox("锁存(需人工确认)", &r->latching);
        ImGui::TextUnformatted("受影响组件（为空 = 自动覆盖绑定该标签的全部组件）");
        // 多选：用临时集合 + 小列表
        static std::vector<ComponentId> picking;
        picking = r->components;
        if (ImGui::BeginListBox("##comps", ImVec2(-FLT_MIN, 5 * ImGui::GetTextLineHeight()))) {
            for (const auto& pg : p.pages)
                for (const auto& c : pg.components) {
                    bool sel = std::find(picking.begin(), picking.end(), c.id) != picking.end();
                    if (ImGui::Selectable((pg.name + "/" + c.name).c_str(), sel)) {
                        if (sel) picking.erase(std::find(picking.begin(), picking.end(), c.id));
                        else picking.push_back(c.id);
                        r->components = picking;
                    }
                }
            ImGui::EndListBox();
        }
        ok = p.tags.find(r->tag) != nullptr;
    }

    ImGui::Separator();
    if (ImGui::Button("确定", ImVec2(90, 0)) && ok) {
        if (s.creating) {
            ctx.doc.commit("新建关联");
            p.associations.push_back(s.draft);
        } else {
            ctx.doc.commit("编辑关联");
            p.associations[s.index] = s.draft;
        }
        s.open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(90, 0))) s.open = false;
    if (!ok) {
        ImGui::SameLine();
        ImGui::TextDisabled("填写完整后可确定");
    }
    ImGui::End();
}

} // namespace

void drawAssociations(PlannerContext& ctx) {
    if (!ImGui::Begin("关联关系")) {
        ImGui::End();
        return;
    }
    Project& p = ctx.project();

    static int filter = -1;  // -1 全部
    if (ImGui::Button("新建绑定")) openCreator(ctx, 0);
    ImGui::SameLine();
    if (ImGui::Button("新建联动")) openCreator(ctx, 1);
    ImGui::SameLine();
    if (ImGui::Button("新建告警")) openCreator(ctx, 2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    const char* filters[] = {"全部", "绑定", "联动", "告警"};
    if (ImGui::BeginCombo("##filter", filters[filter + 1])) {
        for (int i = -1; i <= 2; ++i)
            if (ImGui::Selectable(filters[i + 1], filter == i)) filter = i;
        ImGui::EndCombo();
    }

    auto compLabel = [&](const ComponentId& id) {
        const Component* c = p.findComponent(id);
        if (!c) return id + "(失效)";
        const Page* pg = p.findPageOfComponent(id);
        return (pg ? pg->name + "/" : "") + c->name;
    };

    if (ImGui::BeginTable("assocs", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("类型");
        ImGui::TableSetupColumn("源");
        ImGui::TableSetupColumn("关系");
        ImGui::TableSetupColumn("目标");
        ImGui::TableSetupColumn("操作");
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < p.associations.size(); ++i) {
            const Association& a = p.associations[i];
            int kind = std::holds_alternative<DataBinding>(a)   ? 0
                       : std::holds_alternative<LinkageRule>(a) ? 1
                                                                : 2;
            if (filter >= 0 && kind != filter) continue;
            ImGui::PushID(assocId(a).c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(filters[kind + 1], false, ImGuiSelectableFlags_SpanAllColumns)) {
                if (ImGui::IsMouseDoubleClicked(0)) openEditor(ctx, i);
                else {
                    // 单击跳转到源组件
                    if (auto* b = std::get_if<DataBinding>(&a)) {
                        ctx.focusComponent = b->component;
                        ctx.focusRequested = true;
                    } else if (auto* l = std::get_if<LinkageRule>(&a)) {
                        ctx.focusComponent = l->source;
                        ctx.focusRequested = true;
                    }
                }
            }
            ImGui::TableNextColumn();
            if (auto* b = std::get_if<DataBinding>(&a)) {
                ImGui::TextUnformatted(compLabel(b->component).c_str());
                ImGui::TableNextColumn();
                ImGui::Text(".%s <-", b->property.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(b->tag.c_str());
            } else if (auto* l = std::get_if<LinkageRule>(&a)) {
                ImGui::TextUnformatted(compLabel(l->source).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s ->", eventLabel(l->event));
                ImGui::TableNextColumn();
                switch (l->action) {
                case LinkageAction::SetProperty:
                    ImGui::Text("%s.%s=%s", compLabel(l->target).c_str(), l->param.c_str(),
                                jsonx::toJson(l->value).dump().c_str());
                    break;
                case LinkageAction::Navigate: {
                    const Page* pg = p.findPage(l->param);
                    ImGui::Text("跳转 %s", pg ? pg->name.c_str() : l->param.c_str());
                    break;
                }
                case LinkageAction::ToggleVisible:
                    ImGui::Text("显隐 %s", compLabel(l->target).c_str());
                    break;
                case LinkageAction::SetTagValue:
                    ImGui::Text("%s=%s", l->param.c_str(), jsonx::toJson(l->value).dump().c_str());
                    break;
                case LinkageAction::Pulse:
                    ImGui::Text("脉冲 %s", compLabel(l->target).c_str());
                    break;
                }
            } else if (auto* r = std::get_if<AlarmRule>(&a)) {
                ImGui::TextUnformatted(r->tag.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s %.3g", jsonx::comparatorToString(r->cmp).c_str(), r->threshold);
                ImGui::TableNextColumn();
                ImGui::Text("%zu 个组件", r->components.size());
            }
            // 行内删除
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.2f, 0.2f, 0.7f));
            if (ImGui::SmallButton("删")) {
                ctx.doc.commit("删除关联");
                p.associations.erase(p.associations.begin() + i);
                ImGui::PopStyleColor();
                ImGui::PopID();
                break;  // 迭代器失效
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("双击行编辑; 单击定位源组件; 拖动表头调列宽");
    drawEditor(ctx);
    ImGui::End();
}

} // namespace pv::planner::panels
