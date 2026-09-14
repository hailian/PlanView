// Widget 绘制例程 — 全部 ImDrawList 手绘，无 ImGui 窗口控件、无内部状态。
// 设计器画布与运行器页面共用这些函数（所见即所得的唯一实现点）。
// 约定：r 为屏幕空间矩形；文本字号等以页面空间像素给出，调用方换算。
#pragma once

#include "imgui.h"

#include "base/model/Component.h"
#include "base/render/RenderContext.h"

namespace pv::widgets {

void drawLabel(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale);
void drawButton(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
                float scale);
void drawLamp(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
              float scale);
void drawGauge(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale);
void drawChart(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale);
void drawSwitch(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
                float scale);
void drawSlider(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
                float scale);
void drawPanel(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale);
void drawImage(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale);
void drawDataSource(ImDrawList* dl, const ScreenRect& r, const Component& c,
                    const RenderContext& ctx, float scale);
void drawDataSink(ImDrawList* dl, const ScreenRect& r, const Component& c,
                  const RenderContext& ctx, float scale);
void drawProtocolConfig(ImDrawList* dl, const ScreenRect& r, const Component& c,
                        const RenderContext& ctx, float scale);
void drawProtocolGroup(ImDrawList* dl, const ScreenRect& r, const Component& c,
                       const RenderContext& ctx, float scale);

// 未知组件类型的占位绘制
void drawUnknown(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
                 float scale);

} // namespace pv::widgets
