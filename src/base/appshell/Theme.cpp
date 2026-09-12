#include "base/appshell/Theme.h"

#include "imgui.h"

namespace softg::theme {

namespace {

// ---- 调色板（蓝灰深色系 + 品牌蓝强调），组件绘制保持同一色感 ----
constexpr ImU32 kText          = IM_COL32(227, 231, 239, 255);  // #E3E7EF 主文字
constexpr ImU32 kTextDim       = IM_COL32(138, 147, 166, 255);  // 次级文字
constexpr ImU32 kDarkest       = IM_COL32(14, 17, 22, 255);     // 标题栏/窗口外底
constexpr ImU32 kWindowBg      = IM_COL32(21, 25, 34, 255);     // 窗口/面板底
constexpr ImU32 kFrameBg       = IM_COL32(30, 36, 48, 255);     // 输入框/按钮底
constexpr ImU32 kFrameHover    = IM_COL32(37, 44, 58, 255);
constexpr ImU32 kFrameActive   = IM_COL32(44, 53, 69, 255);
constexpr ImU32 kBorder        = IM_COL32(38, 45, 59, 255);     // 柔和描边
constexpr ImU32 kHeaderSel     = IM_COL32(48, 58, 76, 255);     // 列表选中/菜单高亮
constexpr ImU32 kAccent        = IM_COL32(72, 141, 255, 255);   // #488DFF 品牌蓝
constexpr ImU32 kAccentHover   = IM_COL32(96, 162, 255, 255);
constexpr ImU32 kAccentActive  = IM_COL32(52, 112, 224, 255);
constexpr ImU32 kSuccess       = IM_COL32(74, 200, 135, 255);   // 勾选/正常态

ImVec4 c(ImU32 rgba) { return ImGui::ColorConvertU32ToFloat4(rgba); }

} // namespace

void applyModern() {
    ImGuiStyle& s = ImGui::GetStyle();

    // ---- 几何：适度圆角 + 更松弛的间距 ----
    s.WindowRounding = 7.0f;
    s.ChildRounding = 6.0f;
    s.PopupRounding = 6.0f;
    s.FrameRounding = 5.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 5.0f;
    s.ScrollbarRounding = 12.0f;
    s.WindowBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(9, 5);
    s.CellPadding = ImVec2(7, 5);
    s.ItemSpacing = ImVec2(9, 7);
    s.ItemInnerSpacing = ImVec2(7, 5);
    s.IndentSpacing = 22.0f;
    s.ScrollbarSize = 13.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.DockingSeparatorSize = 2.0f;

    ImVec4* col = s.Colors;
    col[ImGuiCol_Text] = c(kText);
    col[ImGuiCol_TextDisabled] = c(kTextDim);
    col[ImGuiCol_WindowBg] = c(kWindowBg);
    col[ImGuiCol_ChildBg] = c(kWindowBg);
    col[ImGuiCol_PopupBg] = ImVec4(0.082f, 0.106f, 0.141f, 0.98f);
    col[ImGuiCol_Border] = c(kBorder);
    col[ImGuiCol_TitleBg] = c(kDarkest);
    col[ImGuiCol_TitleBgActive] = c(IM_COL32(24, 29, 38, 255));
    col[ImGuiCol_TitleBgCollapsed] = c(kDarkest);
    col[ImGuiCol_MenuBarBg] = c(IM_COL32(17, 21, 28, 255));

    col[ImGuiCol_FrameBg] = c(kFrameBg);
    col[ImGuiCol_FrameBgHovered] = c(kFrameHover);
    col[ImGuiCol_FrameBgActive] = c(kFrameActive);

    // 按钮与输入件同底色，悬停微亮、按下微蓝，保持扁平克制
    col[ImGuiCol_Button] = c(kFrameBg);
    col[ImGuiCol_ButtonHovered] = c(kFrameHover);
    col[ImGuiCol_ButtonActive] = c(IM_COL32(52, 78, 122, 255));

    col[ImGuiCol_Header] = c(kHeaderSel);
    col[ImGuiCol_HeaderHovered] = c(IM_COL32(56, 67, 88, 255));
    col[ImGuiCol_HeaderActive] = c(kFrameActive);

    col[ImGuiCol_CheckMark] = c(kSuccess);
    col[ImGuiCol_SliderGrab] = c(kAccent);
    col[ImGuiCol_SliderGrabActive] = c(kAccentHover);
    col[ImGuiCol_Separator] = c(IM_COL32(44, 52, 66, 255));
    col[ImGuiCol_SeparatorHovered] = c(kAccent);
    col[ImGuiCol_SeparatorActive] = c(kAccent);

    col[ImGuiCol_ScrollbarBg] = ImVec4(0.082f, 0.098f, 0.133f, 0.6f);
    col[ImGuiCol_ScrollbarGrab] = c(IM_COL32(60, 70, 90, 255));
    col[ImGuiCol_ScrollbarGrabHovered] = c(IM_COL32(80, 92, 115, 255));
    col[ImGuiCol_ScrollbarGrabActive] = c(kAccent);

    col[ImGuiCol_TextSelectedBg] = ImVec4(0.282f, 0.553f, 1.0f, 0.35f);
    col[ImGuiCol_NavCursor] = c(kAccent);
    col[ImGuiCol_DragDropTarget] = c(kAccent);
    col[ImGuiCol_ResizeGrip] = c(IM_COL32(60, 70, 90, 80));
    col[ImGuiCol_ResizeGripHovered] = c(kAccent);
    col[ImGuiCol_ResizeGripActive] = c(kAccentHover);

    // 停靠标签：选中页亮底 + 品牌蓝顶线
    col[ImGuiCol_Tab] = c(kFrameBg);
    col[ImGuiCol_TabHovered] = c(kHeaderSel);
    col[ImGuiCol_TabSelected] = c(IM_COL32(37, 44, 58, 255));
    col[ImGuiCol_TabSelectedOverline] = c(kAccent);
    col[ImGuiCol_TabDimmed] = c(IM_COL32(24, 29, 38, 255));
    col[ImGuiCol_TabDimmedSelected] = c(IM_COL32(32, 39, 51, 255));
    col[ImGuiCol_TabDimmedSelectedOverline] = c(IM_COL32(70, 84, 106, 255));
    col[ImGuiCol_DockingPreview] = ImVec4(0.282f, 0.553f, 1.0f, 0.55f);

    col[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
    col[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigWindowsMoveFromTitleBarOnly = true;
}

} // namespace softg::theme
