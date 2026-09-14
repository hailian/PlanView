// 关联关系面板：三类关联（数据绑定/联动/告警）的总览列表、过滤、编辑对话框、跳转定位。
#pragma once

namespace pv::planner {
struct PlannerContext;
}

namespace pv::planner::panels {

void drawAssociations(PlannerContext& ctx);

} // namespace pv::planner::panels
