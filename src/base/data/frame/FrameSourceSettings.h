// FrameSourceSettings — 帧数据源配置：传输(TCP/UDP) + TCP 拆帧参数 + 规约字段→标签槽位映射。
// 随工程 JSON 持久化；enabled=false 时使用原 SoftG TCP 行协议数据源。
#pragma once

#include <string>
#include <vector>

#include "base/model/Component.h"
#include "base/packet/FrameCodec.h"
#include "base/packet/PacketSpec.h"  // FieldType

namespace softg {

// 规约字段 → 标签槽位映射
// 偏移语义随拆帧模式：
//   TLV 模式：tagId 匹配帧的 T 值，offset 相对该帧负载 V 的起点；
//   帧头+Length 模式：tagId 忽略，offset 相对整帧首。
struct TagField {
    std::string name = "字段";               // 字段名（展示用）
    int tagId = 0;                           // TLV 槽位标识（帧 T 值）
    int offset = 0;                          // 字节偏移（含义见上）
    packet::FieldType type = packet::FieldType::U16; // 数值类型（hex/ascii 不参与标签映射）
    int bytes = 2;                           // 固定长度类型由类型决定
    bool bigEndian = true;                   // 多字节字节序
    int address = 0;                         // 标签槽位（Tag::address）
};

struct FrameSourceSettings {
    bool enabled = false;            // true=帧数据源；false=原 SoftG TCP 行协议
    bool udp = false;                // true=UDP 数据报；false=TCP 字节流（需拆帧）
    std::string host = "127.0.0.1";  // TCP 连接目标 / UDP 发送目标
    int remotePort = 9001;           // TCP 远端端口 / UDP 发送目标端口
    int localPort = 9001;            // UDP 本地绑定端口

    packet::FramingConfig framing;   // 仅 TCP 生效；UDP 天然成帧

    std::vector<TagField> fields;
};

// 数据源组件（typeId == "DataSource"）属性 -> 帧数据源设置（运行器取首个该组件驱动 PollWorker）
FrameSourceSettings frameSettingsFromComponent(const Component& c);

} // namespace softg
