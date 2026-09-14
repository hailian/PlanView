#include "planner/panels/Canvas.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "base/model/ComponentRegistry.h"
#include "base/render/ComponentRenderer.h"
#include "imgui.h"
#include "planner/PlannerContext.h"

namespace softg::planner::panels {

namespace {

// ---- 交互状态机 ----
enum class DragKind { None, Move, Resize, RubberBand, Pan };
enum class Handle { None, NW, N, NE, E, SE, S, SW, W };

struct CanvasState {
    DragKind drag = DragKind::None;
    Handle handle = Handle::None;
    ImVec2 startMouse;      // 拖拽起点（屏幕）
    ImVec2 startPage;       // 拖拽起点（页面）
    Rect startFrame;        // 拖拽开始时单选组件的 frame
    std::vector<Rect> startFrames;  // 多选拖动时的初始 frame
    bool committed = false; // 本次拖拽是否已 commit（undo 粒度）
    ComponentId pendingCollapse;  // 非空：按下多选成员未拖动，释放时折叠为单选
    bool moved = false;     // Move 拖动中是否产生过位移
};

CanvasState& state() {
    static CanvasState s;
    return s;
}

constexpr float kHandleSize = 7.0f;  // 手柄大小（屏幕像素）

// 命中 8 个手柄之一（单选时；屏幕坐标）
Handle hitHandle(const CanvasView& view, const Rect& frame, ImVec2 screen) {
    ImVec2 corners[8] = {
        view.toScreen(ImVec2(frame.x, frame.y)),                    // NW
        view.toScreen(ImVec2(frame.x + frame.w * 0.5f, frame.y)),   // N
        view.toScreen(ImVec2(frame.x + frame.w, frame.y)),          // NE
        view.toScreen(ImVec2(frame.x + frame.w, frame.y + frame.h * 0.5f)),  // E
        view.toScreen(ImVec2(frame.x + frame.w, frame.y + frame.h)),         // SE
        view.toScreen(ImVec2(frame.x + frame.w * 0.5f, frame.y + frame.h)),  // S
        view.toScreen(ImVec2(frame.x, frame.y + frame.h)),          // SW
        view.toScreen(ImVec2(frame.x, frame.y + frame.h * 0.5f)),   // W
    };
    float r = kHandleSize * 0.5f + 2.0f;
    for (int i = 0; i < 8; ++i)
        if (screen.x > corners[i].x - r && screen.x < corners[i].x + r &&
            screen.y > corners[i].y - r && screen.y < corners[i].y + r)
            return (Handle)(i + 1);
    return Handle::None;
}

void drawHandle(ImDrawList* dl, ImVec2 pos, ImU32 color = IM_COL32(80, 160, 255, 255)) {
    dl->AddRectFilled(ImVec2(pos.x - kHandleSize * 0.5f, pos.y - kHandleSize * 0.5f),
                      ImVec2(pos.x + kHandleSize * 0.5f, pos.y + kHandleSize * 0.5f), color, 2.0f);
    dl->AddRect(ImVec2(pos.x - kHandleSize * 0.5f, pos.y - kHandleSize * 0.5f),
                ImVec2(pos.x + kHandleSize * 0.5f, pos.y + kHandleSize * 0.5f),
                IM_COL32(255, 255, 255, 200), 2.0f);
}

// ---- 对齐工具条图标（无图标字体，用 ImDrawList 手绘示意图；中心 c、半幅 h） ----
using IconFn = void (*)(ImDrawList*, ImVec2, float, ImU32);

void iconAlignLeft(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    float L = c.x - h, T = c.y - h, B = c.y + h;
    dl->AddLine(ImVec2(L, T), ImVec2(L, B), col, 1.5f);
    dl->AddRectFilled(ImVec2(L, c.y - h * 0.62f), ImVec2(L + h * 0.85f, c.y - h * 0.12f), col);
    dl->AddRectFilled(ImVec2(L, c.y + h * 0.12f), ImVec2(L + h * 1.65f, c.y + h * 0.62f), col);
}
void iconAlignHCenter(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    float X = c.x, T = c.y - h, B = c.y + h;
    dl->AddLine(ImVec2(X, T), ImVec2(X, B), col, 1.5f);
    dl->AddRectFilled(ImVec2(X - h * 0.55f, c.y - h * 0.62f), ImVec2(X + h * 0.55f, c.y - h * 0.12f), col);
    dl->AddRectFilled(ImVec2(X - h * 0.95f, c.y + h * 0.12f), ImVec2(X + h * 0.95f, c.y + h * 0.62f), col);
}
void iconAlignRight(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    float R = c.x + h, T = c.y - h, B = c.y + h;
    dl->AddLine(ImVec2(R, T), ImVec2(R, B), col, 1.5f);
    dl->AddRectFilled(ImVec2(R - h * 0.85f, c.y - h * 0.62f), ImVec2(R, c.y - h * 0.12f), col);
    dl->AddRectFilled(ImVec2(R - h * 1.65f, c.y + h * 0.12f), ImVec2(R, c.y + h * 0.62f), col);
}
void iconAlignTop(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    float L = c.x - h, R = c.x + h, T = c.y - h;
    dl->AddLine(ImVec2(L, T), ImVec2(R, T), col, 1.5f);
    dl->AddRectFilled(ImVec2(c.x - h * 0.62f, T), ImVec2(c.x - h * 0.12f, T + h * 0.85f), col);
    dl->AddRectFilled(ImVec2(c.x + h * 0.12f, T), ImVec2(c.x + h * 0.62f, T + h * 1.65f), col);
}
void iconAlignVCenter(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    float L = c.x - h, R = c.x + h, Y = c.y;
    dl->AddLine(ImVec2(L, Y), ImVec2(R, Y), col, 1.5f);
    dl->AddRectFilled(ImVec2(c.x - h * 0.62f, Y - h * 0.55f), ImVec2(c.x - h * 0.12f, Y + h * 0.55f), col);
    dl->AddRectFilled(ImVec2(c.x + h * 0.12f, Y - h * 0.95f), ImVec2(c.x + h * 0.62f, Y + h * 0.95f), col);
}
void iconAlignBottom(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    float L = c.x - h, R = c.x + h, B = c.y + h;
    dl->AddLine(ImVec2(L, B), ImVec2(R, B), col, 1.5f);
    dl->AddRectFilled(ImVec2(c.x - h * 0.62f, B - h * 0.85f), ImVec2(c.x - h * 0.12f, B), col);
    dl->AddRectFilled(ImVec2(c.x + h * 0.12f, B - h * 1.65f), ImVec2(c.x + h * 0.62f, B), col);
}
void iconEqualWidth(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    float L = c.x - h * 0.95f, w = h * 1.7f;
    dl->AddRectFilled(ImVec2(L, c.y - h * 0.85f), ImVec2(L + w, c.y - h * 0.12f), col);
    dl->AddRectFilled(ImVec2(L, c.y + h * 0.12f), ImVec2(L + w, c.y + h * 0.85f), col);
}
void iconEqualHeight(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    float T = c.y - h * 0.95f, hh = h * 1.7f;
    dl->AddRectFilled(ImVec2(c.x - h * 0.85f, T), ImVec2(c.x - h * 0.12f, T + hh), col);
    dl->AddRectFilled(ImVec2(c.x + h * 0.12f, T), ImVec2(c.x + h * 0.85f, T + hh), col);
}
void iconSameSize(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    float s = h * 1.3f, gap = h * 0.4f, total = s * 2 + gap;
    float x0 = c.x - total * 0.5f, y0 = c.y - s * 0.5f;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + s, y0 + s), col);
    dl->AddRectFilled(ImVec2(x0 + s + gap, y0), ImVec2(x0 + s + gap + s, y0 + s), col);
}
void iconDistributeH(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    float T = c.y - h, B = c.y + h;
    dl->AddRectFilled(ImVec2(c.x - h, T), ImVec2(c.x - h * 0.62f, B), col);
    dl->AddRectFilled(ImVec2(c.x - h * 0.16f, T), ImVec2(c.x + h * 0.16f, B), col);
    dl->AddRectFilled(ImVec2(c.x + h * 0.62f, T), ImVec2(c.x + h, B), col);
}
void iconDistributeV(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    float L = c.x - h, R = c.x + h;
    dl->AddRectFilled(ImVec2(L, c.y - h), ImVec2(R, c.y - h * 0.62f), col);
    dl->AddRectFilled(ImVec2(L, c.y - h * 0.16f), ImVec2(R, c.y + h * 0.16f), col);
    dl->AddRectFilled(ImVec2(L, c.y + h * 0.62f), ImVec2(R, c.y + h), col);
}

// 方形图标按钮：内绘示意图标，tooltip 给出中文名；禁用时随样式 alpha 变暗
bool iconButton(const char* id, const char* tip, IconFn fn) {
    float bh = ImGui::GetFrameHeight();
    bool clicked = ImGui::Button(id, ImVec2(bh, bh));
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    fn(ImGui::GetWindowDrawList(), ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f), bh * 0.32f,
       ImGui::GetColorU32(IM_COL32(215, 222, 235, 255)));
    ImGui::SetItemTooltip("%s", tip);
    return clicked;
}

// 画布顶部对齐工具条：对齐/尺寸以锚点（最后点击组件）为基准；分布需 ≥3 件
void drawAlignToolbar(PlannerContext& ctx, int selCount) {
    bool canAlign = selCount >= 2;
    bool canDist = selCount >= 3;
    auto sep = [] {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
    };

    ImGui::BeginDisabled(!canAlign);
    if (iconButton("##al", "左对齐：左缘对齐到锚点", iconAlignLeft))
        ctx.alignSelection(AlignMode::Left);
    ImGui::SameLine();
    if (iconButton("##ahc", "水平居中：水平中心对齐到锚点", iconAlignHCenter))
        ctx.alignSelection(AlignMode::HCenter);
    ImGui::SameLine();
    if (iconButton("##ar", "右对齐：右缘对齐到锚点", iconAlignRight))
        ctx.alignSelection(AlignMode::Right);
    sep();
    if (iconButton("##at", "顶对齐：上缘对齐到锚点", iconAlignTop))
        ctx.alignSelection(AlignMode::Top);
    ImGui::SameLine();
    if (iconButton("##avc", "垂直居中：垂直中心对齐到锚点", iconAlignVCenter))
        ctx.alignSelection(AlignMode::VCenter);
    ImGui::SameLine();
    if (iconButton("##ab", "底对齐：下缘对齐到锚点", iconAlignBottom))
        ctx.alignSelection(AlignMode::Bottom);
    ImGui::EndDisabled();

    sep();
    ImGui::BeginDisabled(!canAlign);
    if (iconButton("##ew", "等宽：宽度设为锚点宽度", iconEqualWidth))
        ctx.sizeSelection(SizeMode::Width);
    ImGui::SameLine();
    if (iconButton("##eh", "等高：高度设为锚点高度", iconEqualHeight))
        ctx.sizeSelection(SizeMode::Height);
    ImGui::SameLine();
    if (iconButton("##ss", "大小相同：宽高都设为锚点", iconSameSize))
        ctx.sizeSelection(SizeMode::Both);
    ImGui::EndDisabled();

    sep();
    ImGui::BeginDisabled(!canDist);
    if (iconButton("##dh", "水平分布：水平等间距（保持首尾外缘）", iconDistributeH))
        ctx.distributeSelection(true);
    ImGui::SameLine();
    if (iconButton("##dv", "垂直分布：垂直等间距（保持首尾外缘）", iconDistributeV))
        ctx.distributeSelection(false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("选中 %d 个", selCount);
}

// 拖拽手柄调整 frame（页面坐标）
void applyResize(DragKind, Handle h, Rect& f, ImVec2 page, const CanvasView& view) {
    float left = f.x, top = f.y, right = f.x + f.w, bottom = f.y + f.h;
    if (h == Handle::NW || h == Handle::N || h == Handle::NE) top = page.y;
    if (h == Handle::SW || h == Handle::S || h == Handle::SE) bottom = page.y;
    if (h == Handle::NW || h == Handle::W || h == Handle::SW) left = page.x;
    if (h == Handle::NE || h == Handle::E || h == Handle::SE) right = page.x;
    // 吸附 + 最小尺寸
    left = view.snap(left); top = view.snap(top);
    right = view.snap(right); bottom = view.snap(bottom);
    f.x = std::min(left, right);
    f.y = std::min(top, bottom);
    f.w = std::max(8.0f, std::abs(right - left));
    f.h = std::max(8.0f, std::abs(bottom - top));
}

// 「组件 -> 协议配置」关联曲线：端点取朝向对方最近边的中点，控制臂沿主导轴，
// 端点画圆点（协议端略大做「指向」感）。返回曲线中点（供调用方做悬停提示）。
ImVec2 drawProtoLink(ImDrawList* dl, const CanvasView& view, const Component& from,
                     const Component& to, ImU32 col) {
    auto edgeMidpoint = [](const ScreenRect& r, ImVec2 toward, float gap) {
        ImVec2 ctr = r.center();
        float dx = toward.x - ctr.x, dy = toward.y - ctr.y;
        if (std::fabs(dx) >= std::fabs(dy))
            return dx >= 0.0f ? ImVec2(r.Max.x + gap, ctr.y) : ImVec2(r.Min.x - gap, ctr.y);
        return dy >= 0.0f ? ImVec2(ctr.x, r.Max.y + gap) : ImVec2(ctr.x, r.Min.y - gap);
    };
    ScreenRect rc1(view.toScreen(from.frame.pos()),
                   view.toScreen(ImVec2(from.frame.x + from.frame.w, from.frame.y + from.frame.h)));
    ScreenRect rc2(view.toScreen(to.frame.pos()),
                   view.toScreen(ImVec2(to.frame.x + to.frame.w, to.frame.y + to.frame.h)));
    ImVec2 p1 = edgeMidpoint(rc1, rc2.center(), 3.0f);
    ImVec2 p2 = edgeMidpoint(rc2, rc1.center(), 3.0f);
    bool horiz =
        std::fabs(rc2.center().x - rc1.center().x) >= std::fabs(rc2.center().y - rc1.center().y);
    ImVec2 axis(horiz ? (rc2.center().x >= rc1.center().x ? 1.0f : -1.0f) : 0.0f,
                horiz ? 0.0f : (rc2.center().y >= rc1.center().y ? 1.0f : -1.0f));
    ImVec2 mid1(p1.x + axis.x * 70.0f, p1.y + axis.y * 70.0f);
    ImVec2 mid2(p2.x - axis.x * 70.0f, p2.y - axis.y * 70.0f);
    dl->AddBezierCubic(p1, mid1, mid2, p2, col, 2.0f, 24);
    dl->AddCircleFilled(p1, 4.0f, col, 10);
    dl->AddCircleFilled(p2, 5.0f, col, 10);
    return ImVec2((p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f);
}

} // namespace

void drawCanvas(PlannerContext& ctx) {
    if (!ImGui::Begin("画布", nullptr,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    Page* page = ctx.currentPagePtr();
    if (!page) {
        ImGui::TextUnformatted("没有页面");
        ImGui::End();
        return;
    }

    CanvasView& view = ctx.view;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mouse = ImGui::GetIO().MousePos;

    // 顶部对齐工具条（对齐/尺寸/分布）
    int selOnPage = 0;
    for (const auto& c : page->components)
        if (ctx.selection.count(c.id)) ++selOnPage;
    drawAlignToolbar(ctx, selOnPage);

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("canvas", avail,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    ImVec2 canvasMin = ImGui::GetItemRectMin(), canvasMax = ImGui::GetItemRectMax();
    dl->PushClipRect(canvasMin, canvasMax, true);

    // ---- 缩放（以鼠标为锚）/ 平移 ----
    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            ImVec2 before = view.toPage(mouse);
            view.zoom = std::clamp(view.zoom * (wheel > 0 ? 1.1f : 0.9f), 0.1f, 8.0f);
            ImVec2 after = view.toPage(mouse);
            view.offset.x += (after.x - before.x) * view.zoom;
            view.offset.y += (after.y - before.y) * view.zoom;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Keypad0) ||
            (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0))) {
            view.zoom = 1.0f;
            view.offset = ImVec2(40, 40);
        }
    }

    // 页面矩形（画布坐标 = 屏幕，直接用 view 变换）
    ImVec2 pageMin = view.toScreen(ImVec2(0, 0));
    ImVec2 pageMax = view.toScreen(page->size);
    dl->AddRectFilled(pageMin, pageMax, page->background);
    dl->AddRect(pageMin, pageMax, IM_COL32(255, 255, 255, 90), 0.0f, 0, 1.5f);

    // ---- 网格 ----
    if (view.grid >= 1.0f) {
        float step = view.grid * view.zoom;
        if (step >= 6.0f) {  // 太密不画
            for (float x = pageMin.x + step; x < pageMax.x; x += step)
                dl->AddLine(ImVec2(x, pageMin.y), ImVec2(x, pageMax.y),
                            IM_COL32(255, 255, 255, 12));
            for (float y = pageMin.y + step; y < pageMax.y; y += step)
                dl->AddLine(ImVec2(pageMin.x, y), ImVec2(pageMax.x, y),
                            IM_COL32(255, 255, 255, 12));
        }
    }

    // ---- 组件渲染（z 升序，共享渲染器） ----
    RenderContext rc;
    rc.mode = RenderContext::Mode::Design;
    rc.now = std::chrono::steady_clock::now();
    rc.textures = ctx.textures;
    for (const auto& c : page->components)
        ComponentRenderer::drawComponent(dl, c, rc, view.offset, view.zoom);

    // ---- 关联曲线：数据源 / 绑定协议字段的组件 -> 协议配置 ----
    // 核心链路可视化：不受「关联弧线」开关控制，建立关联即显示。
    // 按协议名匹配同页协议配置组件；跨页不画（卡片上仍可见协议名）。
    // 蓝色=数据源使用协议；青色=显示组件直接绑定协议字段（bindField）。
    {
        auto protoOnPage = [&](const std::string& name) -> const Component* {
            if (name.empty()) return nullptr;
            for (const auto& c2 : page->components)
                if (c2.typeId == "ProtocolConfig" && c2.name == name) return &c2;
            return nullptr;
        };
        auto trim = [](const std::string& s) {
            size_t b = s.find_first_not_of(" \t");
            size_t e = s.find_last_not_of(" \t");
            return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
        };
        auto showTip = [&](ImVec2 mid, const std::string& tip) {
            ImVec2 hover = ImGui::GetIO().MousePos;
            float dx = hover.x - mid.x, dy = hover.y - mid.y;
            if (dx * dx + dy * dy < 24.0f * 24.0f && ImGui::IsWindowHovered())
                ImGui::SetTooltip("%s", tip.c_str());
        };

        auto groupOnPage = [&](const std::string& name) -> const Component* {
            if (name.empty()) return nullptr;
            for (const auto& c2 : page->components)
                if (c2.typeId == "ProtocolGroup" && c2.name == name) return &c2;
            return nullptr;
        };

        for (const auto& c : page->components) {
            if (c.typeId == "ProtocolGroup") { // 协议组 -> 各成员协议（绿淡）
                int64_t n = props::asInt(c.propOr("protoCount", int64_t(0)));
                for (int64_t i = 0; i < n; ++i) {
                    std::string member = props::asString(
                        c.propOr("p" + std::to_string(i) + ".name", std::string()));
                    const Component* pc = protoOnPage(member);
                    if (!pc) continue;
                    ImVec2 mid = drawProtoLink(dl, view, c, *pc, IM_COL32(88, 200, 150, 170));
                    showTip(mid, "协议组 " + c.name + " 包含: " + pc->name);
                }
                continue;
            }
            if (c.typeId == "DataSource") {  // 数据源 -> 关联协议（蓝）/ 协议组（绿）
                std::string pn = props::asString(c.propOr("protocol", std::string()));
                const Component* pc = protoOnPage(pn);
                if (pc) { // 直连协议（蓝）；仅绑组时无此线，只画组线
                    ImVec2 mid = drawProtoLink(dl, view, c, *pc, IM_COL32(96, 165, 250, 220));
                    showTip(mid, "数据源 " + c.name + " 使用协议: " + pc->name);
                }
                if (const Component* grp =
                        groupOnPage(props::asString(c.propOr("group", std::string())))) {
                    ImVec2 m2 = drawProtoLink(dl, view, c, *grp, IM_COL32(88, 200, 150, 220));
                    showTip(m2, "数据源 " + c.name + " 使用协议组: " + grp->name);
                }
                // 数据源 -> 关联的数据目的（紫）：原始帧转发
                for (const auto& sk : page->components) {
                    if (sk.typeId != "DataSink") continue;
                    if (props::asString(sk.propOr("source", std::string())) != c.name) continue;
                    ImVec2 m2 = drawProtoLink(dl, view, c, sk, IM_COL32(167, 139, 250, 220));
                    showTip(m2, "数据源 " + c.name + " 转发原始帧 -> " + sk.name);
                }
                continue;
            }
            if (c.typeId == "ProtocolConfig" || c.typeId == "DataSink") continue;
            // 显示组件绑定协议字段（青）：bindField = "协议名/字段名"（容忍历史带空格）
            std::string bf = props::asString(c.propOr("bindField", std::string()));
            if (bf.empty()) continue;
            size_t slash = bf.find('/');
            const Component* pc = protoOnPage(trim(slash == std::string::npos ? bf : bf.substr(0, slash)));
            if (!pc) continue;
            ImVec2 mid = drawProtoLink(dl, view, c, *pc, IM_COL32(76, 201, 240, 210));
            std::string field =
                slash == std::string::npos ? std::string() : trim(bf.substr(slash + 1));
            showTip(mid, c.name + " 绑定协议字段: " + pc->name +
                            (field.empty() ? "" : "/" + field));
        }
    }

    // ---- 关联弧线叠加（联动: 源->目标贝塞尔曲线; 数据绑定: 组件角标圆点数） ----
    if (ctx.showArcs) {
        Project& proj = ctx.project();
        for (const auto& a : proj.associations) {
            if (auto* l = std::get_if<LinkageRule>(&a)) {
                const Component* src = proj.findComponent(l->source);
                const Component* dst = proj.findComponent(l->target);
                if (!src || !dst) continue;
                ImVec2 p1 = view.toScreen(src->frame.center());
                ImVec2 p2 = view.toScreen(dst->frame.center());
                bool alarmish = l->event == LinkageEvent::AlarmActive ||
                                l->event == LinkageEvent::AlarmCleared;
                ImU32 col = alarmish ? IM_COL32(255, 170, 40, 200)
                                     : l->action == LinkageAction::Navigate
                                           ? IM_COL32(150, 120, 255, 200)
                                           : IM_COL32(80, 200, 120, 200);
                ImVec2 mid1(p1.x + (p2.y > p1.y ? 90.0f : -90.0f), p1.y);
                ImVec2 mid2(p2.x + (p2.y > p1.y ? -90.0f : 90.0f), p2.y);
                dl->AddBezierCubic(p1, mid1, mid2, p2, col, 2.0f, 20);
                // 悬停提示
                ImVec2 hover = ImGui::GetIO().MousePos;
                float dx = hover.x - (p1.x + p2.x) * 0.5f;
                float dy = hover.y - (p1.y + p2.y) * 0.5f;
                if (dx * dx + dy * dy < 24.0f * 24.0f && ImGui::IsWindowHovered()) {
                    const char* eventName = l->event == LinkageEvent::Click            ? "点击"
                                            : l->event == LinkageEvent::ValueChanged  ? "值变化"
                                            : l->event == LinkageEvent::AlarmActive    ? "告警触发"
                                                                                       : "告警恢复";
                    ImGui::SetTooltip("%s: %s -> %s", eventName, src->name.c_str(),
                                      dst->name.c_str());
                }
            }
        }
        // 数据绑定角标：右上角小圆点（有绑定的组件）
        for (const auto& c : page->components) {
            int binds = 0;
            for (const auto& a : proj.associations)
                if (auto* b = std::get_if<DataBinding>(&a); b && b->component == c.id) ++binds;
            if (binds > 0) {
                ImVec2 pos = view.toScreen(ImVec2(c.frame.x + c.frame.w, c.frame.y));
                dl->AddCircleFilled(ImVec2(pos.x - 6, pos.y + 6), 4.5f,
                                    IM_COL32(76, 201, 240, 220), 10);
            }
        }
    }

    // ---- 选择叠加层 ----
    ctx.ensureAnchor();
    Component* single = nullptr;
    if (ctx.selection.size() == 1)
        single = page->find(*ctx.selection.begin());
    for (const auto& c : page->components) {
        if (!ctx.selection.count(c.id)) continue;
        ImVec2 a = view.toScreen(c.frame.pos());
        ImVec2 b = view.toScreen(ImVec2(c.frame.x + c.frame.w, c.frame.y + c.frame.h));
        dl->AddRect(a, b, IM_COL32(80, 160, 255, 220), 0.0f, 0, 1.5f);
        // 多选时标记锚点（对齐/尺寸基准）：左上角琥珀色圆点
        if (ctx.selection.size() >= 2 && c.id == ctx.anchor)
            dl->AddCircleFilled(ImVec2(a.x, a.y), 4.5f, IM_COL32(255, 196, 64, 255), 12);
        if (&c == single) {  // 单选画手柄（NW N NE E SE S SW W，与 hitHandle 一致）
            dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, a.y), IM_COL32(80, 160, 255, 90));
            const float fx[8] = {0, 0.5f, 1, 1, 1, 0.5f, 0, 0};
            const float fy[8] = {0, 0, 0, 0.5f, 1, 1, 1, 0.5f};
            for (int i = 0; i < 8; ++i)
                drawHandle(dl, view.toScreen(ImVec2(c.frame.x + c.frame.w * fx[i],
                                                    c.frame.y + c.frame.h * fy[i])));
        }
    }

    CanvasState& st = state();
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();

    // ---- 交互：按下 ----
    if (hovered && ImGui::IsMouseClicked(0)) {
        // 单选组件：先看手柄
        if (single) {
            Handle h = hitHandle(view, single->frame, mouse);
            if (h != Handle::None) {
                st.drag = DragKind::Resize;
                st.handle = h;
                st.startFrame = single->frame;
                st.startPage = view.toPage(mouse);
                st.committed = false;
            }
        }
        if (st.drag == DragKind::None) {
            Component* hit = ctx.pickAt(view.toPage(mouse));
            if (hit && !hit->locked) {
                bool ctrl = ImGui::GetIO().KeyCtrl;
                if (ctrl && ctx.selection.count(hit->id))
                    ctx.selection.erase(hit->id);
                else if (ctrl) {
                    ctx.selection.insert(hit->id);
                    ctx.anchor = hit->id;  // 最后点击者为锚点
                } else if (ctx.selection.count(hit->id) && ctx.selection.size() > 1) {
                    // 普通点击多选成员：保持选区整体拖动；若未拖动即释放，折叠为单选
                    ctx.anchor = hit->id;
                    st.pendingCollapse = hit->id;
                } else {
                    ctx.selection = {hit->id};
                    ctx.anchor = hit->id;
                }
                // 开始移动拖拽
                st.drag = DragKind::Move;
                st.startPage = view.toPage(mouse);
                st.startFrames.clear();
                for (const auto& c : page->components)
                    if (ctx.selection.count(c.id)) st.startFrames.push_back(c.frame);
                st.moved = false;
                st.committed = false;
            } else if (!hit) {
                if (!ImGui::GetIO().KeyCtrl) ctx.selection.clear();
                st.drag = DragKind::RubberBand;
                st.startPage = view.toPage(mouse);
            }
        }
    }
    // 中键平移
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        st.drag = DragKind::Pan;
        st.startMouse = mouse;
    }

    // ---- 交互：拖拽中 ----
    if (active || st.drag == DragKind::Pan) {
        ImVec2 pagePos = view.toPage(mouse);
        switch (st.drag) {
        case DragKind::Move: {
            ImVec2 delta(pagePos.x - st.startPage.x, pagePos.y - st.startPage.y);
            if (std::abs(delta.x) > 0.5f || std::abs(delta.y) > 0.5f) st.moved = true;
            if (st.moved && !st.committed) {
                ctx.doc.commit("移动组件");
                st.committed = true;
            }
            if (!st.moved) break; // 原地未动：不写 undo 也不改坐标
            size_t i = 0;
            for (auto& c : page->components) {
                if (!ctx.selection.count(c.id) || i >= st.startFrames.size()) continue;
                c.frame.x = view.snap(st.startFrames[i].x + delta.x);
                c.frame.y = view.snap(st.startFrames[i].y + delta.y);
                ++i;
            }
            break;
        }
        case DragKind::Resize: {
            if (single) {
                if (!st.committed) {
                    ctx.doc.commit("调整尺寸");
                    st.committed = true;
                }
                single->frame = st.startFrame;
                applyResize(st.drag, st.handle, single->frame, pagePos, view);
            }
            break;
        }
        case DragKind::RubberBand: {
            ImVec2 a = view.toScreen(st.startPage);
            ImVec2 b(mouse.x, mouse.y);
            dl->AddRectFilled(a, b, IM_COL32(80, 160, 255, 25));
            dl->AddRect(a, b, IM_COL32(80, 160, 255, 180));
            ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y));
            ImVec2 hi(std::max(a.x, b.x), std::max(a.y, b.y));
            ImVec2 loPage = view.toPage(lo), hiPage = view.toPage(hi);
            ctx.selection.clear();
            for (const auto& c : page->components) {
                if (c.frame.x >= loPage.x && c.frame.x + c.frame.w <= hiPage.x &&
                    c.frame.y >= loPage.y && c.frame.y + c.frame.h <= hiPage.y)
                    ctx.selection.insert(c.id);
            }
            ctx.ensureAnchor();  // 框选后保持锚点为选区成员
            break;
        }
        case DragKind::Pan: {
            view.offset.x += mouse.x - st.startMouse.x;
            view.offset.y += mouse.y - st.startMouse.y;
            st.startMouse = mouse;
            break;
        }
        case DragKind::None:
            break;
        }
    }

    // ---- 交互：释放 ----
    if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
        st.drag != DragKind::None && st.drag != DragKind::Pan) {
        // 多选成员上原地点击（未拖动）：折叠为单选（与常见设计软件一致）
        if (st.drag == DragKind::Move && !st.pendingCollapse.empty() && !st.moved) {
            ctx.selection = {st.pendingCollapse};
            ctx.anchor = st.pendingCollapse;
        }
        st.pendingCollapse = ComponentId{};
        st.drag = DragKind::None;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && st.drag == DragKind::Pan)
        st.drag = DragKind::None;

    // ---- 拖放创建（来自组件面板） ----
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("palette.typeId")) {
            std::string typeId((const char*)payload->Data);
            ImVec2 pos = view.toPage(mouse);
            const auto* info = ComponentRegistry::instance().find(typeId);
            if (info)  // 鼠标位置为左上角
                ctx.addComponent(typeId, pos);
        }
        ImGui::EndDragDropTarget();
    }

    // ---- 键盘（画布聚焦时） ----
    if (hovered && !ctx.selection.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            ctx.deleteSelection();
        }
        float step = ImGui::GetIO().KeyShift ? 10.0f : 1.0f;
        ImVec2 d(0, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) d.x = -step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) d.x = step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) d.y = -step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) d.y = step;
        if (d.x != 0 || d.y != 0) {
            ctx.doc.commit("微移组件");
            ctx.moveSelection(d);
        }
    }
    if (hovered) {
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) ctx.doc.undo();
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) ctx.doc.redo();
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            for (const auto& c : page->components) ctx.selection.insert(c.id);
            ctx.ensureAnchor();
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) ctx.copySelection();
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) ctx.pasteClipboard();
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) ctx.duplicateSelection();
    }
    if (st.drag == DragKind::None)
        ctx.sanitizeSelection();

    // 跳转请求（关联面板触发）：定位并选中组件
    if (ctx.focusRequested) {
        ctx.focusRequested = false;
        if (Component* target = ctx.project().findComponent(ctx.focusComponent)) {
            if (Page* p = ctx.project().findPageOfComponent(target->id))
                ctx.currentPage = p->id;
            ctx.selection = {target->id};
            view.zoom = std::max(view.zoom, 1.0f);
            // 让组件显示在画布中央
            ImVec2 ctr = target->frame.center();
            view.offset.x = (canvasMin.x + canvasMax.x) * 0.5f - ctr.x * view.zoom;
            view.offset.y = (canvasMin.y + canvasMax.y) * 0.5f - ctr.y * view.zoom;
        }
    }

    dl->PopClipRect();
    ImGui::End();
}

} // namespace softg::planner::panels
