// 属性面板：选中组件的类型化属性编辑 + 几何 + 层级 + 可见/锁定。
#pragma once

namespace softg::planner {
struct PlannerContext;
}

namespace softg::planner::panels {

void drawInspector(PlannerContext& ctx);

} // namespace softg::planner::panels
