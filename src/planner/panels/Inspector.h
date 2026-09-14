// 属性面板：选中组件的类型化属性编辑 + 几何 + 层级 + 可见/锁定。
#pragma once

namespace pv::planner {
struct PlannerContext;
}

namespace pv::planner::panels {

void drawInspector(PlannerContext& ctx);

} // namespace pv::planner::panels
