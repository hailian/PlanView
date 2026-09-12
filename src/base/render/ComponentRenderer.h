// ComponentRenderer — 组件绘制唯一入口：设计器画布与运行器页面渲染共用。
#pragma once

#include "imgui.h"

#include "base/model/Component.h"
#include "base/render/RenderContext.h"

namespace softg {

class ComponentRenderer {
public:
    // origin/scale：页面坐标 -> 屏幕坐标变换（screen = origin + page * scale）
    static void drawComponent(ImDrawList* dl, const Component& c, const RenderContext& ctx,
                              const ImVec2& origin, float scale);
};

} // namespace softg
