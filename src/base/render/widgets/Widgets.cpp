#include "base/render/widgets/Widgets.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "base/render/TextureCache.h"

namespace softg::widgets {

using namespace std::chrono;

// ---- 现代深色默认配色（与 ComponentRegistry 内置默认值一致；仅作属性缺省回退） ----
constexpr ImU32 kDefaultTextFg = IM_COL32(230, 233, 239, 255);   // #E6E9EF
constexpr ImU32 kDefaultBtnBg = IM_COL32(59, 130, 246, 255);     // #3B82F6 品牌蓝
constexpr ImU32 kDefaultBtnPressed = IM_COL32(37, 99, 235, 255); // #2563EB
constexpr ImU32 kDefaultLampOn = IM_COL32(34, 197, 94, 255);     // #22C55E
constexpr ImU32 kDefaultLampOff = IM_COL32(58, 64, 77, 255);     // #3A404D
constexpr ImU32 kDefaultNeedle = IM_COL32(239, 68, 68, 255);     // #EF4444
constexpr ImU32 kDefaultArc = IM_COL32(76, 201, 240, 255);       // #4CC9F0 青
constexpr ImU32 kDefaultPanelFill = IM_COL32(28, 33, 48, 255);   // #1C2130
constexpr ImU32 kDefaultPanelBorder = IM_COL32(46, 53, 66, 255); // #2E3542

// ---- 内部小工具 ----

static ImFont* font() { return ImGui::GetFont(); }

// 文本行宽（页面空间字号）
static float textWidth(const char* text, float fontSize) {
    return font()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text).x;
}

// 按最大宽度适配文本：先降字号（下限 minSize），仍超宽则截断加省略号。
// 原地改写 text，返回实际使用的字号——保证文本始终被卡片框住。
static float fitText(std::string& text, float fontSize, float maxW, float minSize) {
    while (fontSize > minSize && textWidth(text.c_str(), fontSize) > maxW)
        fontSize -= 1.0f;
    if (textWidth(text.c_str(), fontSize) <= maxW) return fontSize;
    auto popChar = [](std::string& t) { // 回退一个完整 UTF-8 字符
        if (t.empty()) return;
        t.pop_back();
        while (!t.empty() && (unsigned char)t.back() >= 0x80 &&
               (unsigned char)t.back() < 0xC0)
            t.pop_back();
    };
    std::string ell = "…";
    while (!text.empty() && textWidth((text + ell).c_str(), fontSize) > maxW)
        popChar(text);
    text += ell;
    return fontSize;
}

// 保留原 alpha，仅缩放 RGB（f>1 提亮，f<1 压暗）
static ImU32 shade(ImU32 c, float f) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.x = std::clamp(v.x * f, 0.0f, 1.0f);
    v.y = std::clamp(v.y * f, 0.0f, 1.0f);
    v.z = std::clamp(v.z * f, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(v);
}

static ImU32 withAlpha(ImU32 c, uint32_t a) { return (c & 0x00FFFFFF) | (a << 24); }

// 底部柔和投影（卡片悬浮感）：深色圆角矩形向下偏移，画在主体填充之前
static void dropShadow(ImDrawList* dl, const ScreenRect& r, float radius, float scale, ImU32 col) {
    float d = std::max(1.5f, 2.0f * scale);
    dl->AddRectFilled(ImVec2(r.Min.x + d * 0.5f, r.Min.y + d),
                      ImVec2(r.Max.x + d * 0.5f, r.Max.y + d), col, radius);
}

// 顶边内侧高光线（扁平设计的"提亮"，模拟顶光）
static void topHighlight(ImDrawList* dl, const ScreenRect& r, float radius, float scale, ImU32 col) {
    float inset = std::max(radius, 3.0f * scale);
    float t = std::max(1.0f, scale);
    dl->AddLine(ImVec2(r.Min.x + inset, r.Min.y + t), ImVec2(r.Max.x - inset, r.Min.y + t), col, t);
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

// 绑定协议字段的复合卡片（数据源信息卡同风格）：深色圆角底 + 青色左竖条 + 左上角
// 字段名。返回 false = 未绑定（不画任何东西，调用方走原渲染）；inner = 内容区
// （顶部让出字段名高度）。
bool drawBindCard(ImDrawList* dl, const ScreenRect& r, const Component& c, float scale,
                  ScreenRect& inner) {
    std::string bf = props::asString(c.propOr("bindField", std::string()));
    if (bf.empty()) {
        inner = r;
        return false;
    }
    std::string fieldName = bf; // bindField = "协议名/字段名" -> 取字段名
    size_t slash = bf.find('/');
    if (slash != std::string::npos) fieldName = bf.substr(slash + 1);
    float titleSize = fitText(fieldName, 11.0f * scale, r.width() - 24.0f * scale,
                              9.0f * scale); // 长字段名缩号/省略，不出卡片

    // 指标卡：柔和圆角 + 底部渐层暗示 + 标题下细分隔线；左上角青点替代粗色条
    float radius = std::clamp(6.0f * scale, 0.0f, std::min(r.width(), r.height()) * 0.5f);
    dropShadow(dl, r, radius, scale, IM_COL32(0, 0, 0, 60));
    dl->AddRectFilled(r.Min, r.Max, IM_COL32(22, 28, 40, 255), radius);
    dl->AddRect(r.Min, r.Max, kDefaultPanelBorder, radius, 0, 1.0f * scale);
    // 标题行：左上小圆点（协议驱动标识）+ 字段名
    float dotY = r.Min.y + 5.0f * scale + titleSize * 0.5f;
    dl->AddCircleFilled(ImVec2(r.Min.x + 10.0f * scale, dotY), 2.2f * scale,
                        withAlpha(kDefaultArc, 190), 10);
    dl->AddText(font(), titleSize, ImVec2(r.Min.x + 17.0f * scale, r.Min.y + 5.0f * scale),
                IM_COL32(125, 140, 168, 255), fieldName.c_str());
    // 标题与内容间的细分隔线（不到边，留呼吸感）
    float sepY = r.Min.y + 20.0f * scale;
    if (r.height() > 30.0f * scale)
        dl->AddLine(ImVec2(r.Min.x + 10.0f * scale, sepY),
                    ImVec2(r.Max.x - 10.0f * scale, sepY), IM_COL32(255, 255, 255, 22));
    inner = ScreenRect(ImVec2(r.Min.x + 6.0f * scale, sepY + 3.0f * scale),
                       ImVec2(r.Max.x - 6.0f * scale, r.Max.y - 4.0f * scale));
    return true;
}

// ---- Label 文本 ----
void drawLabel(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale) {
    // text 经 ctx 解析：运行模式绑定值（文本化后的标签值）优先于本地属性
    std::string text = props::asString(ctx.prop(c, "text", PropertyValue(std::string("文本"))));
    float fontSize = (float)props::asInt(c.propOr("fontSize", int64_t(18))) * scale;
    ImU32 color = props::asColor(c.propOr("color", (uint32_t)kDefaultTextFg));
    std::string align = props::asString(c.propOr("align", std::string("居中")));

    ScreenRect area = r;
    if (!drawBindCard(dl, r, c, scale, area)) { // 未绑定：纯文本渲染
        float tw = textWidth(text.c_str(), fontSize);
        float x = r.Min.x;
        if (align == "居中")
            x = r.Min.x + (r.width() - tw) * 0.5f;
        else if (align == "右")
            x = r.Max.x - tw;
        float y = r.Min.y + (r.height() - fontSize) * 0.5f;
        dl->AddText(font(), fontSize, ImVec2(x, y), color, text.c_str());
        return;
    }
    // 绑定协议字段：复合卡片（左上角字段名由卡片绘制），值同时适配内容区的宽与高
    //（默认 32 高的卡片让出标题后内容区仅 12 高，须先按高度收字号再按宽度收）
    fontSize = std::clamp(fontSize, 8.0f * scale, std::max(8.0f * scale,
                                                           area.height() - 4.0f * scale));
    fontSize = fitText(text, fontSize, area.width(), 8.0f * scale);
    float tw = textWidth(text.c_str(), fontSize);
    float x = area.Min.x;
    if (align == "居中")
        x = area.Min.x + (area.width() - tw) * 0.5f;
    else if (align == "右")
        x = area.Max.x - tw;
    float y = area.Min.y + (area.height() - fontSize) * 0.5f;
    dl->AddText(font(), fontSize, ImVec2(x, y), color, text.c_str());
}

// ---- Button 按钮 ----
void drawButton(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
                float scale) {
    (void)ctx;
    std::string text = props::asString(c.propOr("text", std::string("按钮")));
    float fontSize = (float)props::asInt(c.propOr("fontSize", int64_t(18))) * scale;
    ImU32 fg = props::asColor(c.propOr("fgColor", (uint32_t)IM_COL32(255, 255, 255, 255)));
    ImU32 bg = props::asColor(c.propOr("bgColor", (uint32_t)kDefaultBtnBg));
    bool pressed = props::asBool(c.propOr("_pressed", false));  // 运行时瞬时反馈（viewer 写入）
    if (pressed)
        bg = props::asColor(c.propOr("pressedColor", (uint32_t)kDefaultBtnPressed));
    float radius = std::clamp((float)props::asDouble(c.propOr("radius", 8.0)) * scale, 0.0f,
                              std::min(r.width(), r.height()) * 0.5f);

    // 卡片质感：投影 -> 圆角填充 -> 顶部高光 -> 同色系暗描边
    dropShadow(dl, r, radius, scale, IM_COL32(0, 0, 0, 80));
    dl->AddRectFilled(r.Min, r.Max, bg, radius);
    topHighlight(dl, r, radius, scale, IM_COL32(255, 255, 255, 30));
    dl->AddRect(r.Min, r.Max, withAlpha(shade(bg, 0.55f), 200), radius, 0, std::max(1.0f, scale));

    float tw = textWidth(text.c_str(), fontSize);
    float dy = pressed ? scale : 0.0f;  // 按下文字随之下沉，强化反馈
    dl->AddText(font(), fontSize,
                ImVec2(r.Min.x + (r.width() - tw) * 0.5f,
                       r.Min.y + (r.height() - fontSize) * 0.5f + dy),
                fg, text.c_str());
}

// ---- Lamp 指示灯（LED 立体感）----
void drawLamp(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
              float scale) {
    ImU32 onColor = props::asColor(c.propOr("onColor", (uint32_t)kDefaultLampOn));
    ImU32 offColor = props::asColor(c.propOr("offColor", (uint32_t)kDefaultLampOff));
    bool blink = props::asBool(c.propOr("blinkWhenOn", false));

    bool isOn = props::asBool(ctx.prop(c, "isOn", false));
    ImU32 color = isOn ? onColor : offColor;
    if (isOn && blink && ctx.flashPhase() < 0.5f) {
        color = withAlpha(color, 90);  // 半熄灭相位
        isOn = false;                  // 熄灭相位不画光晕
    }

    // 绑定协议字段：复合卡片（左上角字段名），灯体缩进内容区
    ScreenRect area = r;
    drawBindCard(dl, r, c, scale, area);

    ImVec2 center = area.center();
    float radius = std::min(area.width(), area.height()) * 0.5f - 2.0f * scale;

    if (isOn) {  // 点亮光晕（两层衰减）
        dl->AddCircleFilled(center, radius * 1.7f, withAlpha(color, 26), 32);
        dl->AddCircleFilled(center, radius * 1.35f, withAlpha(color, 44), 32);
    }
    // 灯体：主色 -> 上部亮核 -> 左上高光点 -> 暗色外圈
    dl->AddCircleFilled(center, radius, color, 32);
    dl->AddCircleFilled(ImVec2(center.x, center.y + radius * 0.12f), radius * 0.7f,
                        shade(color, isOn ? 1.3f : 1.18f), 24);
    dl->AddCircleFilled(ImVec2(center.x - radius * 0.32f, center.y - radius * 0.36f),
                        radius * 0.22f, IM_COL32(255, 255, 255, isOn ? 150 : 55), 16);
    dl->AddCircle(center, radius, withAlpha(shade(color, 0.45f), 220), 32, 1.5f * scale);
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
    ImU32 needleColor = props::asColor(c.propOr("needleColor", (uint32_t)kDefaultNeedle));
    ImU32 arcColor = props::asColor(c.propOr("arcColor", (uint32_t)kDefaultArc));
    bool showValue = props::asBool(c.propOr("showValue", true));

    // 绑定协议字段：复合卡片（左上角字段名），表盘缩进内容区
    ScreenRect area = r;
    drawBindCard(dl, r, c, scale, area);

    ImVec2 center = area.center();
    float radius = std::min(area.width(), area.height()) * 0.5f - 4.0f * scale;
    float trackW = 7.0f * scale;

    // 背景轨道弧（暗）+ 数值进度弧（亮，圆点起点随值扫过）
    dl->PathArcTo(center, radius, a0, a1, 48);
    dl->PathStroke(IM_COL32(255, 255, 255, 34), 0, trackW);
    float frac = (float)((value - minV) / (maxV - minV));
    if (frac > 0.003f) {
        dl->PathArcTo(center, radius, a0, a0 + (a1 - a0) * frac, 48);
        dl->PathStroke(arcColor, 0, trackW);
    }

    // 刻度（细、淡）
    for (int i = 0; i <= ticks; ++i) {
        float t = (float)i / ticks;
        float ang = a0 + (a1 - a0) * t;
        ImVec2 dir(std::cos(ang), std::sin(ang));
        dl->AddLine(ImVec2(center.x + dir.x * (radius - 11.0f * scale),
                           center.y + dir.y * (radius - 11.0f * scale)),
                    ImVec2(center.x + dir.x * (radius - 5.0f * scale),
                           center.y + dir.y * (radius - 5.0f * scale)),
                    IM_COL32(255, 255, 255, 95), 1.5f * scale);
    }

    // 指针（细长）+ 深色轴芯 + 同指针色芯点
    float ang = a0 + (a1 - a0) * frac;
    ImVec2 dir(std::cos(ang), std::sin(ang));
    ImVec2 perp(-dir.y, dir.x);
    dl->AddTriangleFilled(
        ImVec2(center.x + dir.x * (radius - 15.0f * scale), center.y + dir.y * (radius - 15.0f * scale)),
        ImVec2(center.x - perp.x * 2.6f * scale, center.y - perp.y * 2.6f * scale),
        ImVec2(center.x + perp.x * 2.6f * scale, center.y + perp.y * 2.6f * scale), needleColor);
    dl->AddCircleFilled(center, 6.5f * scale, IM_COL32(23, 28, 36, 255), 20);
    dl->AddCircleFilled(center, 3.2f * scale, needleColor, 16);

    // 数值
    if (showValue) {
        std::string unit = props::asString(c.propOr("unit", std::string()));
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f%s", value, unit.c_str());
        float fontSize = 17.0f * scale;
        float tw = textWidth(buf, fontSize);
        dl->AddText(font(), fontSize,
                    ImVec2(center.x - tw * 0.5f, center.y + radius * 0.38f),
                    kDefaultTextFg, buf);
    }
}

// ---- Chart 曲线（卡片底 + 光晕描线）----
void drawChart(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale) {
    double minV = props::asDouble(c.propOr("min", 0.0));
    double maxV = props::asDouble(c.propOr("max", 100.0));
    if (maxV <= minV) maxV = minV + 1.0;
    ImU32 lineColor = props::asColor(c.propOr("lineColor", (uint32_t)kDefaultArc));
    bool showGrid = props::asBool(c.propOr("showGrid", true));
    int spanSec = (int)props::asInt(c.propOr("spanSec", int64_t(60)));

    // 卡片底：绑定协议字段时走复合卡片（含字段名）；否则原卡片底
    ScreenRect area = r;
    if (drawBindCard(dl, r, c, scale, area)) {
        // 卡片已画底与描边
    } else {
        float radius = 8.0f * scale;
        dl->AddRectFilled(r.Min, r.Max, IM_COL32(13, 17, 24, 200), radius);
        dl->AddRect(r.Min, r.Max, IM_COL32(255, 255, 255, 30), radius, 0, std::max(1.0f, scale));
    }

    // 网格：横线为主、竖线更淡（画在内容区）
    if (showGrid) {
        for (int i = 1; i < 4; ++i) {
            float x = area.Min.x + area.width() * i / 4.0f;
            dl->AddLine(ImVec2(x, area.Min.y), ImVec2(x, area.Max.y), IM_COL32(255, 255, 255, 9));
        }
        for (int i = 1; i < 4; ++i) {
            float y = area.Min.y + area.height() * i / 4.0f;
            dl->AddLine(ImVec2(area.Min.x, y), ImVec2(area.Max.x, y), IM_COL32(255, 255, 255, 16));
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
        float x = area.Min.x + (float)(td / span) * area.width();
        float u = (float)((std::clamp((double)p.v, minV, maxV) - minV) / (maxV - minV));
        float y = area.Max.y - u * area.height();
        return ImVec2(x, y);
    };

    dl->PathLineTo(toScreen(series[0]));
    for (size_t i = 1; i < series.size(); ++i)
        dl->PathLineTo(toScreen(series[i]));
    // 半透明宽线做光晕，再叠实线主线
    dl->PathStroke(withAlpha(lineColor, 60), 0, 6.0f * scale);
    dl->PathStroke(lineColor, 0, 2.2f * scale);
}

// ---- Switch 开关（iOS 式胶囊）----
void drawSwitch(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
                float scale) {
    ImU32 onColor = props::asColor(c.propOr("onColor", (uint32_t)kDefaultLampOn));
    ImU32 offColor = props::asColor(c.propOr("offColor", (uint32_t)kDefaultLampOff));
    bool isOn = props::asBool(ctx.prop(c, "isOn", false));

    float h = std::min(r.height(), std::max(16.0f, 34.0f * scale));
    ImVec2 size(std::max(r.width(), h * 2.0f), h);
    ImVec2 center = r.center();
    ScreenRect tr(ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f),
              ImVec2(center.x + size.x * 0.5f, center.y + size.y * 0.5f));
    float radius = tr.height() * 0.5f;

    dl->AddRectFilled(tr.Min, tr.Max, isOn ? onColor : offColor, radius);
    dl->AddRect(tr.Min, tr.Max, IM_COL32(0, 0, 0, 70), radius, 0, std::max(1.0f, scale));
    float knobR = radius - 3.0f * scale;
    float cx = isOn ? tr.Max.x - knobR - 2.0f * scale : tr.Min.x + knobR + 2.0f * scale;
    // 旋钮：下沉投影 -> 白芯 -> 细暗环
    dl->AddCircleFilled(ImVec2(cx, center.y + 1.0f * scale), knobR, IM_COL32(0, 0, 0, 70), 24);
    dl->AddCircleFilled(ImVec2(cx, center.y), knobR, IM_COL32(248, 250, 252, 255), 24);
    dl->AddCircle(ImVec2(cx, center.y), knobR, IM_COL32(0, 0, 0, 45), 24, std::max(1.0f, scale));
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

    float track = 8.0f * scale;
    float knobR = 10.0f * scale;
    ImU32 filled = withAlpha(kDefaultArc, 235);

    if (vertical) {
        float w = track;
        ImVec2 a(r.center().x - w * 0.5f, r.Max.y), b(r.center().x + w * 0.5f, r.Min.y);
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 42), w * 0.5f);
        float y = r.Max.y - frac * r.height();
        dl->AddRectFilled(ImVec2(a.x, y), ImVec2(b.x, r.Max.y), filled, w * 0.5f);
        ImVec2 k(r.center().x, y);
        dl->AddCircleFilled(ImVec2(k.x, k.y + 1.5f * scale), knobR, IM_COL32(0, 0, 0, 80), 24);
        dl->AddCircleFilled(k, knobR, IM_COL32(248, 250, 252, 255), 24);
        dl->AddCircle(k, knobR, IM_COL32(0, 0, 0, 45), 24, std::max(1.0f, scale));
    } else {
        float h = track;
        ImVec2 a(r.Min.x, r.center().y - h * 0.5f), b(r.Max.x, r.center().y + h * 0.5f);
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 42), h * 0.5f);
        float x = r.Min.x + frac * r.width();
        dl->AddRectFilled(a, ImVec2(x, b.y), filled, h * 0.5f);
        ImVec2 k(x, r.center().y);
        dl->AddCircleFilled(ImVec2(k.x, k.y + 1.5f * scale), knobR, IM_COL32(0, 0, 0, 80), 24);
        dl->AddCircleFilled(k, knobR, IM_COL32(248, 250, 252, 255), 24);
        dl->AddCircle(k, knobR, IM_COL32(0, 0, 0, 45), 24, std::max(1.0f, scale));
    }
}

// ---- Panel 面板（卡片 + 标题分隔线）----
void drawPanel(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale) {
    (void)ctx;
    ImU32 fill = props::asColor(c.propOr("fill", (uint32_t)kDefaultPanelFill));
    ImU32 border = props::asColor(c.propOr("borderColor", (uint32_t)kDefaultPanelBorder));
    float radius = std::clamp((float)props::asDouble(c.propOr("radius", 10.0)) * scale, 0.0f,
                              std::min(r.width(), r.height()) * 0.5f);
    std::string title = props::asString(c.propOr("title", std::string()));

    dropShadow(dl, r, radius, scale, IM_COL32(0, 0, 0, 70));
    dl->AddRectFilled(r.Min, r.Max, fill, radius);
    dl->AddRect(r.Min, r.Max, border, radius, 0, 1.2f * scale);
    if (!title.empty()) {
        float fontSize = 15.0f * scale;
        ImVec2 tp(r.Min.x + 12.0f * scale, r.Min.y + 7.0f * scale);
        dl->AddText(font(), fontSize, tp, IM_COL32(200, 208, 222, 235), title.c_str());
        // 标题下 hairline 分隔线（圆角内收）
        float ly = tp.y + fontSize + 7.0f * scale;
        dl->AddLine(ImVec2(r.Min.x + radius, ly), ImVec2(r.Max.x - radius, ly),
                    IM_COL32(255, 255, 255, 18), std::max(1.0f, scale));
    }
}

// ---- Image 图片 ----
void drawImage(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
               float scale) {
    std::string source = props::asString(c.propOr("source", std::string()));
    bool keepAspect = props::asBool(c.propOr("keepAspect", true));
    float radius = 8.0f * scale;

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
                                IM_COL32(255, 255, 255, 255), radius);
            return;
        }
    }
    // 无图/加载失败：占位网格
    dl->AddRectFilled(r.Min, r.Max, IM_COL32(28, 33, 48, 220), radius);
    dl->AddRect(r.Min, r.Max, IM_COL32(255, 255, 255, 36), radius, 0, std::max(1.0f, scale));
    dl->AddLine(r.Min, r.Max, IM_COL32(255, 255, 255, 24));
    dl->AddLine(ImVec2(r.Max.x, r.Min.y), ImVec2(r.Min.x, r.Max.y), IM_COL32(255, 255, 255, 24));
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

// ---- DataSource 数据源（通信组件信息卡：传输配置 + 关联协议）----
void drawDataSource(ImDrawList* dl, const ScreenRect& r, const Component& c,
                    const RenderContext& ctx, float scale) {
    (void)ctx;
    float radius = std::clamp(10.0f * scale, 0.0f, std::min(r.width(), r.height()) * 0.5f);
    bool autoStart = props::asBool(c.propOr("autoStart", false));
    // 未设自动启动：左侧品牌竖条置灰 + 标题旁标注（PageViewer 打开后需手动启动）
    ImU32 accent = autoStart ? kDefaultArc : IM_COL32(96, 102, 114, 255);

    dropShadow(dl, r, radius, scale, IM_COL32(0, 0, 0, 70));
    dl->AddRectFilled(r.Min, r.Max, IM_COL32(20, 26, 38, 255), radius);
    dl->AddRect(r.Min, r.Max, kDefaultPanelBorder, radius, 0, 1.2f * scale);
    // 左侧通信竖条
    dl->AddRectFilled(ImVec2(r.Min.x, r.Min.y + radius), ImVec2(r.Min.x + 4.0f * scale, r.Max.y - radius),
                      accent, 2.0f * scale);

    std::string transport = props::asString(c.propOr("transport", std::string("UDP")));
    std::string udpRole = props::asString(c.propOr("udpRole", std::string("服务端")));
    std::string tcpRole = props::asString(c.propOr("tcpRole", std::string("客户端")));
    std::string host = props::asString(c.propOr("host", std::string("127.0.0.1")));
    int64_t remotePort = props::asInt(c.propOr("remotePort", int64_t(9001)));
    int64_t localPort = props::asInt(c.propOr("localPort", int64_t(9001)));
    std::string serialPort = props::asString(c.propOr("serialPort", std::string("COM1")));
    int64_t baud = props::asInt(c.propOr("baud", int64_t(9600)));
    int64_t dataBits = props::asInt(c.propOr("dataBits", int64_t(8)));
    std::string parity = props::asString(c.propOr("parity", std::string("无")));
    int64_t stopBits = props::asInt(c.propOr("stopBits", int64_t(1)));
    std::string protocol = props::asString(c.propOr("protocol", std::string()));
    std::string group = props::asString(c.propOr("group", std::string()));
    std::string assocText = !group.empty() ? "[组] " + group : protocol; // 组优先展示

    dl->AddText(font(), 15.0f * scale, ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 8.0f * scale),
                kDefaultTextFg, "数据源");
    if (!autoStart) { // 标题右侧标注（按标题实测宽度定位，避免重叠）
        ImVec2 ts = font()->CalcTextSizeA(15.0f * scale, FLT_MAX, -1.0f, "数据源");
        dl->AddText(font(), 13.0f * scale,
                    ImVec2(r.Min.x + 14.0f * scale + ts.x + 6.0f * scale, r.Min.y + 9.0f * scale),
                    IM_COL32(150, 150, 110, 255), "需手动启动");
    }
    char line[128];
    if (transport == "串口") { // 参数行："COM3 115200-8-N-1"
        std::snprintf(line, sizeof(line), "串口 %s %lld-%lld-%c-%lld", serialPort.c_str(),
                      (long long)baud, (long long)dataBits,
                      parity == "奇" ? 'O' : parity == "偶" ? 'E' : 'N', (long long)stopBits);
    } else if (transport == "TCP") {
        if (tcpRole == "服务端")
            std::snprintf(line, sizeof(line), "TCP服务端 :%lld", (long long)localPort);
        else
            std::snprintf(line, sizeof(line), "TCP %s:%lld", host.c_str(),
                          (long long)remotePort);
    } else if (udpRole == "客户端") {
        std::snprintf(line, sizeof(line), "UDP客户端 -> %s:%lld", host.c_str(),
                      (long long)remotePort);
    } else {
        std::snprintf(line, sizeof(line), "UDP服务端 :%lld", (long long)localPort);
    }
    dl->AddText(font(), 13.0f * scale, ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 30.0f * scale),
                IM_COL32(160, 172, 192, 255), line);
    std::snprintf(line, sizeof(line), "协议: %s",
                  assocText.empty() ? "(未关联)" : assocText.c_str());
    dl->AddText(font(), 12.0f * scale, ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 50.0f * scale),
                assocText.empty() ? IM_COL32(200, 120, 90, 255) : IM_COL32(120, 150, 190, 255),
                line);
    // 运行态：最后一帧接收时间（设计器不显示）
    if (ctx.commStatsValid) {
        snprintf(line, sizeof(line), "最后帧: %s",
                 ctx.dsLastFrameTime.empty() ? "-" : ctx.dsLastFrameTime.data());
        dl->AddText(font(), 11.0f * scale,
                    ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 68.0f * scale),
                    IM_COL32(150, 165, 185, 255), line);
    }
}

// ---- DataSink 数据目的（通信组件信息卡：关联数据源 + 转发端点）----
void drawDataSink(ImDrawList* dl, const ScreenRect& r, const Component& c,
                  const RenderContext& ctx, float scale) {
    (void)ctx;
    float radius = std::clamp(10.0f * scale, 0.0f, std::min(r.width(), r.height()) * 0.5f);

    dropShadow(dl, r, radius, scale, IM_COL32(0, 0, 0, 70));
    dl->AddRectFilled(r.Min, r.Max, IM_COL32(26, 22, 38, 255), radius); // 偏紫底区别于数据源
    dl->AddRect(r.Min, r.Max, kDefaultPanelBorder, radius, 0, 1.2f * scale);
    // 左侧紫色竖条（转发语义，与数据源青/协议蓝区分）
    dl->AddRectFilled(ImVec2(r.Min.x, r.Min.y + radius), ImVec2(r.Min.x + 4.0f * scale, r.Max.y - radius),
                      IM_COL32(167, 139, 250, 255), 2.0f * scale);

    std::string transport = props::asString(c.propOr("transport", std::string("TCP")));
    std::string udpRole = props::asString(c.propOr("udpRole", std::string("客户端")));
    std::string tcpRole = props::asString(c.propOr("tcpRole", std::string("客户端")));
    std::string host = props::asString(c.propOr("host", std::string("127.0.0.1")));
    int64_t remotePort = props::asInt(c.propOr("remotePort", int64_t(9002)));
    int64_t localPort = props::asInt(c.propOr("localPort", int64_t(9002)));
    std::string serialPort = props::asString(c.propOr("serialPort", std::string("COM1")));
    int64_t baud = props::asInt(c.propOr("baud", int64_t(9600)));
    std::string source = props::asString(c.propOr("source", std::string()));

    dl->AddText(font(), 15.0f * scale, ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 8.0f * scale),
                kDefaultTextFg, "数据目的");
    char line[128];
    if (transport == "串口") {
        std::snprintf(line, sizeof(line), "-> 串口 %s %lld", serialPort.c_str(),
                      (long long)baud);
    } else if (transport == "TCP") {
        if (tcpRole == "服务端")
            std::snprintf(line, sizeof(line), "-> TCP服务端 :%lld", (long long)localPort);
        else
            std::snprintf(line, sizeof(line), "-> TCP %s:%lld", host.c_str(),
                          (long long)remotePort);
    } else if (udpRole == "客户端") {
        std::snprintf(line, sizeof(line), "-> UDP %s:%lld", host.c_str(),
                      (long long)remotePort);
    } else {
        std::snprintf(line, sizeof(line), "-> UDP服务端 :%lld", (long long)localPort);
    }
    dl->AddText(font(), 13.0f * scale, ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 30.0f * scale),
                IM_COL32(170, 160, 200, 255), line);
    std::snprintf(line, sizeof(line), "源: %s", source.empty() ? "(未关联)" : source.c_str());
    dl->AddText(font(), 12.0f * scale, ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 50.0f * scale),
                source.empty() ? IM_COL32(200, 120, 90, 255) : IM_COL32(167, 139, 250, 255),
                line);
}

// ---- ProtocolConfig 协议配置（通信组件信息卡：拆帧方式 + 规约字段数）----
void drawProtocolConfig(ImDrawList* dl, const ScreenRect& r, const Component& c,
                        const RenderContext& ctx, float scale) {
    float radius = std::clamp(10.0f * scale, 0.0f, std::min(r.width(), r.height()) * 0.5f);

    dropShadow(dl, r, radius, scale, IM_COL32(0, 0, 0, 70));
    dl->AddRectFilled(r.Min, r.Max, IM_COL32(22, 24, 34, 255), radius);
    dl->AddRect(r.Min, r.Max, kDefaultPanelBorder, radius, 0, 1.2f * scale);
    dl->AddRectFilled(ImVec2(r.Min.x, r.Min.y + radius), ImVec2(r.Min.x + 4.0f * scale, r.Max.y - radius),
                      kDefaultBtnBg, 2.0f * scale); // 左侧品牌蓝竖条（区别于数据源的青色）

    bool tlv = props::asString(c.propOr("framingMode", std::string("TLV"))) == "TLV";
    int64_t fieldCount = props::asInt(c.propOr("fieldCount", int64_t(0)));

    dl->AddText(font(), 15.0f * scale, ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 8.0f * scale),
                kDefaultTextFg, "协议配置");
    char line[128];
    if (tlv) {
        std::snprintf(line, sizeof(line), "TLV  T:%lld L:%lld %s",
                      (long long)props::asInt(c.propOr("tagBytes", int64_t(1))),
                      (long long)props::asInt(c.propOr("lenBytes", int64_t(2))),
                      props::asBool(c.propOr("bigEndian", true)) ? "大端" : "小端");
    } else {
        std::string hex = props::asString(c.propOr("headerHex", std::string("AA 55")));
        std::snprintf(line, sizeof(line), "帧头+Len  [%s]", hex.c_str());
    }
    dl->AddText(font(), 13.0f * scale, ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 30.0f * scale),
                IM_COL32(160, 172, 192, 255), line);
    std::snprintf(line, sizeof(line), "%lld 字段", (long long)fieldCount);
    dl->AddText(font(), 12.0f * scale, ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 50.0f * scale),
                IM_COL32(120, 132, 152, 255), line);
    // 运行态：符合本协议的帧计数（按协议名查；未被数据源使用的协议不显示）
    if (ctx.commStatsValid && ctx.protoFrameCounts) {
        auto it = ctx.protoFrameCounts->find(c.name);
        if (it != ctx.protoFrameCounts->end()) {
            std::snprintf(line, sizeof(line), "帧计数: %llu",
                          (unsigned long long)it->second);
            dl->AddText(font(), 11.0f * scale,
                        ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 68.0f * scale),
                        IM_COL32(150, 165, 185, 255), line);
        }
    }
}

// ---- ProtocolGroup 协议组（通信组件信息卡：组内协议数 + 成员名）----
void drawProtocolGroup(ImDrawList* dl, const ScreenRect& r, const Component& c,
                       const RenderContext& ctx, float scale) {
    (void)ctx;
    float radius = std::clamp(10.0f * scale, 0.0f, std::min(r.width(), r.height()) * 0.5f);

    dropShadow(dl, r, radius, scale, IM_COL32(0, 0, 0, 70));
    dl->AddRectFilled(r.Min, r.Max, IM_COL32(18, 30, 28, 255), radius); // 偏绿底
    dl->AddRect(r.Min, r.Max, kDefaultPanelBorder, radius, 0, 1.2f * scale);
    dl->AddRectFilled(ImVec2(r.Min.x, r.Min.y + radius), ImVec2(r.Min.x + 4.0f * scale, r.Max.y - radius),
                      IM_COL32(88, 200, 150, 255), 2.0f * scale); // 绿色竖条

    int64_t count = props::asInt(c.propOr("protoCount", int64_t(0)));
    dl->AddText(font(), 15.0f * scale, ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 8.0f * scale),
                kDefaultTextFg, "协议组");
    char line[128];
    std::snprintf(line, sizeof(line), "%lld 协议", (long long)count);
    dl->AddText(font(), 13.0f * scale, ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 30.0f * scale),
                IM_COL32(160, 190, 175, 255), line);
    // 首个成员名预览（重名/空成员由校验面板提示）
    std::string first = props::asString(c.propOr("p0.name", std::string()));
    std::snprintf(line, sizeof(line), "%s%s", first.empty() ? "(空)" : first.c_str(),
                  count > 1 ? " …" : "");
    dl->AddText(font(), 12.0f * scale, ImVec2(r.Min.x + 14.0f * scale, r.Min.y + 50.0f * scale),
                IM_COL32(120, 170, 145, 255), line);
}

// ---- 未知类型占位 ----
void drawUnknown(ImDrawList* dl, const ScreenRect& r, const Component& c, const RenderContext& ctx,
                 float scale) {
    (void)ctx;
    dl->AddRectFilled(r.Min, r.Max, IM_COL32(90, 40, 40, 200), 8.0f * scale);
    dl->AddRect(r.Min, r.Max, IM_COL32(255, 120, 120, 255), 8.0f * scale, 0, 1.5f);
    std::string text = "未知: " + c.typeId;
    float fontSize = 14.0f * scale;
    float tw = textWidth(text.c_str(), fontSize);
    dl->AddText(font(), fontSize,
                ImVec2(r.center().x - tw * 0.5f, r.center().y - fontSize * 0.5f),
                IM_COL32(255, 220, 220, 255), text.c_str());
}

} // namespace softg::widgets
