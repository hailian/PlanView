#include "base/model/ComponentRegistry.h"

namespace pv {

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
constexpr uint32_t kTextFg = IM_COL32(230, 233, 239, 255);      // #E6E9EF 亮灰白
constexpr uint32_t kBtnBg = IM_COL32(59, 130, 246, 255);        // #3B82F6 品牌蓝
constexpr uint32_t kBtnPressed = IM_COL32(37, 99, 235, 255);    // #2563EB
constexpr uint32_t kLampOn = IM_COL32(34, 197, 94, 255);        // #22C55E 翠绿
constexpr uint32_t kLampOff = IM_COL32(58, 64, 77, 255);        // #3A404D 暗灰
constexpr uint32_t kNeedle = IM_COL32(239, 68, 68, 255);        // #EF4444 红
constexpr uint32_t kArc = IM_COL32(76, 201, 240, 255);          // #4CC9F0 青
constexpr uint32_t kChartLine = kArc;
constexpr uint32_t kPanelFill = IM_COL32(28, 33, 48, 255);      // #1C2130
constexpr uint32_t kPanelBorder = IM_COL32(46, 53, 66, 255);    // #2E3542

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
            specDouble("radius", "圆角", 8.0, 0.0, 32.0),
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
            specColor("arcColor", "进度弧颜色", kArc),
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
            specDouble("radius", "圆角", 10.0, 0.0, 32.0),
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
    { // 数据源（通信组件：拖到画布即启用，运行器按其配置收报文驱动标签；
      // 拆帧与规约字段由「协议配置」组件提供，经 protocol 属性按名称关联）
        ComponentTypeInfo i{"DataSource", "数据源", "通信", {170, 84}, {}};
        i.properties = {
            // 自动启动默认关：PageViewer 打开工程不主动连接数据源，顶栏可手动启动/停止
            specBool("autoStart", "自动启动", false),
            specEnum("transport", "传输方式", "UDP",
                     {"UDP", "TCP", "串口", "监听", "自发", "USB", "VISA"}),
            // 自发（本地模拟设备）：仅配周期——按关联协议格式周期性产帧直接驱动自身解析
            //（无网络、无端口），字段值按类型确定性递增（整数 0..max 环回/枚举遍历/字符串 a..z）
            specInt("autoSendMs", "发送周期(ms)", 1000, 20, 60000),
            // UDP 角色：服务端=绑定本地端口收任意对端；客户端=connect 远端仅收该对端；
            // 组播=绑定本地端口并加入 host 组播组（224~239 段，同机多消费者可共收）
            specEnum("udpRole", "UDP角色", "服务端", {"服务端", "客户端", "组播"}),
            // TCP 角色：客户端=连接远端；服务端=监听本地端口等待设备接入
            specEnum("tcpRole", "TCP角色", "客户端", {"客户端", "服务端"}),
            specString("host", "主机/目标IP", "127.0.0.1"),
            specInt("remotePort", "远端端口", 9001, 1, 65535),
            specInt("localPort", "本地监听端口", 9001, 1, 65535),
            // 监听参数（仅传输=监听时显示）：固定三元组 dip/dport/协议
            // 监听方式：本机端口=普通 socket（只收发往本机的流量）；
            // 镜像抓包=Npcap 混杂模式（交换机 SPAN 镜像的第三方流量，需装 Npcap）
            specEnum("listenMode", "监听方式", "本机端口", {"本机端口", "镜像抓包"}),
            specString("listenIp", "监听IP(dip)", "*"),
            specInt("listenPort", "监听端口(dport)", 9002, 1, 65535),
            specEnum("listenProto", "监听协议", "UDP", {"UDP", "TCP"}),
            specString("listenNic", "抓包网卡", ""), // 动态下拉（运行时枚举 Npcap 设备）
            // 串口参数（仅传输=串口时显示）
            specString("serialPort", "串口", "COM1"),
            specInt("baud", "波特率", 9600, 300, 921600),
            specInt("dataBits", "数据位", 8, 5, 8),
            specEnum("parity", "校验", "无", {"无", "奇", "偶"}),
            specInt("stopBits", "停止位", 1, 1, 2),
            // USB 参数（仅传输=USB 时显示）：libusb/WinUSB 设备字节流（同串口拆帧）；
            // 设备经 libusb-1.0.dll 枚举（运行时可选依赖，未放置时连接报放置提示）
            specString("usbDevice", "USB设备", ""), // 动态下拉（运行时 libusb 枚举）
            specInt("usbInterface", "USB接口号", 0, 0, 255),
            specString("usbEpIn", "IN端点(hex)", ""),  // 空 = 自动选首个批量/中断 IN
            specString("usbEpOut", "OUT端点(hex)", ""), // 空 = 自动选
            // VISA 参数（仅传输=VISA 时显示）：经 VISA 运行时统一接 USBTMC/GPIB/以太网
            // 仪器（需装 NI-VISA / Keysight IO Libraries 等，未装时连接报安装提示）。
            // 地址以手工输入为主，属性下方另有「枚举仪器」动态下拉回填
            specString("visaAddress", "VISA地址", ""), // 如 TCPIP0::192.168.1.5::inst0::INSTR
            specString("protocol", "关联协议/组", ""), // 协议配置或协议组组件名（统一动态下拉）
        };
        t.push_back(std::move(i));
    }
    { // 协议组（关联多个协议配置；数据源关联组后按组内字段合并解析——
      // 典型 TLV 总线：不同协议按 T 值分段同一数据流，拆帧参数取组内第一个协议）
        ComponentTypeInfo i{"ProtocolGroup", "协议组", "通信", {170, 84}, {}};
        // 成员列表存 p<i>.name 索引属性（Inspector 自定义区编辑，随组件快照进 undo/序列化）
        t.push_back(std::move(i));
    }
    { // 协议配置（拆帧方式 + 规约字段；被数据源组件关联复用，
      // 字段列表由 Inspector 自定义区编辑，存储为 f<i>.* 索引属性）
        ComponentTypeInfo i{"ProtocolConfig", "协议配置", "通信", {180, 84}, {}};
        i.properties = {
            specEnum("framingMode", "拆帧模式", "TLV", {"TLV", "帧头+Length"}),
            specInt("tagBytes", "T字节数(TLV)", 1, 1, 4),
            specInt("lenBytes", "L字节数(TLV)", 2, 1, 4),
            specBool("bigEndian", "大端(TLV)", true),
            specBool("lenIncludesHeader", "L含帧头(TLV)", false),
            specString("headerHex", "帧头HEX", "AA 55"),
            specInt("lenOffset", "length偏移", 2, 0, 64),
            specInt("lenBytesHeader", "length字节数", 2, 1, 4),
            specBool("bigEndianHeader", "大端(length)", true),
            specBool("lenIncludesAll", "length为整帧长", false),
        };
        t.push_back(std::move(i));
    }
    { // 数据目的（通信组件：按名称关联数据源，运行器把该数据源收到的原始帧
      // 原样转发到本端点——串口设备上传 SCADA 等网关场景）
        ComponentTypeInfo i{"DataSink", "数据目的", "通信", {170, 84}, {}};
        i.properties = {
            specString("source", "关联数据源", ""), // 数据源组件名（Inspector 动态下拉）
            specEnum("transport", "传输方式", "TCP", {"UDP", "TCP", "串口", "USB", "VISA"}),
            // UDP 角色：客户端=发往 host:remotePort；服务端=绑定 localPort 发往最近对端；
            // 组播=发往 host(组地址 224~239):remotePort（发送方无需加入组）
            specEnum("udpRole", "UDP角色", "客户端", {"服务端", "客户端", "组播"}),
            specEnum("tcpRole", "TCP角色", "客户端", {"客户端", "服务端"}),
            specString("host", "主机/目标IP", "127.0.0.1"),
            specInt("remotePort", "远端端口", 9002, 1, 65535),
            specInt("localPort", "本地监听端口", 9002, 1, 65535),
            specString("serialPort", "串口", "COM1"),
            specInt("baud", "波特率", 9600, 300, 921600),
            specInt("dataBits", "数据位", 8, 5, 8),
            specEnum("parity", "校验", "无", {"无", "奇", "偶"}),
            specInt("stopBits", "停止位", 1, 1, 2),
            // USB 转发参数（仅传输=USB 时显示）：只需设备/接口/OUT 端点（转发只发不收）
            specString("usbDevice", "USB设备", ""), // 动态下拉（运行时 libusb 枚举）
            specInt("usbInterface", "USB接口号", 0, 0, 255),
            specString("usbEpOut", "OUT端点(hex)", ""), // 空 = 自动选
            // VISA 转发参数（仅传输=VISA 时显示）：viWrite 发往仪器（地址与数据源同构）
            specString("visaAddress", "VISA地址", ""),
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

} // namespace pv
