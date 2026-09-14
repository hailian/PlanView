// 组件与组件类型信息。
#pragma once

#include <string>
#include <vector>

#include "base/model/Property.h"
#include "base/model/Types.h"

namespace pv {

// 组件类型描述：注册表驱动，两款 exe 注册同一套内置类型。
struct ComponentTypeInfo {
    std::string typeId;      // "Gauge"
    std::string displayName; // "仪表"
    std::string category;    // "显示" / "操作" / "容器" / "图形"
    ImVec2 defaultSize = ImVec2(100, 40);
    std::vector<PropertySpec> properties;  // 业务属性；几何/名称/z 不在此列
};

// 页面上的一个组件实例（纯值类型，拷贝即快照）
struct Component {
    ComponentId id;
    std::string typeId;
    std::string name;
    Rect frame;      // 页面绝对坐标
    int z = 0;
    bool visible = true;
    bool locked = false;
    PropertyMap props;

    const PropertyValue* prop(std::string_view key) const {
        auto it = props.find(key);
        return it != props.end() ? &it->second : nullptr;
    }
    PropertyValue propOr(std::string_view key, const PropertyValue& d) const {
        const PropertyValue* p = prop(key);
        return p ? *p : d;
    }
    void setProp(std::string_view key, PropertyValue v) {
        props.insert_or_assign(std::string(key), std::move(v));
    }
};

} // namespace pv
