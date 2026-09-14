// LogicPlanner — 逻辑规划软件（设计器）入口
#include "base/appshell/AppShell.h"
#include "imgui.h"
#include "planner/PlannerApp.h"

int main() {
    pv::AppShell shell;

    pv::AppConfig cfg;
    cfg.windowTitle = "逻辑规划软件 - LogicPlanner";
    cfg.windowSize = ImVec2(1680, 960);
    cfg.iniFilename = "LogicPlanner.ini";

    static pv::planner::PlannerApp app(shell);
    return shell.run(cfg, [](pv::AppShell&) { return app.frame(); });
}
