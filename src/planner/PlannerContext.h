// PlannerContext — 设计器各面板共享的状态与编辑操作（面板为自由函数，避免互相依赖）。
#pragma once

#include <set>
#include <string>
#include <vector>

#include "base/model/AlignOps.h"
#include "base/model/Project.h"
#include "base/render/RenderContext.h"
#include "base/render/TextureCache.h"
#include "planner/Document.h"

namespace pv::planner {

// 画布视图状态（屏幕 = 页面 * zoom + offset）
struct CanvasView {
    ImVec2 offset = ImVec2(40, 40);
    float zoom = 1.0f;
    float grid = 8.0f;       // 0 = 关闭吸附；同时控制网格线显示
    bool snapEnabled = true;

    ImVec2 toScreen(ImVec2 page) const { return ImVec2(page.x * zoom + offset.x, page.y * zoom + offset.y); }
    ImVec2 toPage(ImVec2 screen) const {
        return ImVec2((screen.x - offset.x) / zoom, (screen.y - offset.y) / zoom);
    }
    float snap(float v) const { return (snapEnabled && grid > 0.5f) ? std::round(v / grid) * grid : v; }
};

struct PlannerContext {
    Document doc;
    PageId currentPage;                       // 空时取第一个页面
    std::set<ComponentId> selection;          // 可跨页（画布只编辑当前页）
    ComponentId anchor;                       // 锚点=最后点击的组件（对齐/尺寸基准）
    CanvasView view;
    bool showArcs = false;                    // 关联弧线叠加
    bool focusRequested = false;              // 关联面板"跳转"请求
    ComponentId focusComponent;
    TextureCache* textures = nullptr;         // 由 PlannerApp 持有并注入

    // 剪贴板（保存于面板帧之间）
    std::vector<Component> clipboard;

    // ---- 便捷访问 ----
    Project& project() { return doc.project(); }
    Page* currentPagePtr() {
        Project& p = project();
        if (!p.findPage(currentPage))
            currentPage = p.pages.empty() ? PageId{} : p.pages.front().id;
        return p.findPage(currentPage);
    }
    bool isSelected(const ComponentId& id) const { return selection.count(id) > 0; }

    // ---- 编辑操作（内部处理 commit / 选择 / z 不变量） ----

    // 在当前页 pagePos 处创建组件并选中；返回 nullptr 表示类型未注册
    Component* addComponent(const std::string& typeId, ImVec2 pagePos);
    void deleteSelection();
    // z 序调整：delta >0 前移
    void nudgeZOrder(int delta);
    // 选择集整体平移（页面坐标，已吸附）
    void moveSelection(ImVec2 delta);

    // ---- 对齐 / 尺寸 / 分布（当前页、未锁定、选中组件；操作前 commit） ----
    void ensureAnchor();                      // 维护锚点不变量（选区非空时锚点为选区成员）
    Component* selectionAnchor();             // 锚点组件（缺失时按页面顺序回退）
    void alignSelection(AlignMode mode);      // 以锚点为基准对齐
    void sizeSelection(SizeMode mode);        // 以锚点为基准统一尺寸
    void distributeSelection(bool horizontal);// 等距分布（≥3 件；基于跨度）
    // 当前页内命中最上层组件（z 降序）
    Component* pickAt(ImVec2 pagePos);
    // 校正选择集：剔除已不存在的组件（undo/加载后调用）
    void sanitizeSelection();

    // ---- 剪贴板（仅当前页；粘贴重新分配 id，偏移 +16px） ----
    void copySelection();
    void pasteClipboard();
    void duplicateSelection();  // = copy + paste
};

} // namespace pv::planner
