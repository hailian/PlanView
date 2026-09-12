#include "base/render/widgets/Widgets.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "base/render/TextureCache.h"

namespace softg::widgets {

using namespace std::chrono;

// ---- 内部小工具 ----

static ImFont* font() { return ImGui::GetFont(); }

// 文本行宽（页面空间字号）
static float textWidth(const char* text, float fontSize) {
    return font()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text).x;
}

// 组件 id 的稳定哈希（设计模式演示波形相位差）
static uint32_t idHash(const std::string& id) {
    uint32_t h = 2166136261u;
    for (char ch : id) {
        h ^= (uint8_t)ch;
        h *= 16777619u;
    }
    return h;
}

// 设计模式合成值：0..1 慢扫掠正弦（仪表/滑块演示动画）
static float designSweep(const RenderContext& ctx, const Component& c) {
    float phase = (idHash(c.id) % 1000) / 1000.0f;
    float t = duration_cast<duration<float>>(ctx.now.time_since_epoch()).count();
    return 0.5f + 0.5f * std::sin(2.0f * 3.14159265358979f * (t / 6.0f + phase));
}

// 解析数值型属性：运行模式绑定值优先，设计模式对 "value" 用合成扫掠
static double liveDouble(const RenderContext& ctx, const Component& c, const char* key,
                         double localValue, double minV, double maxV) {
    if (ctx.properties) {
        PropertyValue v = ctx.prop(c, key, localValue);
        return props::asDouble(v, localValue);
    }
    if (ctx.mode == RenderContext::Mode::Design && std::string_view(key) == "value")
        return minV + (maxV - minV) * designSweep(ctx, c);
    return localValue;
}

static ImVec2 clamp01(ImVec2 v) { return ImVec2(std::clamp(v.x, 0.0f, 1.0f), std::clamp(v.y, 0.0f, 1.0f)); }

// ---- Label 文本 ----
void drawLabel(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale) {
    (void)ctx;
    std::string text = props::asString(c.propOr("text", std::string("文本")));
    float fontSize = (float)props::asInt(c.propOr("fontSize", int64_t(18))) * scale;
    ImU32 color = props::asColor(c.propOr("color", (uint32_t)IM_COL32(234, 234, 234, 255)));
    std::string align = props::asString(c.propOr("align", std::string("居中")));

    float tw = textWidth(text.c_str(), fontSize);
    float th = fontSize;
    float x = r.Min.x;
    if (align == "居中")
        x = r.Min.x + (r.width() - tw) * 0.5f;
    else if (align == "右")
        x = r.Max.x - tw;
    float y = r.Min.y + (r.height() - th) * 0.5f;
    dl->AddText(font(), fontSize, ImVec2(x, y), color, text.c_str());
}

// ---- Button 按钮 ----
void drawButton(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
                float scale) {
    (void)ctx;
    std::string text = props::asString(c.propOr("text", std::string("按钮")));
    float fontSize = (float)props::asInt(c.propOr("fontSize", int64_t(18))) * scale;
    ImU32 fg = props::asColor(c.propOr("fgColor", (uint32_t)0xFFFFFFFF));
    ImU32 bg = props::asColor(c.propOr("bgColor", (uint32_t)IM_COL32(61, 90, 128, 255)));
    bool pressed = props::asBool(c.propOr("_pressed", false));  // 运行时瞬时反馈（viewer 写入）
    if (pressed)
        bg = props::asColor(c.propOr("pressedColor", (uint32_t)IM_COL32(152, 193, 217, 255)));
    float radius = (float)props::asDouble(c.propOr("radius", 4.0)) * scale;

    dl->AddRectFilled(r.Min, r.Max, bg, radius);
    dl->AddRect(r.Min, r.Max, IM_COL32(255, 255, 255, 40), radius);

    float tw = textWidth(text.c_str(), fontSize);
    dl->AddText(font(), fontSize,
                ImVec2(r.Min.x + (r.width() - tw) * 0.5f,
                       r.Min.y + (r.height() - fontSize) * 0.5f),
                fg, text.c_str());
}

// ---- Lamp 指示灯 ----
void drawLamp(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
              float scale) {
    (void)scale;
    ImU32 onColor = props::asColor(c.propOr("onColor", (uint32_t)IM_COL32(56, 176, 0, 255)));
    ImU32 offColor = props::asColor(c.propOr("offColor", (uint32_t)IM_COL32(78, 78, 80, 255)));
    bool blink = props::asBool(c.propOr("blinkWhenOn", false));

    bool isOn = props::asBool(ctx.prop(c, "isOn", false));
    ImU32 color = isOn ? onColor : offColor;
    if (isOn && blink && ctx.flashPhase() < 0.5f)
        color = (color & 0x00FFFFFF) | 0x40000000;  // 半熄灭相位

    ImVec2 center = r.center();
    float radius = std::min(r.width(), r.height()) * 0.5f - 2.0f;
    dl->AddCircleFilled(center, radius, color, 24);
    dl->AddCircle(center, radius, IM_COL32(255, 255, 255, 60), 24, 1.5f);
    if (isOn)  // 点亮光晕
        dl->AddCircleFilled(center, radius * 1.35f, (color & 0x00FFFFFF) | 0x28000000, 24);
}

// ---- Gauge 仪表 ----
void drawGauge(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale) {
    double minV = props::asDouble(c.propOr("min", 0.0));
    double maxV = props::asDouble(c.propOr("max", 100.0));
    if (maxV <= minV) maxV = minV + 1.0;
    double value = liveDouble(ctx, c, "value", props::asDouble(c.propOr("value", 0.0)), minV, maxV);
    value = std::clamp(value, minV, maxV);

    float a0 = (float)props::asDouble(c.propOr("startAngle", 135.0)) * 3.14159265358979f / 180.0f;
    float a1 = (float)props::asDouble(c.propOr("endAngle", 405.0)) * 3.14159265358979f / 180.0f;
    int ticks = (int)props::asInt(c.propOr("majorTicks", int64_t(6)));
    ticks = std::clamp(ticks, 2, 24);
    ImU32 needleColor = props::asColor(c.propOr("needleColor", (uint32_t)IM_COL32(230, 57, 70, 255)));
    bool showValue = props::asBool(c.propOr("showValue", true));

    ImVec2 center = r.center();
    float radius = std::min(r.width(), r.height()) * 0.5f - 4.0f * scale;

    // 背景轨道弧
    dl->PathArcTo(center, radius, a0, a1, 48);
    dl->PathStroke(IM_COL32(255, 255, 255, 40), 0, 8.0f * scale);

    // 刻度
    for (int i = 0; i <= ticks; ++i) {
        float t = (float)i / ticks;
        float ang = a0 + (a1 - a0) * t;
        ImVec2 dir(std::cos(ang), std::sin(ang));
        dl->AddLine(ImVec2(center.x + dir.x * (radius - 12.0f * scale),
                           center.y + dir.y * (radius - 12.0f * scale)),
                    ImVec2(center.x + dir.x * (radius - 3.0f * scale),
                           center.y + dir.y * (radius - 3.0f * scale)),
                    IM_COL32(255, 255, 255, 120), 2.0f * scale);
    }

    // 指针
    float frac = (float)((value - minV) / (maxV - minV));
    float ang = a0 + (a1 - a0) * frac;
    ImVec2 dir(std::cos(ang), std::sin(ang));
    dl->AddTriangleFilled(
        ImVec2(center.x + dir.x * (radius - 14.0f * scale), center.y + dir.y * (radius - 14.0f * scale)),
        ImVec2(center.x - dir.y * 4.0f * scale, center.y + dir.x * 4.0f * scale),
        ImVec2(center.x + dir.y * 4.0f * scale, center.y - dir.x * 4.0f * scale), needleColor);
    dl->AddCircleFilled(center, 5.0f * scale, IM_COL32(200, 200, 210, 255), 16);

    // 数值
    if (showValue) {
        std::string unit = props::asString(c.propOr("unit", std::string()));
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f%s", value, unit.c_str());
        float fontSize = 16.0f * scale;
        float tw = textWidth(buf, fontSize);
        dl->AddText(font(), fontSize,
                    ImVec2(center.x - tw * 0.5f, center.y + radius * 0.35f),
                    IM_COL32(235, 235, 245, 255), buf);
    }
}

// ---- Chart 曲线 ----
void drawChart(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale) {
    (void)scale;
    double minV = props::asDouble(c.propOr("min", 0.0));
    double maxV = props::asDouble(c.propOr("max", 100.0));
    if (maxV <= minV) maxV = minV + 1.0;
    ImU32 lineColor = props::asColor(c.propOr("lineColor", (uint32_t)IM_COL32(76, 201, 240, 255)));
    bool showGrid = props::asBool(c.propOr("showGrid", true));
    int spanSec = (int)props::asInt(c.propOr("spanSec", int64_t(60)));

    // 背景
    dl->AddRectFilled(r.Min, r.Max, IM_COL32(0, 0, 0, 70), 4.0f);
    dl->AddRect(r.Min, r.Max, IM_COL32(255, 255, 255, 35), 4.0f);

    // 网格
    if (showGrid) {
        for (int i = 1; i < 4; ++i) {
            float x = r.Min.x + r.width() * i / 4.0f;
            dl->AddLine(ImVec2(x, r.Min.y), ImVec2(x, r.Max.y), IM_COL32(255, 255, 255, 18));
        }
        for (int i = 1; i < 4; ++i) {
            float y = r.Min.y + r.height() * i / 4.0f;
            dl->AddLine(ImVec2(r.Min.x, y), ImVec2(r.Max.x, y), IM_COL32(255, 255, 255, 18));
        }
    }

    // 数据序列：运行模式取引擎历史；设计模式合成正弦
    std::vector<ChartPoint> series;
    if (ctx.chartSeries) {
        ctx.chartSeries->getSeries(c.id, series);
    } else {
        steady_clock::time_point now = ctx.now;
        float phase = (idHash(c.id) % 997) / 997.0f;
        int count = 120;
        for (int i = count; i >= 0; --i) {
            auto t = now - seconds((int64_t)(spanSec * i / (double)count));
            float u = duration_cast<duration<float>>(t.time_since_epoch()).count();
            float v = (float)(0.5 + 0.45 * std::sin(2.0f * 3.14159265358979f * (u / 8.0f + phase)));
            series.push_back({t, v});
        }
    }
    if (series.size() < 2)
        return;

    steady_clock::time_point tEnd = series.back().t;
    steady_clock::time_point tBegin = tEnd - seconds((int64_t)spanSec);

    // 归一化绘制
    auto toScreen = [&](const ChartPoint& p) {
        auto td = duration_cast<duration<double>>(p.t - tBegin).count();
        auto span = duration_cast<duration<double>>(tEnd - tBegin).count() + 1e-9;
        float x = r.Min.x + (float)(td / span) * r.width();
        float u = (float)((std::clamp((double)p.v, minV, maxV) - minV) / (maxV - minV));
        float y = r.Max.y - u * r.height();
        return ImVec2(x, y);
    };

    dl->PathLineTo(toScreen(series[0]));
    for (size_t i = 1; i < series.size(); ++i)
        dl->PathLineTo(toScreen(series[i]));
    dl->PathStroke(lineColor, 0, 2.0f);
}

// ---- Switch 开关 ----
void drawSwitch(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
                float scale) {
    (void)scale;
    ImU32 onColor = props::asColor(c.propOr("onColor", (uint32_t)IM_COL32(56, 176, 0, 255)));
    ImU32 offColor = props::asColor(c.propOr("offColor", (uint32_t)IM_COL32(78, 78, 80, 255)));
    bool isOn = props::asBool(ctx.prop(c, "isOn", false));

    float h = std::min(r.height(), 32.0f);
    ImVec2 size(std::max(r.width(), h * 2.0f), h);
    ImVec2 center = r.center();
    ScreenRect tr(ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f),
              ImVec2(center.x + size.x * 0.5f, center.y + size.y * 0.5f));
    float radius = tr.height() * 0.5f;

    dl->AddRectFilled(tr.Min, tr.Max, isOn ? onColor : offColor, radius);
    float knobR = radius - 3.0f;
    float cx = isOn ? tr.Max.x - knobR - 2.0f : tr.Min.x + knobR + 2.0f;
    dl->AddCircleFilled(ImVec2(cx, center.y), knobR, IM_COL32(240, 240, 245, 255), 20);
}

// ---- Slider 滑块 ----
void drawSlider(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
                float scale) {
    double minV = props::asDouble(c.propOr("min", 0.0));
    double maxV = props::asDouble(c.propOr("max", 100.0));
    if (maxV <= minV) maxV = minV + 1.0;
    double value = liveDouble(ctx, c, "value", props::asDouble(c.propOr("value", 50.0)), minV, maxV);
    value = std::clamp(value, minV, maxV);
    float frac = (float)((value - minV) / (maxV - minV));
    bool vertical = props::asString(c.propOr("orientation", std::string("水平"))) == "垂直";

    if (vertical) {
        float w = 10.0f * scale;
        ImVec2 a(r.center().x - w * 0.5f, r.Max.y), b(r.center().x + w * 0.5f, r.Min.y);
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 45), w * 0.5f);
        float y = r.Max.y - frac * r.height();
        dl->AddRectFilled(ImVec2(a.x, y), ImVec2(b.x, r.Max.y), IM_COL32(76, 201, 240, 220), w * 0.5f);
        dl->AddCircleFilled(ImVec2(r.center().x, y), 9.0f * scale, IM_COL32(240, 240, 245, 255), 20);
    } else {
        float h = 10.0f * scale;
        ImVec2 a(r.Min.x, r.center().y - h * 0.5f), b(r.Max.x, r.center().y + h * 0.5f);
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 45), h * 0.5f);
        float x = r.Min.x + frac * r.width();
        dl->AddRectFilled(a, ImVec2(x, b.y), IM_COL32(76, 201, 240, 220), h * 0.5f);
        dl->AddCircleFilled(ImVec2(x, r.center().y), 9.0f * scale, IM_COL32(240, 240, 245, 255), 20);
    }
}

// ---- Panel 面板 ----
void drawPanel(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale) {
    (void)ctx;
    ImU32 fill = props::asColor(c.propOr("fill", (uint32_t)IM_COL32(38, 38, 58, 255)));
    ImU32 border = props::asColor(c.propOr("borderColor", (uint32_t)IM_COL32(87, 87, 112, 255)));
    float radius = (float)props::asDouble(c.propOr("radius", 8.0)) * scale;
    std::string title = props::asString(c.propOr("title", std::string()));

    dl->AddRectFilled(r.Min, r.Max, fill, radius);
    dl->AddRect(r.Min, r.Max, border, radius, 0, 1.5f * scale);
    if (!title.empty()) {
        float fontSize = 16.0f * scale;
        dl->AddText(font(), fontSize, ImVec2(r.Min.x + 8.0f * scale, r.Min.y + 5.0f * scale),
                    IM_COL32(220, 220, 230, 230), title.c_str());
    }
}

// ---- Image 图片 ----
void drawImage(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale) {
    std::string source = props::asString(c.propOr("source", std::string()));
    bool keepAspect = props::asBool(c.propOr("keepAspect", true));

    if (!source.empty() && ctx.textures) {
        if (const TextureCache::Entry* e = ctx.textures->get(source)) {
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (keepAspect && e->w > 0 && e->h > 0) {
                // 等比缩放 letterbox：调整 uv 裁掉多余部分
                float targetAspect = r.width() / r.height();
                float srcAspect = (float)e->w / (float)e->h;
                if (srcAspect > targetAspect) {
                    float cut = 1.0f - targetAspect / srcAspect;
                    uv0.x = cut * 0.5f;
                    uv1.x = 1.0f - cut * 0.5f;
                } else {
                    float cut = 1.0f - srcAspect / targetAspect;
                    uv0.y = cut * 0.5f;
                    uv1.y = 1.0f - cut * 0.5f;
                }
            }
            dl->AddImageRounded((ImTextureID)e->srv, r.Min, r.Max, uv0, uv1,
                                IM_COL32(255, 255, 255, 255), 4.0f * scale);
            return;
        }
    }
    // 无图/加载失败：占位网格
    dl->AddRectFilled(r.Min, r.Max, IM_COL32(40, 40, 55, 200), 4.0f);
    dl->AddRect(r.Min, r.Max, IM_COL32(255, 255, 255, 40), 4.0f);
    dl->AddLine(r.Min, r.Max, IM_COL32(255, 255, 255, 30));
    dl->AddLine(ImVec2(r.Max.x, r.Min.y), ImVec2(r.Min.x, r.Max.y), IM_COL32(255, 255, 255, 30));
    if (!source.empty()) {
        float fontSize = 13.0f * scale;
        float tw = textWidth(source.c_str(), fontSize);
        dl->AddText(font(), fontSize,
                    ImVec2(r.center().x - tw * 0.5f, r.center().y - fontSize * 0.5f),
                    IM_COL32(255, 200, 120, 220), source.c_str());
    } else {
        const char* text = "图片";
        float fontSize = 14.0f * scale;
        float tw = textWidth(text, fontSize);
        dl->AddText(font(), fontSize,
                    ImVec2(r.center().x - tw * 0.5f, r.center().y - fontSize * 0.5f),
                    IM_COL32(200, 200, 210, 200), text);
    }
}

// ---- 未知类型占位 ----
void drawUnknown(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
                 float scale) {
    (void)ctx;
    dl->AddRectFilled(r.Min, r.Max, IM_COL32(90, 40, 40, 200), 4.0f);
    dl->AddRect(r.Min, r.Max, IM_COL32(255, 120, 120, 255), 4.0f, 0, 1.5f);
    std::string text = "未知: " + c.typeId;
    float fontSize = 14.0f * scale;
    float tw = textWidth(text.c_str(), fontSize);
    dl->AddText(font(), fontSize,
                ImVec2(r.center().x - tw * 0.5f, r.center().y - fontSize * 0.5f),
                IM_COL32(255, 220, 220, 255), text.c_str());
}

} // namespace softg::widgets
