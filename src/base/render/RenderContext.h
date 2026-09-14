// RenderContext — 组件绘制的上下文：模式(设计/运行)、时钟、告警态、数据提供方。
// 设计器画布与运行器渲染共用同一 ComponentRenderer，本结构是两条数据分支的开关。
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "base/model/Component.h"
#include "base/model/Types.h"

namespace softg {

class RuntimeEngine;      // M6 实现（前向声明，运行模式注入）
class TextureCache;

// 屏幕空间矩形（imgui 1.92 起 ImRect 属内部头，此处自带最小实现）
struct ScreenRect {
    ImVec2 Min{0, 0}, Max{0, 0};
    ScreenRect() = default;
    ScreenRect(ImVec2 a, ImVec2 b) : Min(a), Max(b) {}
    float width() const { return Max.x - Min.x; }
    float height() const { return Max.y - Min.y; }
    ImVec2 center() const { return ImVec2((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f); }
};

// 告警视觉状态（渲染只关心这两项）
struct AlarmVisual {
    AlarmSeverity severity = AlarmSeverity::Low;
    AlarmStyle style = AlarmStyle::Flash;
};

// 组件属性的运行时解析源：运行模式由 RuntimeEngine 实现（绑定值优先于本地属性）。
// 为空（设计模式/未连接）时渲染直接读组件本地属性。
class IPropertyProvider {
public:
    virtual ~IPropertyProvider() = default;
    virtual const PropertyValue* resolved(const ComponentId& id, std::string_view key) const = 0;
};

// 曲线历史数据源：运行模式由 RuntimeEngine 实现（按绑定 tag 的环形缓冲采样）。
// 为空时（设计模式）渲染画合成正弦演示波。
struct ChartPoint {
    std::chrono::steady_clock::time_point t{};
    float v = 0.0f;
};
class IChartSeriesSource {
public:
    virtual ~IChartSeriesSource() = default;
    virtual void getSeries(const ComponentId& compId, std::vector<ChartPoint>& out) const = 0;
};

// 告警状态源：运行模式由 RuntimeEngine 实现。
class IAlarmSource {
public:
    virtual ~IAlarmSource() = default;
    virtual bool alarmOf(const ComponentId& id, AlarmVisual& out) const = 0;
};

struct RenderContext {
    enum class Mode : uint8_t { Design, Runtime };

    Mode mode = Mode::Design;
    std::chrono::steady_clock::time_point now{};  // 闪烁相位/演示动画用

    IPropertyProvider* properties = nullptr;  // Runtime: 引擎; Design: null
    IChartSeriesSource* chartSeries = nullptr;
    IAlarmSource* alarms = nullptr;
    TextureCache* textures = nullptr;  // Image 组件用; 两 app 各自持有

    // 通信组件运行统计（Runtime 由 PageViewer 填充；设计器为默认值不显示）：
    // 数据源卡显示最后一帧接收时间；协议配置卡按协议名查自己的帧计数
    //（协议组多帧头：各协议只统计命中自己帧头的帧；不在表中的协议不显示计数）
    bool commStatsValid = false;
    std::string_view dsLastFrameTime;          // "HH:MM:SS"，空串 = 尚未收到
    const std::map<std::string, uint64_t>* protoFrameCounts = nullptr; // 协议名 -> 帧计数

    // 复合卡片（绑定协议字段）左上角字段名的统一字号（工程设置；两 app 各自填充）
    float bindTitleFontSize = 11.0f;

    // 0..1 方波闪烁相位（1Hz）
    float flashPhase() const {
        using namespace std::chrono;
        constexpr auto period = duration_cast<steady_clock::duration>(milliseconds(500));
        auto ticks = now.time_since_epoch().count();
        auto half = period.count();
        return (ticks / half) % 2 == 0 ? 1.0f : 0.15f;
    }

    // 解析组件属性：Runtime 有 provider 时绑定值优先
    PropertyValue prop(const Component& c, std::string_view key,
                       const PropertyValue& fallback) const {
        if (properties) {
            if (const PropertyValue* v = properties->resolved(c.id, key)) return *v;
        }
        return c.propOr(key, fallback);
    }
};

} // namespace softg
