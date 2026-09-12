// 组件面板：按分类列出全部已注册组件，拖拽源（拖到画布创建）。
#pragma once

namespace softg::planner {
struct PlannerContext;
}

namespace softg::planner::panels {

void drawPalette(PlannerContext& ctx);

} // namespace softg::planner::panels
