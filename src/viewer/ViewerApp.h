// ViewerApp — 页面展示软件（运行器）：
// 打开配置 → 共享渲染器展示页面 → 数据联动（绑定刷新/联动/告警）→ 导航/交互/写回。
#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <string>

#include "base/appshell/AppShell.h"
#include "base/data/frame/FrameDataSource.h"
#include "base/render/TextureCache.h"
#include "base/runtime/RuntimeEngine.h"
#include "viewer/PollWorker.h"

namespace softg::viewer {

class ViewerApp {
public:
    explicit ViewerApp(AppShell& shell);
    ~ViewerApp();
    ViewerApp(const ViewerApp&) = delete;
    ViewerApp& operator=(const ViewerApp&) = delete;

    bool frame();  // 每帧；返回 false 退出

    // 打开指定工程文件（命令行/欢迎界面入口）；失败返回 false
    bool openPath(const std::string& path);

private:
    void drawMainUi();
    void drawPage(ImDrawList* dl, const Page& page);
    void drawAlarmBanner();
    void drawStatusBar();
    void handleInteractions(const Page& page);
    void handleActions();
    void openProjectDialog();
    void startPolling();
    void stopPolling();
    void detailPopup(const Component& c);

    // 上次打开的工程（存 exe 同目录 PageViewer.recent）
    std::string loadRecentPath();
    void saveRecentPath(const std::string& path);

    // 页面视图变换（缩放/平移）；viewZoom_ <= 0 表示未初始化，首帧按系统 DPI 设置
    ImVec2 viewOffset_ = ImVec2(40, 40);
    float viewZoom_ = 0.0f;
    std::string dsLastFrameTimeCache_; // RenderContext.string_view 的宿主（每帧刷新）
    std::map<std::string, uint64_t> protoFrameCounts_; // 协议名 -> 帧计数（组内各协议独立）

    ImVec2 toScreen(ImVec2 page) const {
        return ImVec2(page.x * viewZoom_ + viewOffset_.x, page.y * viewZoom_ + viewOffset_.y);
    }
    ImVec2 toPage(ImVec2 screen) const {
        return ImVec2((screen.x - viewOffset_.x) / viewZoom_,
                      (screen.y - viewOffset_.y) / viewZoom_);
    }

    AppShell& shell_;
    TextureCache textures_;
    bool hasProject_ = false;
    Project project_;          // 运行副本（可写，不回存文件）
    RuntimeEngine engine_{project_};
    PageId currentPage_;
    PollWorker worker_;

    // 组件详情弹窗
    bool showDetail_ = false;
    ComponentId detailComp_;

    // 报文监视（帧数据源）
    bool showFrameMonitor_ = false;
    bool showTestGen_ = false;     // 协议测试数据窗口（独立窗口）
    int genFrameCount_ = 8;        // 协议测试数据生成帧数
    std::string testHex_;          // 最近生成的随机帧 HEX 文本（每帧一行）
    int selectedFrame_ = -1;   // -1 = 跟随最新一帧
    std::deque<FrameDataSource::FrameLogEntry> frameLog_;
    void drawFrameMonitor();
    void drawTestGen();

    // 交互状态
    std::map<ComponentId, std::chrono::steady_clock::time_point> pressedUntil_;  // 按钮按下反馈
    std::string dragSlider_;    // 正在拖动的滑块 id（释放时写回）
    bool exit_ = false;
};

} // namespace softg::viewer
