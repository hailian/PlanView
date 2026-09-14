// 组件面板：按分类列出全部已注册组件，拖拽源（拖到画布创建）。
#pragma once

namespace pv::planner {
struct PlannerContext;
}

namespace pv::planner::panels {

void drawPalette(PlannerContext& ctx);

} // namespace pv::planner::panels
