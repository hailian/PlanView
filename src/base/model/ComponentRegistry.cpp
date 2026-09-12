#include "base/model/ComponentRegistry.h"

namespace softg {

// ---- 内置组件类型的属性规格 ----
namespace {

PropertySpec specBool(std::string key, std::string label, bool def) {
    return {std::move(key), std::move(label), PropertyType::Bool, def, {}, {}};
}
PropertySpec specInt(std::string key, std::string label, int64_t def,
                     std::optional<double> mn = {}, std::optional<double> mx = {}) {
    return {std::move(key), std::move(label), PropertyType::Int, def, mn, mx};
}
PropertySpec specDouble(std::string key, std::string label, double def,
                        std::optional<double> mn = {}, std::optional<double> mx = {}) {
    return {std::move(key), std::move(label), PropertyType::Double, def, mn, mx};
}
PropertySpec specString(std::string key, std::string label, std::string def) {
    return {std::move(key), std::move(label), PropertyType::String, std::move(def), {}, {}};
}
PropertySpec specColor(std::string key, std::string label, uint32_t def) {
    return {std::move(key), std::move(label), PropertyType::Color, def, {}, {}};
}
PropertySpec specEnum(std::string key, std::string label, std::string def,
                      std::vector<std::string> values) {
    return {std::move(key), std::move(label), PropertyType::Enum, std::move(def), {}, {},
            std::move(values)};
}

constexpr uint32_t kWhite = IM_COL32(255, 255, 255, 255);
constexpr uint32_t kTextFg = IM_COL32(234, 234, 234, 255);     // 亮灰白
constexpr uint32_t kBtnBg = IM_COL32(61, 90, 128, 255);        // 蓝灰
constexpr uint32_t kBtnPressed = IM_COL32(152, 193, 217, 255);
constexpr uint32_t kLampOn = IM_COL32(56, 176, 0, 255);        // 绿
constexpr uint32_t kLampOff = IM_COL32(78, 78, 80, 255);       // 暗灰
constexpr uint32_t kNeedle = IM_COL32(230, 57, 70, 255);       // 红
constexpr uint32_t kChartLine = IM_COL32(76, 201, 240, 255);   // 青
constexpr uint32_t kPanelFill = IM_COL32(38, 38, 58, 255);
constexpr uint32_t kPanelBorder = IM_COL32(87, 87, 112, 255);

std::vector<ComponentTypeInfo> builtinTypes() {
    std::vector<ComponentTypeInfo> t;

    { // 文本
        ComponentTypeInfo i{"Label", "文本", "显示", {120, 32}, {}};
        i.properties = {
            specString("text", "文本", "文本"),
            specInt("fontSize", "字号", 18, 8, 96),
            specColor("color", "颜色", kTextFg),
            specEnum("align", "对齐", "居中", {"左", "居中", "右"}),
        };
        t.push_back(std::move(i));
    }
    { // 按钮
        ComponentTypeInfo i{"Button", "按钮", "操作", {100, 40}, {}};
        i.properties = {
            specString("text", "文本", "按钮"),
            specInt("fontSize", "字号", 18, 8, 96),
            specColor("fgColor", "文字颜色", kWhite),
            specColor("bgColor", "背景颜色", kBtnBg),
            specColor("pressedColor", "按下颜色", kBtnPressed),
            specDouble("radius", "圆角", 4.0, 0.0, 32.0),
        };
        t.push_back(std::move(i));
    }
    { // 指示灯
        ComponentTypeInfo i{"Lamp", "指示灯", "显示", {40, 40}, {}};
        i.properties = {
            specBool("isOn", "点亮", false),
            specColor("onColor", "点亮颜色", kLampOn),
            specColor("offColor", "熄灭颜色", kLampOff),
            specBool("blinkWhenOn", "点亮时闪烁", false),
        };
        t.push_back(std::move(i));
    }
    { // 仪表
        ComponentTypeInfo i{"Gauge", "仪表", "显示", {160, 160}, {}};
        i.properties = {
            specDouble("value", "当前值", 0.0),
            specDouble("min", "量程下限", 0.0),
            specDouble("max", "量程上限", 100.0),
            specString("unit", "单位", ""),
            specDouble("startAngle", "起始角", 135.0, -360.0, 360.0),
            specDouble("endAngle", "终止角", 405.0, -360.0, 720.0),
            specInt("majorTicks", "主刻度数", 6, 2, 24),
            specColor("needleColor", "指针颜色", kNeedle),
            specBool("showValue", "显示数值", true),
        };
        t.push_back(std::move(i));
    }
    { // 曲线
        ComponentTypeInfo i{"Chart", "曲线", "显示", {300, 150}, {}};
        i.properties = {
            specDouble("value", "当前值", 0.0), // 运行时由绑定写入/设计器演示
            specDouble("min", "量程下限", 0.0),
            specDouble("max", "量程上限", 100.0),
            specInt("spanSec", "时间跨度(秒)", 60, 5, 3600),
            specColor("lineColor", "曲线颜色", kChartLine),
            specBool("showGrid", "显示网格", true),
        };
        t.push_back(std::move(i));
    }
    { // 开关
        ComponentTypeInfo i{"Switch", "开关", "操作", {64, 32}, {}};
        i.properties = {
            specBool("isOn", "状态", false),
            specColor("onColor", "开启颜色", kLampOn),
            specColor("offColor", "关闭颜色", kLampOff),
        };
        t.push_back(std::move(i));
    }
    { // 滑块
        ComponentTypeInfo i{"Slider", "滑块", "操作", {160, 28}, {}};
        i.properties = {
            specDouble("value", "当前值", 50.0),
            specDouble("min", "量程下限", 0.0),
            specDouble("max", "量程上限", 100.0),
            specEnum("orientation", "方向", "水平", {"水平", "垂直"}),
        };
        t.push_back(std::move(i));
    }
    { // 面板（装饰容器，v1 平面）
        ComponentTypeInfo i{"Panel", "面板", "容器", {300, 200}, {}};
        i.properties = {
            specColor("fill", "填充颜色", kPanelFill),
            specColor("borderColor", "边框颜色", kPanelBorder),
            specDouble("radius", "圆角", 8.0, 0.0, 32.0),
            specString("title", "标题", ""),
        };
        t.push_back(std::move(i));
    }
    { // 图片
        ComponentTypeInfo i{"Image", "图片", "图形", {200, 150}, {}};
        i.properties = {
            specString("source", "图片路径(相对工程)", ""),
            specBool("keepAspect", "保持宽高比", true),
        };
        t.push_back(std::move(i));
    }
    return t;
}

} // namespace

ComponentRegistry::ComponentRegistry() {
    for (auto& info : builtinTypes())
        types_.push_back(std::move(info));
}

ComponentRegistry& ComponentRegistry::instance() {
    static ComponentRegistry inst;
    return inst;
}

void ComponentRegistry::registerType(ComponentTypeInfo info) {
    for (auto& existing : types_) {
        if (existing.typeId == info.typeId) {
            existing = std::move(info);
            return;
        }
    }
    types_.push_back(std::move(info));
}

const ComponentTypeInfo* ComponentRegistry::find(std::string_view typeId) const {
    for (const auto& t : types_)
        if (t.typeId == typeId)
            return &t;
    return nullptr;
}

const std::vector<ComponentTypeInfo>& ComponentRegistry::all() const { return types_; }

Component ComponentRegistry::createComponent(std::string_view typeId, const ComponentId& id) {
    const ComponentTypeInfo* info = instance().find(typeId);
    Component c;
    c.id = id;
    c.typeId = std::string(typeId);
    c.name = info ? info->displayName : std::string(typeId);
    if (info) {
        c.frame = Rect{0, 0, info->defaultSize.x, info->defaultSize.y};
        for (const auto& spec : info->properties)
            c.props[spec.key] = spec.defaultValue;
    }
    return c;
}

} // namespace softg
