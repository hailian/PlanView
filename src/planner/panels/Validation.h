// 校验面板：扫描悬空引用（失效的组件/标签/页面引用），双击定位。
#pragma once

namespace pv::planner {
struct PlannerContext;
}

namespace pv::planner::panels {

void drawValidation(PlannerContext& ctx);

} // namespace pv::planner::panels
