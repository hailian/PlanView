// PageViewer — 页面展示软件（运行器）入口
// 用法: PageViewer.exe [工程文件.json]   （命令行指定则自动打开）
#include "base/appshell/AppShell.h"
#include "imgui.h"
#include "viewer/ViewerApp.h"

int main(int argc, char** argv) {
    pv::AppShell shell;

    pv::AppConfig cfg;
    cfg.windowTitle = "页面展示软件 - PageViewer";
    cfg.windowSize = ImVec2(1280, 860);
    cfg.iniFilename = "PageViewer.ini";

    static pv::viewer::ViewerApp app(shell);
    if (argc >= 2)
        app.openPath(argv[1]);  // 命令行直接打开（失败则落到欢迎界面）
    return shell.run(cfg, [](pv::AppShell&) { return app.frame(); });
}
