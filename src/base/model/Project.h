// Project — 全部状态的聚合点：页面/标签/关联/Id 分配。
// 纯值类型：拷贝即快照（undo 与序列化的基础）。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/data/frame/FrameSourceSettings.h"
#include "base/model/Association.h"
#include "base/model/Page.h"
#include "base/model/TagDatabase.h"
#include "base/model/Types.h"

namespace softg {

// TCP 数据服务器连接设置（自定义行文本协议，见 base/data/tcp/SoftgProtocol.h）
struct TcpSettings {
    std::string host = "127.0.0.1";
    int port = 9000;
    int pollMs = 200;
};

struct ProjectSettings {
    TcpSettings tcp;             // SoftG 行文本协议数据源
    FrameSourceSettings frame;   // 帧数据源（TCP/UDP + 拆帧 + 规约解析）；enabled 时优先
    // 复合卡片（绑定协议字段的显示组件）左上角协议字段名的统一字号
    int bindTitleFontSize = 11;
};

struct Project {
    int schemaVersion = 1;
    std::string name = "未命名工程";
    ProjectSettings settings;
    std::vector<Page> pages;
    TagDatabase tags;
    std::vector<Association> associations;
    uint64_t nextId = 1;  // 所有 "xxx-<n>" 形式 Id 的单调分配源

    // 分配新 Id："comp" -> "comp-12"（同时推进 nextId）
    std::string allocId(std::string_view prefix) {
        return std::string(prefix) + "-" + std::to_string(nextId++);
    }

    Page* findPage(const PageId& id) {
        for (auto& p : pages)
            if (p.id == id) return &p;
        return nullptr;
    }
    const Page* findPage(const PageId& id) const {
        for (const auto& p : pages)
            if (p.id == id) return &p;
        return nullptr;
    }

    // 全工程按组件 Id 查找（遍历页面；联动目标可跨页）
    Component* findComponent(const ComponentId& id) {
        for (auto& p : pages)
            if (Component* c = p.find(id)) return c;
        return nullptr;
    }
    const Component* findComponent(const ComponentId& id) const {
        for (const auto& p : pages)
            if (const Component* c = p.find(id)) return c;
        return nullptr;
    }

    Association* findAssociation(const AssocId& id) {
        for (auto& a : associations)
            if (assocId(a) == id) return &a;
        return nullptr;
    }

    // 组件 Id -> 所在页面（用于画布定位/关联弧线）
    Page* findPageOfComponent(const ComponentId& id) {
        for (auto& p : pages)
            if (p.find(id)) return &p;
        return nullptr;
    }
    const Page* findPageOfComponent(const ComponentId& id) const {
        for (const auto& p : pages)
            if (p.find(id)) return &p;
        return nullptr;
    }

    // 按类型取第一个组件（页序 + 组件序）；数据源组件等全局唯一型组件用
    Component* findComponentByType(std::string_view typeId) {
        for (auto& pg : pages)
            for (auto& c : pg.components)
                if (c.typeId == typeId) return &c;
        return nullptr;
    }
    const Component* findComponentByType(std::string_view typeId) const {
        for (const auto& pg : pages)
            for (const auto& c : pg.components)
                if (c.typeId == typeId) return &c;
        return nullptr;
    }
};

} // namespace softg
