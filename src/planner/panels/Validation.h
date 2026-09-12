// 校验面板：扫描悬空引用（失效的组件/标签/页面引用），双击定位。
#pragma once

namespace softg::planner {
struct PlannerContext;
}

namespace softg::planner::panels {

void drawValidation(PlannerContext& ctx);

} // namespace softg::planner::panels
