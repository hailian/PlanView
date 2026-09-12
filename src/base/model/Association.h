// 关联关系：三种语义（数据绑定 / 组件间联动 / 阈值告警），variant 判别。
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "base/model/Property.h"
#include "base/model/Types.h"

namespace softg {

// (a) 数据绑定：组件属性 <-> 数据标签
struct DataBinding {
    AssocId id;
    ComponentId component;
    std::string property;  // "value" / "isOn" / "text" / "visible" ...
    TagName tag;
};

// (b) 组件间联动：源组件事件 -> 目标动作
enum class LinkageEvent : uint8_t { Click, ValueChanged, AlarmActive, AlarmCleared };
enum class LinkageAction : uint8_t {
    SetProperty,    // param=属性名, value=值
    Navigate,       // param=目标 PageId（无 target 组件）
    ToggleVisible,  // 翻转 target.visible
    SetTagValue,    // value 写入 param 指定的标签
    Pulse,          // target 短暂高亮（无参数）
};

struct LinkageRule {
    AssocId id;
    ComponentId source;
    LinkageEvent event = LinkageEvent::Click;
    ComponentId target;             // Navigate 类动作可为空
    LinkageAction action = LinkageAction::SetProperty;
    std::string param;              // SetProperty->属性名 / Navigate->PageId / SetTagValue->标签名
    PropertyValue value;            // SetProperty/SetTagValue 的值
};

// (c) 阈值告警：标签比较 -> 视觉告警
struct AlarmRule {
    AssocId id;
    TagName tag;
    Comparator cmp = Comparator::GT;
    double threshold = 0.0;
    AlarmSeverity severity = AlarmSeverity::High;
    AlarmStyle style = AlarmStyle::Flash;
    bool latching = true;                     // 越限恢复后保持告警直到人工确认
    std::vector<ComponentId> components;      // 显式受影响组件；为空时自动覆盖绑定该标签的组件
};

using Association = std::variant<DataBinding, LinkageRule, AlarmRule>;

// 关联 id 访问（不关心具体类型时）
inline const AssocId& assocId(const Association& a) {
    return std::visit([](const auto& x) -> const AssocId& { return x.id; }, a);
}

} // namespace softg
