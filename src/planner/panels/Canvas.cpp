#include "planner/panels/Canvas.h"

#include <algorithm>
#include <chrono>
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
    Component* single = nullptr;
    if (ctx.selection.size() == 1)
        single = page->find(*ctx.selection.begin());
    for (const auto& c : page->components) {
        if (!ctx.selection.count(c.id)) continue;
        ImVec2 a = view.toScreen(c.frame.pos());
        ImVec2 b = view.toScreen(ImVec2(c.frame.x + c.frame.w, c.frame.y + c.frame.h));
        dl->AddRect(a, b, IM_COL32(80, 160, 255, 220), 0.0f, 0, 1.5f);
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
                else if (ctrl)
                    ctx.selection.insert(hit->id);
                else
                    ctx.selection = {hit->id};
                // 开始移动拖拽
                st.drag = DragKind::Move;
                st.startPage = view.toPage(mouse);
                st.startFrames.clear();
                for (const auto& c : page->components)
                    if (ctx.selection.count(c.id)) st.startFrames.push_back(c.frame);
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
            if (!st.committed) {
                ctx.doc.commit("移动组件");
                st.committed = true;
            }
            ImVec2 delta(pagePos.x - st.startPage.x, pagePos.y - st.startPage.y);
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
        st.drag != DragKind::None && st.drag != DragKind::Pan)
        st.drag = DragKind::None;
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
