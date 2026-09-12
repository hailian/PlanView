// LogicPlanner — 逻辑规划软件（设计器）入口
#include "base/appshell/AppShell.h"
#include "imgui.h"
#include "planner/PlannerApp.h"

int main() {
    softg::AppShell shell;

    softg::AppConfig cfg;
    cfg.windowTitle = "逻辑规划软件 - LogicPlanner";
    cfg.windowSize = ImVec2(1680, 960);
    cfg.iniFilename = "LogicPlanner.ini";

    static softg::planner::PlannerApp app(shell);
    return shell.run(cfg, [](softg::AppShell&) { return app.frame(); });
}
