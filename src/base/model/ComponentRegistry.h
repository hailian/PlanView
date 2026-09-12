// 组件类型注册表（进程级单例）：构造时注册全部内置类型。
#pragma once

#include <string_view>

#include "base/model/Component.h"

namespace softg {

class ComponentRegistry {
public:
    static ComponentRegistry& instance();

    void registerType(ComponentTypeInfo info);
    const ComponentTypeInfo* find(std::string_view typeId) const; // 未注册返回 nullptr
    const std::vector<ComponentTypeInfo>& all() const;

    // 按注册类型创建组件实例：填默认属性；id 由调用方（Project）分配
    static Component createComponent(std::string_view typeId, const ComponentId& id);

private:
    ComponentRegistry(); // 注册内置类型
    std::vector<ComponentTypeInfo> types_;
};

} // namespace softg
