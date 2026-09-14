#include "planner/panels/Tags.h"

#include <algorithm>

#include "imgui.h"
#include "imgui_stdlib.h"
#include "planner/PlannerContext.h"

namespace pv::planner::panels {

namespace {

const char* tagTypeName(TagDataType t) { return tagDataTypeName(t); }

} // namespace

void drawTags(PlannerContext& ctx) {
    if (!ImGui::Begin("标签库")) {
        ImGui::End();
        return;
    }
    TagDatabase& db = ctx.project().tags;

    if (ImGui::Button("新建标签")) {
        ctx.doc.commit("新建标签");
        Tag t;
        t.name = "Tag" + std::to_string(db.all().size() + 1);
        t.address = (int)db.all().size();
        std::string err;
        db.add(std::move(t), err);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("槽位 = TCP 数据服务器的数据索引 (0..65535)");

    static int selectedRow = -1;
    if (ImGui::BeginTable("tags", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("名称");
        ImGui::TableSetupColumn("类型");
        ImGui::TableSetupColumn("槽位");
        ImGui::TableSetupColumn("scale");
        ImGui::TableSetupColumn("offset");
        ImGui::TableSetupColumn("备注");
        ImGui::TableHeadersRow();

        for (size_t row = 0; row < db.all().size(); ++row) {
            Tag& t = db.all()[row];
            ImGui::PushID(t.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(t.name.c_str(), (int)row == selectedRow,
                                  ImGuiSelectableFlags_SpanAllColumns))
                selectedRow = (int)row;
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##type", tagTypeName(t.type))) {
                for (uint8_t i = 0; i <= (uint8_t)TagDataType::Float32; ++i) {
                    TagDataType cand = (TagDataType)i;
                    if (ImGui::Selectable(tagTypeName(cand), cand == t.type)) {
                        ctx.doc.commit("标签类型");
                        t.type = cand;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            {
                int addr = t.address;
                if (ImGui::InputInt("##addr", &addr, 1, 1)) {
                    if (ImGui::IsItemActivated()) ctx.doc.commit("标签槽位");
                    t.address = std::clamp(addr, 0, 65535);
                }
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            {
                float scale = (float)t.scale;
                if (ImGui::DragFloat("##scale", &scale, 0.01f, -1e6f, 1e6f, "%.4f")) {
                    if (ImGui::IsItemActivated()) ctx.doc.commit("标签 scale");
                    t.scale = scale;
                }
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            {
                float offset = (float)t.offset;
                if (ImGui::DragFloat("##offset", &offset, 0.01f, -1e6f, 1e6f, "%.4f")) {
                    if (ImGui::IsItemActivated()) ctx.doc.commit("标签 offset");
                    t.offset = offset;
                }
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##comment", &t.comment))
                if (ImGui::IsItemDeactivatedAfterEdit()) ctx.doc.commit("标签备注");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // 删除选中行
    if (selectedRow >= 0 && selectedRow < (int)db.all().size()) {
        const Tag& t = db.all()[selectedRow];
        // 引用检查提示
        int refs = 0;
        for (const auto& a : ctx.project().associations) {
            if (auto* b = std::get_if<DataBinding>(&a); b && b->tag == t.name) ++refs;
            if (auto* r = std::get_if<AlarmRule>(&a); r && r->tag == t.name) ++refs;
            if (auto* l = std::get_if<LinkageRule>(&a); l && l->param == t.name) ++refs;
        }
        ImGui::Text("选中: %s (%d 处关联引用)", t.name.c_str(), refs);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1));
        if (ImGui::Button(refs > 0 ? "删除(有引用!)" : "删除")) {
            ctx.doc.commit("删除标签 " + t.name);
            db.remove(t.name);
            selectedRow = -1;
        }
        ImGui::PopStyleColor();
        // 重命名（同步关联引用）
        static std::string renameBuf;
        renameBuf = t.name;
        if (ImGui::InputText("重命名", &renameBuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::string oldName = t.name;
            if (oldName != renameBuf && !renameBuf.empty() && !db.find(renameBuf)) {
                ctx.doc.commit("标签重命名");
                Tag renamed = t;
                renamed.name = renameBuf;
                std::string err;
                db.add(std::move(renamed), err);
                db.remove(oldName);
                for (auto& a : ctx.project().associations) {
                    if (auto* b = std::get_if<DataBinding>(&a); b && b->tag == oldName) b->tag = renameBuf;
                    if (auto* r = std::get_if<AlarmRule>(&a); r && r->tag == oldName) r->tag = renameBuf;
                    if (auto* l = std::get_if<LinkageRule>(&a); l && l->param == oldName && l->action == LinkageAction::SetTagValue) l->param = renameBuf;
                }
            }
        }
    }
    ImGui::End();
}

} // namespace pv::planner::panels
