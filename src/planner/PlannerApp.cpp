#include "planner/PlannerApp.h"

#include <chrono>

#include "base/appshell/FileDialog.h"
#include "base/model/ComponentRegistry.h"
#include "base/render/ComponentRenderer.h"
#include "base/serialize/ProjectJson.h"
#include "imgui.h"
#include "imgui_internal.h"  // DockBuilder（布局初始化; ImGui 官方 demo 同款用法）
#include "planner/panels/Associations.h"
#include "planner/panels/Canvas.h"
#include "planner/panels/Inspector.h"
#include "planner/panels/Palette.h"
#include "planner/panels/Pages.h"
#include "planner/panels/Tags.h"
#include "planner/panels/Validation.h"

namespace softg::planner {

static const std::vector<dialog::Filter> kProjectFilters = {
    {"SoftG 工程 (*.json)", "*.json"}, {"所有文件 (*.*)", "*.*"}};

// UTF-8 消息弹窗（MessageBoxA 不认 UTF-8 中文）
static int msgBox(const std::string& utf8Text, const std::string& utf8Title, UINT type) {
    auto wide = [](const std::string& s) {
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(len > 0 ? len : 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), (int)w.size());
        return w;
    };
    return MessageBoxW(nullptr, wide(utf8Text).c_str(), wide(utf8Title).c_str(), type);
}

PlannerApp::PlannerApp(AppShell& shell) : textures_(shell.device()) {
    shell_ = &shell;
    ctx_.textures = &textures_;
    // 确保初始页面 id
    ctx_.currentPagePtr();
}

void PlannerApp::firstRunLayout(ImGuiID dockspaceId) {
    if (layoutBuilt_)
        return;
    // DockSpaceOverViewport 会预先创建空节点，GetNode!=nullptr 不能作为"已有布局"依据；
    // 只有节点已挂窗口或存在分割（ini 恢复出的布局）时才跳过首次布局
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
    bool hasLayout =
        node && (!node->Windows.empty() || node->ChildNodes[0] != nullptr ||
                 node->ChildNodes[1] != nullptr);
    if (hasLayout) {
        layoutBuilt_ = true;
        return;
    }
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID left, right, main;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.18f, &left, &main);
    ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.32f, &right, &main);

    ImGui::DockBuilderDockWindow("组件面板", left);
    ImGui::DockBuilderDockWindow("页面", left);
    ImGui::DockBuilderDockWindow("画布", main);
    ImGui::DockBuilderDockWindow("属性", right);
    // 右侧标签页组
    ImGuiID rightTabs;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, &rightTabs, &right);
    ImGui::DockBuilderDockWindow("关联关系", right);
    ImGui::DockBuilderDockWindow("标签库", right);
    ImGui::DockBuilderDockWindow("校验", rightTabs);
    ImGui::DockBuilderFinish(dockspaceId);
    layoutBuilt_ = true;
}

bool PlannerApp::frame() {
    if (!viewInited_) { // ImGui 就绪后按系统 DPI 设置画布默认缩放
        viewInited_ = true;
        ctx_.view.zoom = shell_->dpiScale();
    }
    ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    firstRunLayout(dockspaceId);

    mainMenu();
    toolbar();

    panels::drawPalette(ctx_);
    panels::drawPages(ctx_);
    panels::drawCanvas(ctx_);
    panels::drawInspector(ctx_);
    panels::drawAssociations(ctx_);
    panels::drawTags(ctx_);
    panels::drawValidation(ctx_);
    return !exit_;
}

bool PlannerApp::confirmDiscard() {
    if (!ctx_.doc.dirty()) return true;
    // 原生阻塞确认（ImGui 弹窗需跨帧状态，这里够用）
    return msgBox("当前工程有未保存的修改，是否放弃？", "确认", MB_OKCANCEL | MB_ICONWARNING) == IDOK;
}

void PlannerApp::doNew() {
    if (!confirmDiscard()) return;
    Project p;
    ctx_.doc.reset(std::move(p));
    ctx_.doc.setPath("");
    ctx_.currentPage = "page-1";
    ctx_.selection.clear();
}

bool PlannerApp::doOpen() {
    if (!confirmDiscard()) return false;
    std::string path;
    if (!dialog::openFile("打开工程", kProjectFilters, path)) return false;
    Project loaded;
    std::string err;
    if (!projio::load(path, loaded, err)) {
        msgBox(err, "打开失败", MB_OK | MB_ICONERROR);
        return false;
    }
    ctx_.doc.reset(std::move(loaded));
    ctx_.doc.setPath(path);
    ctx_.currentPage = ctx_.project().pages.empty() ? PageId{} : ctx_.project().pages.front().id;
    ctx_.selection.clear();
    // 贴图基准目录 = 工程所在目录
    std::string dir = path;
    size_t slash = dir.find_last_of("/\\");
    textures_.setBaseDir(slash == std::string::npos ? "." : dir.substr(0, slash));
    return true;
}

bool PlannerApp::doSaveAs() {
    std::string path;
    std::string def = ctx_.project().name + ".json";
    if (!dialog::saveFile("工程另存为", kProjectFilters, def, path)) return false;
    if (path.find(".json") == std::string::npos) path += ".json";
    std::string err;
    if (!projio::save(path, ctx_.project(), err)) {
        msgBox(err, "保存失败", MB_OK | MB_ICONERROR);
        return false;
    }
    ctx_.doc.setPath(path);
    ctx_.doc.clearDirty();
    // 贴图基准目录 = 工程所在目录
    std::string dir = path;
    size_t slash = dir.find_last_of("/\\");
    textures_.setBaseDir(slash == std::string::npos ? "." : dir.substr(0, slash));
    return true;
}

bool PlannerApp::doSave() {
    if (ctx_.doc.path().empty()) return doSaveAs();
    std::string err;
    if (!projio::save(ctx_.doc.path(), ctx_.project(), err)) {
        msgBox(err, "保存失败", MB_OK | MB_ICONERROR);
        return false;
    }
    ctx_.doc.clearDirty();
    return true;
}

void PlannerApp::mainMenu() {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("文件")) {
        if (ImGui::MenuItem("新建", "Ctrl+N")) doNew();
        if (ImGui::MenuItem("打开...", "Ctrl+O")) doOpen();
        if (ImGui::MenuItem("保存", "Ctrl+S", false, !ctx_.doc.path().empty())) doSave();
        if (ImGui::MenuItem("另存为...", nullptr, false, true)) doSaveAs();
        ImGui::Separator();
        if (ImGui::MenuItem("退出")) exit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("编辑")) {
        if (ImGui::MenuItem("撤销", "Ctrl+Z", false, ctx_.doc.canUndo())) ctx_.doc.undo();
        if (ImGui::MenuItem("重做", "Ctrl+Y", false, ctx_.doc.canRedo())) ctx_.doc.redo();
        ImGui::Separator();
        if (ImGui::MenuItem("复制", "Ctrl+C", false, !ctx_.selection.empty())) ctx_.copySelection();
        if (ImGui::MenuItem("粘贴", "Ctrl+V", false, !ctx_.clipboard.empty()))
            ctx_.pasteClipboard();
        if (ImGui::MenuItem("再制", "Ctrl+D", false, !ctx_.selection.empty()))
            ctx_.duplicateSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("删除选中", "Del", false, !ctx_.selection.empty()))
            ctx_.deleteSelection();
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();

    // 全局快捷键（面板未聚焦时）
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_N)) doNew();
        if (ImGui::IsKeyPressed(ImGuiKey_O)) doOpen();
        if (ImGui::IsKeyPressed(ImGuiKey_S)) doSave();
    }
}

void PlannerApp::toolbar() {
    // 高度需含 WindowPadding 上下留白，否则按钮下半截被侧边栏窗口裁剪
    float barH = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
    if (!ImGui::BeginViewportSideBar("##toolbar", ImGui::GetMainViewport(), ImGuiDir_Up,
                                     barH, ImGuiWindowFlags_NoDecoration)) {
        ImGui::End();  // 不可见分支
        return;
    }
    Page* page = ctx_.currentPagePtr();
    if (ImGui::Button("新建")) doNew();
    ImGui::SameLine();
    if (ImGui::Button("打开")) doOpen();
    ImGui::SameLine();
    if (ImGui::Button("保存")) doSave();
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    if (ImGui::Button(ctx_.doc.canUndo() ? "撤销" : "撤销~")) ctx_.doc.undo();
    ImGui::SameLine();
    if (ImGui::Button(ctx_.doc.canRedo() ? "重做" : "重做~")) ctx_.doc.redo();
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    // 网格/吸附
    const char* grids[] = {"关闭", "4", "8", "16"};
    float values[] = {0.0f, 4.0f, 8.0f, 16.0f};
    int cur = ctx_.view.grid < 1.0f ? 0 : ctx_.view.grid == 4.0f ? 1 : ctx_.view.grid == 8.0f ? 2 : 3;
    ImGui::SetNextItemWidth(70 * shell_->dpiScale());
    if (ImGui::BeginCombo("网格", grids[cur])) {
        for (int i = 0; i < 4; ++i)
            if (ImGui::Selectable(grids[i], i == cur))
                ctx_.view.grid = values[i];
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("吸附", &ctx_.view.snapEnabled);
    ImGui::SameLine();
    ImGui::Checkbox("关联弧线", &ctx_.showArcs);
    ImGui::SameLine();
    char zoom[32];
    snprintf(zoom, sizeof(zoom), "%.0f%%", ctx_.view.zoom * 100.0f);
    ImGui::TextUnformatted(zoom);
    ImGui::SameLine();
    ImGui::TextDisabled("  %s%s", ctx_.doc.dirty() ? "* " : "",
                        ctx_.doc.path().empty() ? "未保存" : ctx_.doc.path().c_str());
    if (page) {
        ImGui::SameLine();
        ImGui::TextDisabled("  组件: %d  选中: %d", (int)page->components.size(),
                            (int)ctx_.selection.size());
    }
    ImGui::End();
}

} // namespace softg::planner
