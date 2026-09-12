#include "planner/PlannerContext.h"

#include "base/model/ComponentRegistry.h"

#include <algorithm>

namespace softg::planner {

Component* PlannerContext::addComponent(const std::string& typeId, ImVec2 pagePos) {
    auto* reg = ComponentRegistry::instance().find(typeId);
    if (!reg) return nullptr;
    Page* page = currentPagePtr();
    if (!page) return nullptr;

    doc.commit("添加 " + reg->displayName);

    // 同类型显示名自动编号
    std::string baseName = reg->displayName;
    int n = 1;
    for (const auto& c : page->components)
        if (c.name.rfind(baseName, 0) == 0) ++n;

    Component c = ComponentRegistry::createComponent(typeId, project().allocId("comp"));
    c.name = baseName + " " + std::to_string(n);
    c.frame.x = view.snap(pagePos.x);
    c.frame.y = view.snap(pagePos.y);
    c.z = page->components.empty() ? 0 : page->components.back().z + 1;
    page->components.push_back(std::move(c));
    selection = {page->components.back().id};
    return &page->components.back();
}

void PlannerContext::deleteSelection() {
    Page* page = currentPagePtr();
    if (!page || selection.empty()) return;
    doc.commit(selection.size() == 1 ? "删除组件" : ("删除 " + std::to_string(selection.size()) + " 个组件"));
    page->components.erase(
        std::remove_if(page->components.begin(), page->components.end(),
                       [this](const Component& c) { return selection.count(c.id) > 0; }),
        page->components.end());
    selection.clear();
}

void PlannerContext::nudgeZOrder(int delta) {
    Page* page = currentPagePtr();
    if (!page || selection.empty()) return;
    doc.commit("调整层级");
    for (auto& c : page->components)
        if (selection.count(c.id)) c.z += delta;
    page->sortComponents();
}

// 纯位移，不 commit——调用方决定 undo 粒度（拖拽开始 commit 一次，键盘微移每次 commit）
void PlannerContext::moveSelection(ImVec2 delta) {
    Page* page = currentPagePtr();
    if (!page || delta.x == 0.0f || delta.y == 0.0f) return;
    for (auto& c : page->components) {
        if (!selection.count(c.id)) continue;
        c.frame.x = view.snap(c.frame.x + delta.x);
        c.frame.y = view.snap(c.frame.y + delta.y);
    }
}

Component* PlannerContext::pickAt(ImVec2 pagePos) {
    Page* page = currentPagePtr();
    if (!page) return nullptr;
    for (auto it = page->components.rbegin(); it != page->components.rend(); ++it)  // z 降序
        if (it->frame.contains(pagePos))
            return &*it;
    return nullptr;
}

void PlannerContext::sanitizeSelection() {
    Project& p = project();
    for (auto it = selection.begin(); it != selection.end();) {
        if (p.findComponent(*it)) ++it;
        else it = selection.erase(it);
    }
}

void PlannerContext::copySelection() {
    Page* page = currentPagePtr();
    if (!page) return;
    clipboard.clear();
    for (const auto& c : page->components)
        if (selection.count(c.id)) clipboard.push_back(c);
}

void PlannerContext::pasteClipboard() {
    Page* page = currentPagePtr();
    if (!page || clipboard.empty()) return;
    doc.commit("粘贴组件");
    selection.clear();
    int maxZ = page->components.empty() ? 0 : page->components.back().z;
    for (auto c : clipboard) {  // 拷贝粘贴（剪贴板保留可重复粘贴）
        c.id = project().allocId("comp");
        c.frame.x += 16.0f;
        c.frame.y += 16.0f;
        c.z = ++maxZ;
        page->components.push_back(std::move(c));
        selection.insert(page->components.back().id);
    }
}

void PlannerContext::duplicateSelection() {
    copySelection();
    pasteClipboard();
}

} // namespace softg::planner
