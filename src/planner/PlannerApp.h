// PlannerApp — 设计器主框架：DockSpace 布局、主菜单、工具栏、面板编排、文件操作。
#pragma once

#include "base/appshell/AppShell.h"
#include "planner/PlannerContext.h"

namespace softg::planner {

class PlannerApp {
public:
    explicit PlannerApp(AppShell& shell);
    // 每帧调用；返回 false 请求退出
    bool frame();

private:
    void mainMenu();
    void toolbar();
    void firstRunLayout(ImGuiID dockspaceId);

    // 文件操作
    bool doSave();       // 无路径时转另存为
    bool doSaveAs();
    bool doOpen();
    void doNew();
    bool confirmDiscard();  // 有未保存修改时确认; false = 取消后续操作

    PlannerContext ctx_;
    TextureCache textures_;
    AppShell* shell_ = nullptr;       // 用于读取 DPI 缩放
    bool layoutBuilt_ = false;
    bool viewInited_ = false;         // 首帧按 DPI 设置画布默认缩放
    bool exit_ = false;
};

} // namespace softg::planner
