// 页面：组件的容器，按 z 升序排列（不变量）。
#pragma once

#include <algorithm>

#include "base/model/Component.h"
#include "base/model/Types.h"

namespace softg {

struct Page {
    PageId id;
    std::string name;
    ImVec2 size = ImVec2(1280, 800);
    uint32_t background = IM_COL32(30, 30, 46, 255);
    std::vector<Component> components;  // 始终按 z 升序

    Component* find(const ComponentId& id) {
        for (auto& c : components)
            if (c.id == id) return &c;
        return nullptr;
    }
    const Component* find(const ComponentId& id) const {
        for (const auto& c : components)
            if (c.id == id) return &c;
        return nullptr;
    }
    void sortComponents() {
        std::stable_sort(components.begin(), components.end(),
                         [](const Component& a, const Component& b) { return a.z < b.z; });
    }
};

} // namespace softg
