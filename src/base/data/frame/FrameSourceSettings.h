// FrameSourceSettings — 帧数据源配置：传输(TCP/UDP) + TCP 拆帧参数 + 规约字段→标签槽位映射。
// 随工程 JSON 持久化；enabled=false 时使用原 PlanView TCP 行协议数据源。
#pragma once

#include <string>
#include <vector>

#include "base/model/Component.h"
#include "base/packet/FrameCodec.h"
#include "base/packet/PacketSpec.h"  // FieldType

namespace pv {

class Project; // 前向声明（Project.h 反向包含本文件，勿加完整定义）
class Page;

// 规约字段 → 标签槽位映射
// 偏移语义随拆帧模式：
//   TLV 模式：tagId 匹配帧的 T 值，offset 相对该帧负载 V 的起点；
//   帧头+Length 模式：tagId 忽略，offset 相对整帧首。
struct TagField {
    std::string name = "字段";               // 字段名（展示用）
    std::string protoName;                   // 来源协议配置名（协议组合并后追溯字段归属）
    int tagId = 0;                           // TLV 槽位标识（帧 T 值）
    int offset = 0;                          // 字节偏移（含义见上）
    packet::FieldType type = packet::FieldType::U16; // 字段类型
    int bytes = 2;                           // 固定类型由类型决定；string/enum 由长度属性
    bool bigEndian = true;                   // 多字节字节序
    int address = 0;                         // 标签槽位（Tag::address）
    int framingIndex = 0;                    // 所属拆帧配置索引（协议组多帧头：字段归属哪条协议）
    double scale = 1.0;                      // 数值字段工程换算：工程值 = 原始值 * scale（bool/string/enum 不适用）
    std::vector<std::pair<int64_t, std::string>> enums; // Enum：值 → 名称
};

// 数据目的 → 数据源转发配置：把指定数据源收到的原始帧原样发往另一端点（网关/上传）。
// 传输参数与 FrameSourceSettings 同构；sourceName 按名称关联数据源组件。
struct FrameSinkSettings {
    std::string sourceName;          // 关联的数据源组件名（空 = 未关联）
    bool udp = false;                // 传输选择（serial 优先，其次 udp，均 false = TCP）
    bool serial = false;
    bool udpClient = false;          // 仅 UDP：客户端=发往 connect 的远端；服务端=发往最近对端
    bool udpMulticast = false;       // 仅 UDP：组播=发往 host(组地址):remotePort（无需加入组）
    bool tcpClient = true;           // 仅 TCP：客户端=连接远端；服务端=监听，向接入方转发
    std::string host = "127.0.0.1";
    int remotePort = 9002;
    int localPort = 9002;
    std::string serialPort = "COM1"; // 仅串口
    int baud = 9600;
    int dataBits = 8;
    std::string parity = "无";
    int stopBits = 1;
};

struct FrameSourceSettings {
    bool enabled = false;            // true=帧数据源；false=原 PlanView TCP 行协议
    bool autoStart = false;          // 仅帧数据源：true=PageViewer 打开即连接；false=顶栏手动启动
    bool autoSend = false;           // 自发送（模拟设备）：按关联协议格式周期性发帧（收包不受影响）
    int autoSendMs = 1000;           // 自发送周期（ms），20..60000
    bool udp = false;                // 传输选择（serial 优先，其次 udp，均 false = TCP）
    bool serial = false;             // true=串口字节流（同样走 TCP 拆帧）
    bool udpClient = false;          // 仅 UDP：true=客户端(connect 远端)；false=服务端(bind 本地)
    bool udpMulticast = false;       // 仅 UDP：true=组播（bind 本地端口 + 加入 host 组播组）
    bool tcpClient = true;           // 仅 TCP：true=客户端(connect 远端)；false=服务端(listen 本地)
    std::string host = "127.0.0.1";  // TCP 客户端 / UDP 客户端连接目标
    int remotePort = 9001;           // TCP 客户端远端端口 / UDP 客户端目标端口
    int localPort = 9001;            // TCP 服务端监听端口 / UDP 服务端本地绑定端口

    // 仅监听（固定三元组 dip/dport/协议 过滤；绑定端口即 dport，无独立本地端口）：
    //   UDP：绑定 dport，命中 = 源==(dip,dport)（反向）或 dip="*"（正向全收）
    //   TCP：监听 dport，仅接受对端 IP==dip 的连接（连接内双向都解析）
    bool listen = false;
    std::string listenIp = "*";      // dip（"*" 通配任意 IP）
    int listenPort = 9002;           // dport（UDP 反向匹配的源端口）
    bool listenTcp = false;          // 协议：false=UDP / true=TCP
    // 监听方式=镜像抓包（listenPcap）：Npcap 混杂模式抓网卡，收交换机镜像/SPAN
    // 出来的第三方流量。三元组命中 = 任一方向（正向=目的 dip:dport、反向=源
    // dip:dport，收完整会话）；UDP 数据报天然成帧，TCP 逐流拆帧（尽力而为）
    bool listenPcap = false;
    std::string listenNic;           // 抓包网卡（Npcap 设备名 \Device\NPF\...）

    // 仅串口
    std::string serialPort = "COM1"; // 串口名（"COM3" 或数字 "3"）
    int baud = 9600;                 // 波特率
    int dataBits = 8;                // 数据位 5..8
    std::string parity = "无";       // 校验：无 / 奇 / 偶
    int stopBits = 1;                // 停止位 1 / 2

    packet::FramingConfig framing;   // 主拆帧配置（首个成员/单协议）；仅 TCP 生效，UDP 天然成帧
    // 协议组多帧头：各成员的拆帧配置（帧头+Length 不同帧头时各成一条；TLV/单一配置为空）。
    // 运行时逐帧头匹配拆帧，字段按 framingIndex 只由命中帧头的协议解析
    std::vector<packet::FramingConfig> framings;

    std::vector<TagField> fields;
    std::string sourceName;                 // 生效数据源组件名（供数据目的按名称关联）
    std::vector<FrameSinkSettings> sinks;   // 数据目的（转发生效数据源的原始帧）
};

// 协议配置组件（typeId == "ProtocolConfig"）属性 -> 拆帧参数 + 规约字段。
// 字段以 f<i>.* 索引属性存储（Inspector 自定义区编辑）
void protocolFramingFromComponent(const Component& proto, packet::FramingConfig& framing,
                                  std::vector<TagField>& fields);

// 工程级合成：取首个数据源组件（传输）+ 其 protocol 属性按名称关联的协议配置组件
//（拆帧/字段）。数据源组件存在即生效（autoStart 仅控制 PageViewer 打开时是否主动连接，
// 默认关 = 手动启动）。无数据源组件时返回旧工程 settings.frame 或 disabled（走行协议）。
FrameSourceSettings frameSettingsFromProject(const Project& p);

// 组件直接绑定协议字段的属性名（"协议名/字段名" 存于组件 bindField 属性）
const char* defaultBindableProperty(const Component& c);

// 一键生成规约字段的显示组件到页面：bool→指示灯(Lamp/isOn)，其余类型（整数/浮点/
// string/enum）一律文本(Label/text)——数值精度与名称类都更适合文本展示，仪表按需手工
// 添加并绑定。组件纵向排列在协议组件下方（行距 56）；字段名由绑定组件的
// 复合卡片自显示（Label 左上角），不再单独生成名字文本。
// 同类型且已绑定同字段的组件跳过（skipped 返回跳过数）；返回新生成组件的 id 列表。
// 内部先快照协议组件数据再循环——push_back 扩容会使引用失效，不得边扩容边读协议组件。
std::vector<ComponentId> generateFieldComponents(Page& page, Project& proj,
                                                 const ComponentId& protoId, int& skipped);

// 组件 bindField（"协议名/字段名"）→ 自动合成隐式标签（按字段槽位/类型）+
// 数据绑定（组件默认属性 ← 标签），使组件直接由协议字段驱动，无需手工建标签库/关联。
// 幂等：重复调用不重复创建；字段/协议不存在时跳过（校验面板负责提示）。
void synthesizeImplicitBindings(Project& p);

} // namespace pv
