#include "planner/panels/Pages.h"

#include "imgui.h"
#include "planner/PlannerContext.h"

namespace softg::planner::panels {

void drawPages(PlannerContext& ctx) {
    if (!ImGui::Begin("页面")) {
        ImGui::End();
        return;
    }
    Project& p = ctx.project();

    if (ImGui::Button("新建页面")) {
        ctx.doc.commit("新建页面");
        Page pg;
        pg.id = p.allocId("page");
        pg.name = "页面 " + std::to_string(p.pages.size() + 1);
        p.pages.push_back(std::move(pg));
        ctx.currentPage = p.pages.back().id;
    }
    ImGui::Separator();

    for (size_t i = 0; i < p.pages.size(); ++i) {
        Page& pg = p.pages[i];
        ImGui::PushID(pg.id.c_str());
        bool selected = pg.id == ctx.currentPage;
        ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick;
        if (ImGui::Selectable(pg.name.c_str(), selected, flags)) {
            if (!selected) {
                ctx.currentPage = pg.id;
                ctx.selection.clear();
            }
            if (ImGui::IsMouseDoubleClicked(0)) {
                // 双击改名
                ImGui::OpenPopup("重命名");
            }
        }
        if (ImGui::BeginPopupContextItem("page_ctx")) {
            if (ImGui::MenuItem("重命名...")) ImGui::OpenPopup("重命名");
            if (ImGui::MenuItem("删除页面", nullptr, false, p.pages.size() > 1)) {
                ctx.doc.commit("删除页面 " + pg.name);
                p.pages.erase(p.pages.begin() + i);
                if (ctx.currentPage == pg.id) {
                    ctx.currentPage = p.pages.front().id;
                    ctx.selection.clear();
                }
                ImGui::EndPopup();
                ImGui::PopID();
                break;  // 迭代器失效，本帧结束遍历
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("重命名")) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s", pg.name.c_str());
            if (ImGui::InputText("名称", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                ctx.doc.commit("页面改名");
                pg.name = buf;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace softg::planner::panels
