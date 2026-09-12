// 画布面板 — 设计器的复杂度中心：
// 视图变换(缩放/平移)、选择/框选、拖动/8 手柄缩放、吸附、拖放创建、键盘操作、渲染编排。
#pragma once

namespace softg::planner {
struct PlannerContext;
}

namespace softg::planner::panels {

void drawCanvas(PlannerContext& ctx);

} // namespace softg::planner::panels
