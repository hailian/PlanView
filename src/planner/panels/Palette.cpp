#include "planner/panels/Palette.h"

#include <algorithm>
#include <map>
#include <string>

#include "base/model/ComponentRegistry.h"
#include "imgui.h"
#include "planner/PlannerContext.h"

namespace pv::planner::panels {

void drawPalette(PlannerContext& ctx) {
    (void)ctx;
    if (!ImGui::Begin("组件面板")) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted("拖到画布添加组件");
    ImGui::Separator();

    // 按钮尺寸随文本与 DPI 缩放自适应：面板放不下两列时退化为单列，避免名称被裁剪
    float maxTextW = 0.0f;
    for (const auto& info : ComponentRegistry::instance().all()) {
        std::string label = info.displayName + " " + info.typeId;
        maxTextW = std::max(maxTextW, ImGui::CalcTextSize(label.c_str()).x);
    }
    float btnW = maxTextW + ImGui::GetStyle().FramePadding.x * 2.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = (btnW * 2.0f + ImGui::GetStyle().ItemSpacing.x <= avail) ? 2 : 1;

    // 按分类分组
    std::map<std::string, std::vector<const ComponentTypeInfo*>> groups;
    for (const auto& info : ComponentRegistry::instance().all())
        groups[info.category].push_back(&info);

    for (auto& [category, infos] : groups) {
        if (ImGui::CollapsingHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginTable("palette", cols, ImGuiTableFlags_SizingFixedSame)) {
                for (const auto* info : infos) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(info->typeId.c_str());
                    // 尺寸 (0,0)：宽度由表格列取最宽按钮统一，高度随字体缩放
                    ImGui::Button((info->displayName + " " + info->typeId).c_str(), ImVec2(0, 0));
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

} // namespace pv::planner::panels
