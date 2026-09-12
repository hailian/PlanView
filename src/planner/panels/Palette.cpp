#include "planner/panels/Palette.h"

#include <map>

#include "base/model/ComponentRegistry.h"
#include "imgui.h"
#include "planner/PlannerContext.h"

namespace softg::planner::panels {

void drawPalette(PlannerContext& ctx) {
    (void)ctx;
    if (!ImGui::Begin("组件面板")) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted("拖到画布添加组件");
    ImGui::Separator();

    // 按分类分组
    std::map<std::string, std::vector<const ComponentTypeInfo*>> groups;
    for (const auto& info : ComponentRegistry::instance().all())
        groups[info.category].push_back(&info);

    for (auto& [category, infos] : groups) {
        if (ImGui::CollapsingHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginTable("palette", 2, ImGuiTableFlags_SizingFixedSame)) {
                for (const auto* info : infos) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(info->typeId.c_str());
                    ImVec2 size(86, 34);
                    // 按钮既是拖拽源，也可点击添加到画布左上
                    ImGui::Button((info->displayName + " " + info->typeId).c_str(), size);
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("palette.typeId", info->typeId.c_str(),
                                                  info->typeId.size() + 1);
                        ImGui::Text("添加: %s", info->displayName.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        // 双击：添加到当前页左上角
                        ctx.addComponent(info->typeId, ImVec2(40, 40));
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::End();
}

} // namespace softg::planner::panels
