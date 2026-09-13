#include "base/render/ComponentRenderer.h"

#include "base/render/widgets/Widgets.h"

namespace softg {

void ComponentRenderer::drawComponent(ImDrawList* dl, const Component& c, const RenderContext& ctx,
                                      const ImVec2& origin, float scale) {
    if (!c.visible)
        return;

    ScreenRect r(ImVec2(origin.x + c.frame.x * scale, origin.y + c.frame.y * scale),
             ImVec2(origin.x + (c.frame.x + c.frame.w) * scale,
                    origin.y + (c.frame.y + c.frame.h) * scale));
    if (r.width() < 1.0f || r.height() < 1.0f)
        return;

    dl->PushClipRect(ImVec2(r.Min.x - 8, r.Min.y - 8), ImVec2(r.Max.x + 8, r.Max.y + 8), true);

    using namespace widgets;
    if (c.typeId == "Label") drawLabel(dl, r, c, ctx, scale);
    else if (c.typeId == "Button") drawButton(dl, r, c, ctx, scale);
    else if (c.typeId == "Lamp") drawLamp(dl, r, c, ctx, scale);
    else if (c.typeId == "Gauge") drawGauge(dl, r, c, ctx, scale);
    else if (c.typeId == "Chart") drawChart(dl, r, c, ctx, scale);
    else if (c.typeId == "Switch") drawSwitch(dl, r, c, ctx, scale);
    else if (c.typeId == "Slider") drawSlider(dl, r, c, ctx, scale);
    else if (c.typeId == "Panel") drawPanel(dl, r, c, ctx, scale);
    else if (c.typeId == "Image") drawImage(dl, r, c, ctx, scale);
    else if (c.typeId == "DataSource") drawDataSource(dl, r, c, ctx, scale);
    else drawUnknown(dl, r, c, ctx, scale);

    // ---- 告警视觉统一叠加（三种样式，颜色随严重度） ----
    if (ctx.alarms) {
        AlarmVisual av;
        if (ctx.alarms->alarmOf(c.id, av)) {
            ImU32 color = av.severity == AlarmSeverity::Critical
                              ? IM_COL32(255, 60, 60, 255)
                              : av.severity == AlarmSeverity::High
                                    ? IM_COL32(255, 160, 0, 255)
                                    : IM_COL32(255, 230, 80, 255);
            float alpha = av.style == AlarmStyle::Flash ? ctx.flashPhase() : 1.0f;
            if (av.style == AlarmStyle::Color) {
                dl->AddRectFilled(r.Min, r.Max, (color & 0x00FFFFFF) |
                                                    (uint32_t)(60 * alpha) << 24);
            } else {
                float thick = 3.0f * std::max(scale, 0.6f);
                dl->AddRect(ImVec2(r.Min.x - 2, r.Min.y - 2), ImVec2(r.Max.x + 2, r.Max.y + 2),
                            (color & 0x00FFFFFF) | (uint32_t)(255 * alpha) << 24, 6.0f * scale, 0,
                            thick);
            }
        }
    }

    dl->PopClipRect();
}

} // namespace softg
