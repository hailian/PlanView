#include "viewer/ViewerApp.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "base/appshell/FileDialog.h"
#include "base/log/Log.h"
#include "base/model/ComponentRegistry.h"
#include "base/render/ComponentRenderer.h"
#include "base/serialize/JsonHelpers.h"
#include "base/serialize/ProjectJson.h"
#include "imgui.h"
#include "imgui_internal.h"  // BeginViewportSideBar / SeparatorEx（与 PlannerApp 同款用法）

namespace softg::viewer {

static const std::vector<dialog::Filter> kProjectFilters = {
    {"SoftG 工程 (*.json)", "*.json"}, {"所有文件 (*.*)", "*.*"}};

static std::string valueText(const Tag* t) {
    if (!t) return "?";
    char buf[64];
    if (auto* b = std::get_if<bool>(&t->currentValue))
        snprintf(buf, sizeof(buf), *b ? "true" : "false");
    else if (auto* i = std::get_if<int64_t>(&t->currentValue))
        snprintf(buf, sizeof(buf), "%lld", (long long)*i);
    else if (auto* d = std::get_if<double>(&t->currentValue))
        snprintf(buf, sizeof(buf), "%.4g", *d);
    else
        snprintf(buf, sizeof(buf), "%s", std::get<std::string>(t->currentValue).c_str());
    return buf;
}

static const char* qualityText(TagQuality q) {
    switch (q) {
    case TagQuality::Good: return "好";
    case TagQuality::Bad: return "坏";
    case TagQuality::CommLost: return "通讯丢失";
    }
    return "?";
}

ViewerApp::ViewerApp(AppShell& shell) : shell_(shell), textures_(shell.device()) {}

ViewerApp::~ViewerApp() { stopPolling(); }

// ---- 工程与连接 ----

void ViewerApp::openProjectDialog() {
    std::string path;
    if (!dialog::openFile("打开工程", kProjectFilters, path)) return;
    openPath(path);
}

bool ViewerApp::openPath(const std::string& path) {
    Project loaded;
    std::string err;
    if (!projio::load(path, loaded, err)) {
        SOFTG_LOG_ERROR("打开工程失败: %s", err.c_str());
        ImGui::Text("打开失败: %s", err.c_str());  // 本帧提示（欢迎界面显示）
        return false;
    }
    stopPolling();
    project_ = std::move(loaded);
    // 组件 bindField（直接绑定协议字段）→ 合成隐式标签 + 数据绑定，再交引擎
    synthesizeImplicitBindings(project_);
    engine_.rebind(project_);
    hasProject_ = true;
    currentPage_ = project_.pages.empty() ? PageId{} : project_.pages.front().id;
    viewZoom_ = 0.0f;  // 首帧按系统 DPI 重新初始化（命令行打开时 run 未启动，此处拿不到缩放）
    viewOffset_ = ImVec2(40, 40);

    std::string dir = path;
    size_t slash = dir.find_last_of("/\\");
    textures_.setBaseDir(slash == std::string::npos ? "." : dir.substr(0, slash));

    // 连接参数初始化为工程设置
    snprintf(hostBuf_, sizeof(hostBuf_), "%s", project_.settings.tcp.host.c_str());
    portBuf_ = project_.settings.tcp.port;
    pollMsBuf_ = project_.settings.tcp.pollMs;

    saveRecentPath(path);
    SOFTG_LOG_INFO("工程已加载: %s (%d 页 / %d 标签 / %d 关联)", path.c_str(),
                   (int)project_.pages.size(), (int)project_.tags.all().size(),
                   (int)project_.associations.size());
    startPolling();
    return true;
}

std::string ViewerApp::loadRecentPath() {
    std::ifstream f("PageViewer.recent");
    std::string path;
    if (f) std::getline(f, path);
    return path;
}

void ViewerApp::saveRecentPath(const std::string& path) {
    std::ofstream f("PageViewer.recent", std::ios::trunc);
    if (f) f << path;
}

void ViewerApp::startPolling() {
    if (!hasProject_ || project_.tags.all().empty()) return;
    // 数据源组件（传输）+ 关联协议配置组件（拆帧/字段）合成；无组件时回退旧工程设置
    project_.settings.frame = frameSettingsFromProject(project_);
    worker_.start(project_.settings, project_.tags.all());
}

void ViewerApp::stopPolling() { worker_.stop(); }

void ViewerApp::connectDialog() {
    if (!ImGui::Begin("连接设置", &showConnectDlg_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::InputText("主机", hostBuf_, sizeof(hostBuf_));
    ImGui::InputInt("端口", &portBuf_);
    ImGui::InputInt("轮询间隔(ms)", &pollMsBuf_);
    portBuf_ = std::clamp(portBuf_, 1, 65535);
    pollMsBuf_ = std::clamp(pollMsBuf_, 20, 10000);
    ImGui::Separator();
    float s = shell_.dpiScale();  // 按钮定宽适配高缩放
    if (ImGui::Button("应用并重连", ImVec2(110 * s, 0))) {
        project_.settings.tcp.host = hostBuf_;
        project_.settings.tcp.port = portBuf_;
        project_.settings.tcp.pollMs = pollMsBuf_;
        stopPolling();
        startPolling();
        showConnectDlg_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("断开", ImVec2(80 * s, 0))) {
        stopPolling();
        showConnectDlg_ = false;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(worker_.isConnected() ? "已连接" : "未连接");
    ImGui::End();
}

// ---- 每帧主流程 ----

bool ViewerApp::frame() {
    if (!hasProject_) {
        // 欢迎界面
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(460 * shell_.dpiScale(), 0));
        if (ImGui::Begin("页面展示软件", nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextUnformatted("PageViewer — 页面展示软件");
            ImGui::Spacing();
            if (ImGui::Button("打开工程...", ImVec2(-1, 36))) openProjectDialog();
            std::string recent = loadRecentPath();
            if (!recent.empty() && ImGui::Button(("打开上次: " + recent).c_str(), ImVec2(-1, 30)))
                openPath(recent);
            ImGui::TextDisabled("配置文件由 LogicPlanner 生成 (.json)");
            ImGui::End();
        }
        return !exit_;
    }

    auto now = std::chrono::steady_clock::now();
    engine_.tick(now);

    // 1) worker 读结果 -> 引擎；帧数据源报文 -> 监视缓存
    auto updates = worker_.drainResults();
    if (!updates.empty()) engine_.applyTagUpdates(updates);
    worker_.drainFrames(frameLog_);

    // 2) 页面渲染 + 交互
    drawMainUi();
    handleActions();
    return !exit_;
}

void ViewerApp::drawMainUi() {
    // 顶部：页面导航 + 连接 + 告警计数
    if (ImGui::BeginViewportSideBar("##topbar", ImGui::GetMainViewport(), ImGuiDir_Up,
                                    ImGui::GetFrameHeight() * 1.4f,
                                    ImGuiWindowFlags_NoDecoration)) {
        for (const auto& pg : project_.pages) {
            if (ImGui::Button(pg.name.c_str())) currentPage_ = pg.id;
            ImGui::SameLine();
        }
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        if (ImGui::Button("打开")) openProjectDialog();
        ImGui::SameLine();
        if (ImGui::Button("连接设置")) showConnectDlg_ = true;
        if (project_.settings.frame.enabled) {
            ImGui::SameLine();
            ImGui::Checkbox("报文监视", &showFrameMonitor_);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("缩放 %.0f%%  (滚轮/中键拖动)", viewZoom_ * 100.0f);
        ImGui::End();
    }

    if (showConnectDlg_) connectDialog();

    drawAlarmBanner();

    // 画布区
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos);
    ImGui::SetNextWindowSize(ImGui::GetMainViewport()->WorkSize);
    if (!ImGui::Begin("##page", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    const Page* page = project_.findPage(currentPage_);
    if (!page && !project_.pages.empty()) {
        currentPage_ = project_.pages.front().id;
        page = project_.findPage(currentPage_);
    }
    if (page) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        drawPage(dl, *page);
        handleInteractions(*page);
    } else {
        ImGui::TextUnformatted("工程没有页面");
    }
    ImGui::End();

    drawStatusBar();

    if (showFrameMonitor_) drawFrameMonitor();

    if (showDetail_) {
        if (const Component* c = project_.findComponent(detailComp_))
            detailPopup(*c);
        else
            showDetail_ = false;
    }
}

void ViewerApp::drawPage(ImDrawList* dl, const Page& page) {
    if (viewZoom_ <= 0.0f) { // 首帧：按系统 DPI 初始化视图，保证页面物理尺寸一致
        viewZoom_ = shell_.dpiScale();
        viewOffset_ = ImVec2(40 * viewZoom_, 40 * viewZoom_);
    }
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    ImVec2 canvasMax(canvasMin.x + avail.x, canvasMin.y + avail.y);
    ImGui::InvisibleButton("viewer_canvas", avail,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();

    // 缩放/平移
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        ImVec2 before = toPage(mouse);
        viewZoom_ = std::clamp(viewZoom_ * (ImGui::GetIO().MouseWheel > 0 ? 1.1f : 0.9f), 0.1f, 8.0f);
        ImVec2 after = toPage(mouse);
        viewOffset_.x += (after.x - before.x) * viewZoom_;
        viewOffset_.y += (after.y - before.y) * viewZoom_;
    }
    static bool panning = false;
    static ImVec2 panStart;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        panning = true;
        panStart = ImGui::GetIO().MousePos;
    }
    if (panning) {
        ImVec2 m = ImGui::GetIO().MousePos;
        viewOffset_.x += m.x - panStart.x;
        viewOffset_.y += m.y - panStart.y;
        panStart = m;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) panning = false;
    }

    dl->PushClipRect(canvasMin, canvasMax, true);

    // 页面背景（工作区背景更深，与主题底色一致）
    dl->AddRectFilled(canvasMin, canvasMax, IM_COL32(14, 17, 22, 255));
    ImVec2 pMin = toScreen(ImVec2(0, 0));
    ImVec2 pMax = toScreen(page.size);
    dl->AddRectFilled(pMin, pMax, page.background);

    // 组件渲染（Runtime 模式，共享渲染器）
    RenderContext ctx;
    ctx.mode = RenderContext::Mode::Runtime;
    ctx.now = std::chrono::steady_clock::now();
    ctx.properties = &engine_;
    ctx.chartSeries = &engine_;
    ctx.alarms = &engine_;
    ctx.textures = &textures_;
    for (const auto& c : page.components)
        ComponentRenderer::drawComponent(dl, c, ctx, viewOffset_, viewZoom_);

    dl->PopClipRect();
}

// ---- 交互（只读语义 + 操作组件写回） ----

void ViewerApp::handleInteractions(const Page& page) {
    if (!ImGui::IsItemHovered()) return;
    ImVec2 mouse = ImGui::GetIO().MousePos;
    ImVec2 pagePos = toPage(mouse);
    auto now = std::chrono::steady_clock::now();

    // 命中最上层可见组件
    const Component* hit = nullptr;
    for (auto it = page.components.rbegin(); it != page.components.rend(); ++it) {
        if (it->visible && it->frame.contains(pagePos)) {
            hit = &*it;
            break;
        }
    }

    // 左键：操作组件
    if (hit && ImGui::IsMouseClicked(0)) {
        if (hit->typeId == "Button") {
            pressedUntil_[hit->id] = now + std::chrono::milliseconds(200);
            engine_.raiseComponentEvent(hit->id, LinkageEvent::Click);
        } else if (hit->typeId == "Switch") {
            bool cur = props::asBool(hit->propOr("isOn", false));
            if (const PropertyValue* rv = engine_.resolved(hit->id, "isOn"))
                cur = props::asBool(*rv);
            engine_.setInteractiveValue(hit->id, "isOn", TagValue(!cur));
        } else if (hit->typeId == "Slider") {
            dragSlider_ = hit->id;  // 按下开始拖动
        }
    }
    // 右键：详情弹窗
    if (hit && ImGui::IsMouseClicked(1)) {
        detailComp_ = hit->id;
        showDetail_ = true;
    }

    // 滑块拖动/释放
    if (!dragSlider_.empty()) {
        if (Component* s = project_.findComponent(dragSlider_)) {
            if (s->frame.contains(pagePos) && ImGui::IsMouseDown(0)) {
                double minV = props::asDouble(s->propOr("min", 0.0));
                double maxV = props::asDouble(s->propOr("max", 100.0));
                if (maxV <= minV) maxV = minV + 1;
                bool vertical =
                    props::asString(s->propOr("orientation", std::string("水平"))) == "垂直";
                double frac = vertical
                                  ? 1.0 - (pagePos.y - s->frame.y) / s->frame.h
                                  : (pagePos.x - s->frame.x) / s->frame.w;
                frac = std::clamp(frac, 0.0, 1.0);
                engine_.setInteractiveValue(s->id, "value",
                                            TagValue(minV + (maxV - minV) * frac));
            }
            if (!ImGui::IsMouseDown(0)) {
                // 释放：值变化联动（写回已在 setInteractiveValue 排队）
                dragSlider_.clear();
            }
        } else {
            dragSlider_.clear();
        }
    }

    // 按钮按下视觉反馈（_pressed 瞬态属性）
    for (auto& c : const_cast<Page&>(page).components) {
        auto it = pressedUntil_.find(c.id);
        bool pressed = it != pressedUntil_.end() && it->second > now;
        c.setProp("_pressed", pressed);
    }
}

// ---- 动作消费 ----

void ViewerApp::handleActions() {
    for (const PendingAction& a : engine_.drainActions()) {
        switch (a.kind) {
        case PendingAction::Kind::Navigate:
            if (project_.findPage(a.target)) currentPage_ = a.target;
            break;
        case PendingAction::Kind::WriteTag:
            worker_.enqueueWrite(a.target, a.value);
            break;
        }
    }
}

// ---- 告警条 / 状态栏 / 详情 ----

void ViewerApp::drawAlarmBanner() {
    const auto& fired = engine_.firedAlarmRules();
    if (fired.empty()) return;
    float h = ImGui::GetFrameHeight() * 1.2f;
    ImVec2 pos = ImGui::GetMainViewport()->WorkPos;
    ImGui::SetNextWindowPos(ImVec2(pos.x, pos.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetMainViewport()->WorkSize.x, h));
    if (ImGui::Begin("##alarms", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings)) {
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        ImGui::GetWindowDrawList()->AddRectFilled(
            wp, ImVec2(wp.x + ws.x, wp.y + ws.y), IM_COL32(255, 170, 0, 60));
        ImGui::Text("%d 条激活告警:", (int)fired.size());
        ImGui::SameLine();
        int shown = 0;
        for (const AssocId& id : fired) {
            const AlarmRule* r = engine_.findRule(id);
            if (!r) continue;
            if (++shown > 3) {
                ImGui::TextUnformatted("...");
                break;
            }
            ImGui::SameLine();
            const Tag* t = project_.tags.find(r->tag);
            ImGui::Text("[%s %s %.3g 当前 %.3g]", r->tag.c_str(),
                        jsonx::comparatorToString(r->cmp).c_str(), r->threshold,
                        t ? t->numeric() : 0.0);
            ImGui::SameLine();
            ImGui::PushID(id.c_str());
            if (ImGui::SmallButton("确认")) engine_.acknowledgeAlarm(id);
            ImGui::PopID();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("全部确认")) engine_.acknowledgeAllAlarms();
        ImGui::End();
    }
}

void ViewerApp::drawStatusBar() {
    if (!ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down,
                                     ImGui::GetFrameHeight(), ImGuiWindowFlags_NoDecoration)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%s", worker_.isConnected() ? "已连接" : "未连接");
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    ImGui::Text("标签 %d/%d", engine_.goodTagCount(), (int)project_.tags.all().size());
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    int alarms = engine_.activeAlarmCount();
    if (alarms > 0)
        ImGui::TextColored(ImVec4(1, 0.65f, 0.2f, 1), "告警 %d", alarms);
    else
        ImGui::Text("告警 0");
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    if (!worker_.isConnected() && worker_.running()) {
        std::string err = worker_.lastError();
        if (!err.empty()) ImGui::TextDisabled("%s", err.c_str());
    }
    ImGui::End();
}

// ---- 报文监视（帧数据源）：原始帧 HEX + 按规约解析 ----

void ViewerApp::drawFrameMonitor() {
    if (!ImGui::Begin("报文监视", &showFrameMonitor_))
        return ImGui::End();

    if (frameLog_.empty())
        ImGui::TextDisabled("(暂无报文)");

    // 报文列表（子区域固定高度 + 自动滚底，避免解析表被推走）
    ImGui::BeginChild("fmframes", ImVec2(0, ImGui::GetTextLineHeight() * 10),
                      ImGuiChildFlags_Borders);
    for (size_t i = 0; i < frameLog_.size(); ++i) {
        const auto& e = frameLog_[i];
        ImGui::PushID((int)i);
        std::string preview;
        for (size_t k = 0; k < e.data.size() && k < 12; ++k) {
            char b[4];
            std::snprintf(b, sizeof(b), "%02X", e.data[k]);
            preview += (k ? " " : "") + std::string(b);
        }
        if (e.data.size() > 12) preview += " ...";
        char line[512];
        std::snprintf(line, sizeof(line), "%s  [%d] %zu 字节  %s", e.timeText.c_str(),
                      (int)i, e.data.size(), preview.c_str());
        bool selected = (int)i == selectedFrame_;
        if (ImGui::Selectable(line, selected)) selectedFrame_ = (int)i;
        ImGui::PopID();
    }
    if (selectedFrame_ < 0 && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    // 选中帧：HEX DUMP + 规约解析
    if (!frameLog_.empty()) {
        if (selectedFrame_ < 0 || selectedFrame_ >= (int)frameLog_.size())
            selectedFrame_ = (int)frameLog_.size() - 1;
        const auto& e = frameLog_[selectedFrame_];

        ImGui::Separator();
        ImGui::BeginChild("fmdump", ImVec2(0, ImGui::GetTextLineHeight() * 5),
                          ImGuiChildFlags_Borders);
        for (size_t base = 0; base < e.data.size(); base += 16) {
            char text[256];
            int n = std::snprintf(text, sizeof(text), "%04zX  ", base);
            for (size_t k = 0; k < 16 && base + k < e.data.size(); ++k)
                n += std::snprintf(text + n, sizeof(text) - n, "%02X ", e.data[base + k]);
            ImGui::TextUnformatted(text);
        }
        ImGui::EndChild();

        // 按工程规约解析：与数据源同语义——TLV 帧只列匹配槽位的字段（偏移相对负载），
        // 帧头+Length 列全部字段（偏移相对整帧）
        std::vector<packet::PacketField> pf;
        int64_t tagId = -1;
        const uint8_t* payload = nullptr;
        int payloadLen = 0;
        bool structured = packet::decodeFrameOnce(project_.settings.frame.framing, e.data,
                                                  tagId, payload, payloadLen);
        if (structured) {
            for (const auto& tf : project_.settings.frame.fields) {
                if (project_.settings.frame.framing.mode == packet::FrameMode::Tlv &&
                    tf.tagId != tagId)
                    continue;
                packet::PacketField f;
                f.name = tf.name + " (@" + std::to_string(tf.tagId) + ")";
                f.offset = tf.offset;
                f.length = tf.bytes;
                f.type = tf.type;
                f.bigEndian = tf.bigEndian;
                pf.push_back(f);
            }
        }
        std::vector<uint8_t> payloadVec(payload, payload + payloadLen);
        auto parsed = packet::parsePacket(pf, structured ? payloadVec : e.data);
        if (ImGui::BeginTable("fmparsed", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("字段");
            ImGui::TableSetupColumn("偏移");
            ImGui::TableSetupColumn("原始HEX");
            ImGui::TableSetupColumn("值");
            ImGui::TableHeadersRow();
            for (const auto& p : parsed) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", p.offset);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.rawHex.c_str());
                ImGui::TableNextColumn();
                if (p.ok)
                    ImGui::TextUnformatted(p.engText.c_str());
                else
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", p.rawText.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void ViewerApp::detailPopup(const Component& c) {
    ImGui::SetNextWindowSize(ImVec2(380 * shell_.dpiScale(), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("组件详情", &showDetail_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    const auto* info = ComponentRegistry::instance().find(c.typeId);
    ImGui::Text("%s (%s)", c.name.c_str(), info ? info->displayName.c_str() : c.typeId.c_str());
    ImGui::TextDisabled("id: %s  页面: %s", c.id.c_str(),
                        project_.findPageOfComponent(c.id)
                            ? project_.findPageOfComponent(c.id)->name.c_str()
                            : "?");
    ImGui::Separator();

    ImGui::TextUnformatted("数据绑定:");
    bool any = false;
    for (const auto& a : project_.associations) {
        auto* b = std::get_if<DataBinding>(&a);
        if (!b || b->component != c.id) continue;
        any = true;
        const Tag* t = project_.tags.find(b->tag);
        ImGui::BulletText("%s <- %s: %s", b->property.c_str(), b->tag.c_str(),
                          t ? valueText(t).c_str() : "(标签不存在)");
        if (t) {
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", qualityText(t->quality));
        }
    }
    if (!any) ImGui::TextDisabled("  (无)");

    ImGui::Separator();
    ImGui::TextUnformatted("激活告警:");
    AlarmVisual av;
    if (engine_.alarmOf(c.id, av))
        ImGui::BulletText("severity=%d style=%d", (int)av.severity, (int)av.style);
    else
        ImGui::TextDisabled("  (无)");

    ImGui::Separator();
    if (ImGui::Button("关闭", ImVec2(80, 0))) showDetail_ = false;
    ImGui::End();
}

} // namespace softg::viewer
